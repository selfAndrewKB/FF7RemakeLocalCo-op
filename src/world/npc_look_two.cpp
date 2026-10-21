#include "world/npc_look_two.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"

#include <Unreal/NameTypes.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace lss::world::npc_look_two {

namespace {

using platform::Read;
using platform::ReadPointer;

using TickFn       = char (*)(std::uint8_t* lookAt);
using InRangeFn    = bool (*)(const std::uint8_t* lookAt, const float* location);
using ClearFn      = std::uint64_t (*)(void* controller, std::uint8_t type);
using ControllerFn = void* (*)(void* host);
using SerialFn     = std::int32_t (*)(void* objectArray, std::int32_t index);

constexpr std::uint32_t kItemBytes       = 0x18;  // FUObjectItem: Object +0, Flags +8, SerialNumber +0x10
constexpr std::uint32_t kItemFlags       = 0x08;
constexpr std::uint32_t kItemSerial      = 0x10;
constexpr std::int32_t  kUnreachableBits = 0x30000000;  // PendingKill | Unreachable: the pointer is not to be used

/// A weak pair (InternalIndex, serial) packed into one word, so a worker reads both at once. 0 = none.
std::uint64_t Pack(std::int32_t index, std::int32_t serial) noexcept
{
    return serial == 0 || index < 0 ? 0
                                    : static_cast<std::uint64_t>(static_cast<std::uint32_t>(index)) |
                                          static_cast<std::uint64_t>(static_cast<std::uint32_t>(serial)) << 32;
}

platform::CallRedirect g_tickCall;
TickFn                 g_tick       = nullptr;
InRangeFn              g_inRange    = nullptr;
ClearFn                g_clear      = nullptr;
SerialFn               g_serial     = nullptr;
void*                  g_objectArray = nullptr;
std::uint8_t**         g_objects    = nullptr;
std::int32_t*          g_objectCount = nullptr;
const std::uint64_t*   g_leaderName = nullptr;

// Written by the game thread (Tick), read by the field-action worker.
std::atomic<bool>          g_enabled{true};
std::atomic<std::uint64_t> g_one{0}, g_two{0}, g_lastTwo{0};
std::atomic<std::uint64_t> g_toTwo{0}, g_toOne{0}, g_noController{0};

/// The names that mean player 1's character: a row's target name the game itself resolved to player 1 (its registry
/// lookup by name), each with player 1's weak pair at the time, so a name learned for an earlier leader is not used.
/// Filled by the workers (claim an empty name, then the owner), emptied by the game thread when player 1 changes.
constexpr int kNameSlots = 8;
struct LearnedName
{
    std::atomic<std::uint64_t> name{0};
    std::atomic<std::uint64_t> one{0};
};
std::array<LearnedName, kNameSlots> g_oneNames{};

bool NamesPlayerOne(std::uint64_t name, std::uint64_t one) noexcept
{
    for (const auto& slot : g_oneNames)
    {
        if (slot.name.load(std::memory_order_acquire) == name)
        {
            return slot.one.load(std::memory_order_acquire) == one;
        }
    }
    return false;
}

/// Records that the game aimed a look named `name` at player 1. True the first time (the caller logs it).
bool LearnName(std::uint64_t name, std::uint64_t one) noexcept
{
    for (auto& slot : g_oneNames)
    {
        std::uint64_t held = slot.name.load(std::memory_order_acquire);
        if (held == name)
        {
            return false;
        }
        if (held == 0 && slot.name.compare_exchange_strong(held, name, std::memory_order_acq_rel))
        {
            slot.one.store(one, std::memory_order_release);
            return true;
        }
        if (held == name)
        {
            return false;  // another thread put it there first
        }
    }
    return false;  // full: further names stay the game's
}

// Game thread.
void* g_oneCharacter = nullptr;
void* g_twoCharacter = nullptr;

/// The object a packed weak pair names, if it is still that object and alive (the engine's own validity rule).
std::uint8_t* Resolve(std::uint64_t weak) noexcept
{
    const auto    index  = static_cast<std::int32_t>(weak & 0xFFFFFFFFu);
    const auto    serial = static_cast<std::int32_t>(weak >> 32);
    std::uint8_t* objects = nullptr;
    std::int32_t  count   = 0;
    if (weak == 0 || !Read(g_objects, objects) || objects == nullptr || !Read(g_objectCount, count) || index >= count)
    {
        return nullptr;
    }
    const std::uint8_t* item   = objects + static_cast<std::size_t>(index) * kItemBytes;
    std::uint8_t*       object = nullptr;
    std::int32_t        flags = 0, itemSerial = 0;
    if (!Read(item, object) || !Read(item + kItemFlags, flags) || !Read(item + kItemSerial, itemSerial) ||
        itemSerial != serial || (flags & kUnreachableBits) != 0)
    {
        return nullptr;
    }
    return object;
}

/// The game's range test of this look against a character, from its root's location as the game reads it.
bool InRange(const std::uint8_t* lookAt, std::uint64_t weak) noexcept
{
    float       location[3]{};
    const auto* root = static_cast<const std::uint8_t*>(ReadPointer(Resolve(weak), facts::kActorRootComponent));
    return root != nullptr && Read(root + facts::kComponentWorldLocation, location) && g_inRange(lookAt, location);
}

std::wstring NpcName(const std::uint8_t* npc)
{
    std::int64_t bits = 0;
    if (!Read(npc + 0x18, bits))
    {
        return L"?";
    }
    try
    {
        return RC::Unreal::FName(bits).ToString();
    }
    catch (...)
    {
        return L"?";
    }
}

/// Why a look was left to the game (the reading counts each; a look's first sighting logs its fields once).
enum Outcome : int
{
    kUnreadable,     // a field could not be read
    kOtherName,      // the row names someone other than `Leader` or player 1's character
    kPlace,          // a look at a place
    kLookAway,       // a look away from the target
    kNoPlayers,      // player 1's character not published yet
    kUnknownTarget,  // aimed at someone the mod does not know (an earlier leader, or not resolved yet)
    kKept,           // the pick is the current target
    kTurned,         // the mod wrote a new target
    kOutcomes
};
constexpr const wchar_t* kOutcomeNames[kOutcomes] = {L"unreadable", L"aimed by name at another", L"at a place",
                                                     L"looking away", L"no players yet", L"aimed at someone unknown",
                                                     L"kept", L"turned"};
std::array<std::atomic<std::uint64_t>, kOutcomes> g_outcomes{};

/// The looks seen so far (their owners' weak pairs), each logged once with its fields. Lock-free insert.
constexpr int kSeenCap = 256;
std::array<std::atomic<std::uint64_t>, kSeenCap> g_seen{};
std::atomic<int> g_seenCount{0};

bool FirstSighting(std::uint64_t ownerWeak) noexcept
{
    if (ownerWeak == 0)
    {
        return false;
    }
    for (auto& slot : g_seen)
    {
        std::uint64_t held = slot.load(std::memory_order_acquire);
        if (held == ownerWeak)
        {
            return false;
        }
        if (held == 0 && slot.compare_exchange_strong(held, ownerWeak, std::memory_order_acq_rel))
        {
            g_seenCount.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (held == ownerWeak)
        {
            return false;  // another thread put it there first
        }
    }
    return false;  // full: the rest are counted only
}

std::wstring NameText(std::uint64_t bits)
{
    try
    {
        return RC::Unreal::FName(static_cast<std::int64_t>(bits)).ToString();
    }
    catch (...)
    {
        return L"?";
    }
}

const wchar_t* Who(std::uint64_t weak, std::uint64_t one, std::uint64_t two) noexcept
{
    return weak == 0 ? L"none" : weak == one ? L"player 1" : weak == two ? L"player 2" : L"another";
}

/// Before the game's tick of one look: player 1 if in range, else player 2 if in range, else player 1.
Outcome Choose(std::uint8_t* lookAt) noexcept
{
    std::uint64_t name = 0, leader = 0, current = 0;
    std::int32_t  mode = 1;
    std::uint8_t  lookAway = 1;
    if (!Read(lookAt + facts::kFieldLookAtTargetName, name) || !Read(g_leaderName, leader) ||
        !Read(lookAt + facts::kFieldLookAtMode, mode) || !Read(lookAt + facts::kFieldLookAtLookAway, lookAway) ||
        !Read(lookAt + facts::kFieldLookAtTarget, current))
    {
        return kUnreadable;
    }
    const std::uint64_t one     = g_one.load(std::memory_order_acquire);
    const std::uint64_t two     = g_enabled.load(std::memory_order_relaxed) ? g_two.load(std::memory_order_acquire) : 0;
    const std::uint64_t lastTwo = g_lastTwo.load(std::memory_order_acquire);
    if (std::uint64_t ownerWeak = 0; Read(lookAt + facts::kFieldLookAtOwner, ownerWeak) && FirstSighting(ownerWeak))
    {
        float reach = 0.0f;
        Read(lookAt + facts::kFieldLookAtReach, reach);
        const std::uint8_t* npc = Resolve(ownerWeak);
        platform::LogDebug(L"npc look: {} has a field look: target name {}{}, mode {}, look away {}, reach {:.0f}, aimed "
                           L"at {}; player 1 in reach {}, player 2 in reach {}",
                           npc != nullptr ? NpcName(npc) : L"?", NameText(name), name == leader ? L" (Leader)" : L"",
                           mode, lookAway, reach, Who(current, one, two), one != 0 && InRange(lookAt, one) ? L"yes" : L"no",
                           two != 0 && InRange(lookAt, two) ? L"yes" : L"no");
    }
    if (name != 0 && name != leader && mode == 0 && one != 0 && current == one && LearnName(name, one))
    {
        // Rows name the character (`Cloud`), and the game resolves the name through its character registry. A look the
        // game itself aimed at player 1 by that name is a look at the player, as `Leader` is.
        platform::LogDebug(L"npc look: the target name {} is player 1's character (the game aimed a look by it at "
                           L"player 1); looks by that name watch either player", NameText(name));
    }
    if (name == 0 || (name != leader && !NamesPlayerOne(name, one)))
    {
        return kOtherName;
    }
    if (mode != 0)
    {
        return kPlace;
    }
    if (lookAway != 0)
    {
        return kLookAway;
    }
    if (one == 0)
    {
        return kNoPlayers;
    }
    if (current != one && (current == 0 || (current != two && current != lastTwo)))
    {
        return kUnknownTarget;
    }
    const std::uint64_t pick = two != 0 && !InRange(lookAt, one) && InRange(lookAt, two) ? two : one;
    if (pick == current)
    {
        return kKept;
    }
    std::uint8_t requested = 0;
    if (Read(lookAt + facts::kFieldLookAtRequested, requested) && requested != 0)
    {
        // The game requests the look once on entering range; the old request goes first, with the game's own clear.
        std::uint64_t ownerWeak = 0;
        Read(lookAt + facts::kFieldLookAtOwner, ownerWeak);
        std::uint8_t* const owner = Resolve(ownerWeak);
        auto* const   host  = owner != nullptr ? owner + facts::kCharacterLookAtHost : nullptr;
        auto* const   table = static_cast<std::uint8_t*>(ReadPointer(host, 0));
        ControllerFn  get   = nullptr;
        void*         controller = nullptr;
        std::uint64_t cleared = 0;
        if (table == nullptr || !Read(table + facts::kLookAtHostController, get) || get == nullptr ||
            !platform::CallGuarded(get, controller, static_cast<void*>(host)) || controller == nullptr ||
            !platform::CallGuarded(g_clear, cleared, controller, facts::kLookAtTypeAi))
        {
            g_noController.fetch_add(1, std::memory_order_relaxed);
            return kUnreadable;
        }
        constexpr std::uint8_t kNotRequested = 0;
        platform::WriteGuarded(lookAt + facts::kFieldLookAtRequested, &kNotRequested, 1);
    }
    if (!platform::WriteGuarded(lookAt + facts::kFieldLookAtTarget, &pick, sizeof(pick)))
    {
        return kUnreadable;
    }
    (pick == two ? g_toTwo : g_toOne).fetch_add(1, std::memory_order_relaxed);
    std::uint64_t ownerWeak = 0;
    Read(lookAt + facts::kFieldLookAtOwner, ownerWeak);
    if (const std::uint8_t* npc = Resolve(ownerWeak); npc != nullptr)
    {
        platform::LogDebug(L"npc look: {} watches player {}", NpcName(npc), pick == two ? 2 : 1);
    }
    return kTurned;
}

char LookTick(std::uint8_t* lookAt) noexcept
{
    g_outcomes[static_cast<std::size_t>(Choose(lookAt))].fetch_add(1, std::memory_order_relaxed);
    return g_tick(lookAt);
}

/// A character's weak pair, the way the game builds one (InternalIndex plus the allocated serial). Game thread.
std::uint64_t WeakOf(void* character) noexcept
{
    std::int32_t index = -1, serial = 0;
    if (character == nullptr || !Read(static_cast<std::uint8_t*>(character) + facts::kObjectIndex, index) || index < 0 ||
        !platform::CallGuarded(g_serial, serial, g_objectArray, index))
    {
        return 0;
    }
    return Pack(index, serial);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto site    = image.Rebase(facts::kFieldLookAtTickCall, 5);
    const auto tick    = platform::FindFunction(image, facts::kFieldLookAtTick);
    const auto inRange = platform::FindFunction(image, facts::kFieldLookAtInRange);
    const auto clear   = platform::FindFunction(image, facts::kLookAtClear);
    const auto serial  = platform::FindFunction(image, facts::kAllocateSerialNumber);
    const auto array   = image.Rebase(facts::kObjectArray, sizeof(void*));
    const auto objects = image.Rebase(facts::kObjectArrayObjects, sizeof(void*));
    const auto count   = image.Rebase(facts::kObjectArrayCount, sizeof(std::int32_t));
    const auto leader  = image.Rebase(facts::kNameLeaderGlobal, sizeof(std::uint64_t));
    if (!site || !tick || !inRange || !clear || !serial || !array || !objects || !count || !leader)
    {
        platform::LogWarning(L"npc look: the field look-at code is not what the research read; NPCs that watch the "
                             L"player watch player 1 only");
        return false;
    }
    g_tick        = reinterpret_cast<TickFn>(*tick);
    g_inRange     = reinterpret_cast<InRangeFn>(*inRange);
    g_clear       = reinterpret_cast<ClearFn>(*clear);
    g_serial      = reinterpret_cast<SerialFn>(*serial);
    g_objectArray = reinterpret_cast<void*>(*array);
    g_objects     = reinterpret_cast<std::uint8_t**>(*objects);
    g_objectCount = reinterpret_cast<std::int32_t*>(*count);
    g_leaderName  = reinterpret_cast<const std::uint64_t*>(*leader);
    if (!g_tickCall.Install(*site, *tick, reinterpret_cast<std::uintptr_t>(&LookTick)))
    {
        platform::LogWarning(L"npc look: the look's tick could not be redirected; NPCs that watch the player watch player 1 only");
        return false;
    }
    control::Register({.name  = L"npc-look",
                       .shape = control::Shape::OnOff,
                       .help  = L"NPCs that watch the player watch player 2 when player 1 is out of their reach "
                                L"(world/npc_look_two.h; on)",
                       .set   = [](const control::Value& value) {
                           SetEnabled(std::get<bool>(value));
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled.load()}; }});
    control::Register({.name = L"world.npc-look", .shape = control::Shape::Text,
                       .help = L"the field looks turned to player 2 and back to player 1",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"npc look: installed; NPCs that watch the player watch player 2 when player 1 is out of their reach");
    return true;
}

void Tick(void* memberOne, void* memberTwo) noexcept
{
    if (memberOne != g_oneCharacter)
    {
        g_oneCharacter = memberOne;
        g_one.store(WeakOf(memberOne), std::memory_order_release);
        for (auto& slot : g_oneNames)
        {
            slot.one.store(0, std::memory_order_release);  // learned again under the new player 1
            slot.name.store(0, std::memory_order_release);
        }
    }
    if (memberTwo != g_twoCharacter)
    {
        if (g_twoCharacter != nullptr)
        {
            g_lastTwo.store(g_two.load(), std::memory_order_release);  // its looks go back to player 1
        }
        g_twoCharacter = memberTwo;
        const std::uint64_t two = WeakOf(memberTwo);
        g_two.store(two, std::memory_order_release);
        if (two != 0)
        {
            g_lastTwo.store(two, std::memory_order_release);
        }
    }
}

void SetEnabled(bool on) noexcept
{
    g_enabled = on;
    platform::Log(L"npc look {}", on ? L"on" : L"off (the looks the mod turned go back to player 1)");
}

std::wstring Reading() noexcept
try
{
    std::wstring names;
    for (const auto& slot : g_oneNames)
    {
        if (const std::uint64_t name = slot.name.load(); name != 0)
        {
            names += (names.empty() ? L"" : L" ") + NameText(name);
        }
    }
    std::wstring ticks;
    for (int i = 0; i < kOutcomes; ++i)
    {
        ticks += std::format(L"{}{} {}", ticks.empty() ? L"" : L", ", kOutcomeNames[i],
                             g_outcomes[static_cast<std::size_t>(i)].load());
    }
    return std::format(L"{}; looks turned to player 2 {}, back to player 1 {}; a requested look whose controller was "
                       L"unreadable {}; NPC looks seen {}; names that mean player 1 {}; look ticks: {}",
                       g_enabled.load() ? L"on" : L"off", g_toTwo.load(), g_toOne.load(), g_noController.load(),
                       g_seenCount.load(), names.empty() ? L"none" : names, ticks);
}
catch (...)
{
    return L"?";
}

}  // namespace lss::world::npc_look_two
