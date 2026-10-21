#include "world/npc_look_two.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"

#include <Unreal/NameTypes.hpp>

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

/// Before the game's tick of one look: player 1 if in range, else player 2 if in range, else player 1.
void Pick(std::uint8_t* lookAt) noexcept
{
    std::uint64_t name = 0, leader = 0, current = 0;
    std::int32_t  mode = 1;
    std::uint8_t  lookAway = 1;
    if (!Read(lookAt + facts::kFieldLookAtTargetName, name) || !Read(g_leaderName, leader) || name != leader ||
        name == 0 || !Read(lookAt + facts::kFieldLookAtMode, mode) || mode != 0 ||
        !Read(lookAt + facts::kFieldLookAtLookAway, lookAway) || lookAway != 0 ||
        !Read(lookAt + facts::kFieldLookAtTarget, current))
    {
        return;  // not a look at the leader: the game's alone
    }
    const std::uint64_t one     = g_one.load(std::memory_order_acquire);
    const std::uint64_t two     = g_enabled.load(std::memory_order_relaxed) ? g_two.load(std::memory_order_acquire) : 0;
    const std::uint64_t lastTwo = g_lastTwo.load(std::memory_order_acquire);
    if (one == 0 || (current != one && (current == 0 || (current != two && current != lastTwo))))
    {
        return;  // aimed at someone the mod does not know (an earlier leader, or not resolved yet): the game's alone
    }
    const std::uint64_t pick = two != 0 && !InRange(lookAt, one) && InRange(lookAt, two) ? two : one;
    if (pick == current)
    {
        return;
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
            return;
        }
        constexpr std::uint8_t kNotRequested = 0;
        platform::WriteGuarded(lookAt + facts::kFieldLookAtRequested, &kNotRequested, 1);
    }
    if (!platform::WriteGuarded(lookAt + facts::kFieldLookAtTarget, &pick, sizeof(pick)))
    {
        return;
    }
    (pick == two ? g_toTwo : g_toOne).fetch_add(1, std::memory_order_relaxed);
    std::uint64_t ownerWeak = 0;
    Read(lookAt + facts::kFieldLookAtOwner, ownerWeak);
    if (const std::uint8_t* npc = Resolve(ownerWeak); npc != nullptr)
    {
        platform::LogDebug(L"npc look: {} watches player {}", NpcName(npc), pick == two ? 2 : 1);
    }
}

char LookTick(std::uint8_t* lookAt) noexcept
{
    Pick(lookAt);
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
    return std::format(L"{}; looks turned to player 2 {}, back to player 1 {}; a requested look whose controller was "
                       L"unreadable {}",
                       g_enabled.load() ? L"on" : L"off", g_toTwo.load(), g_toOne.load(), g_noController.load());
}
catch (...)
{
    return L"?";
}

}  // namespace lss::world::npc_look_two
