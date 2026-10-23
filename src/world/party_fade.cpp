#include "world/party_fade.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/weak_object.h"

#include <cstdint>
#include <format>
#include <string>

namespace lss::world::party_fade {

namespace {

using platform::Read;
using platform::ReadPointer;

using ApplyCameraFadeFn = void (*)(void* character, std::uint8_t enable, const std::uint64_t* reason, std::uint8_t force,
                                   std::uint8_t forceOn);
using NameFromWideFn    = std::int32_t* (*)(std::uint64_t* out, const wchar_t* text, std::int32_t findType);
using GetPlayerActorFn  = void* (*)(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate);

constexpr int           kTypes           = 8;   // the party manager's player type slots
constexpr std::uint32_t kCheckEveryTicks = 15;  // a new actor or a forced fade is put right within a quarter second
constexpr std::int32_t  kMaxReasons      = 32;  // a longer list is not a list this code reads
constexpr const wchar_t* kReasonText     = L"LocalSplitScreen_NoCameraFade";

ApplyCameraFadeFn g_apply          = nullptr;
NameFromWideFn    g_nameFromWide   = nullptr;
GetPlayerActorFn  g_getPlayerActor = nullptr;
void**            g_partyManager   = nullptr;

std::uint64_t g_reason = 0;  // the mod's reason name (made on the game thread at first use)
bool          g_wasIn  = false;
std::uint32_t g_ticks  = 0;

/// The member of each type slot the mod listed its reason on, kept as a weak pair so a destroyed actor is noticed.
std::int32_t g_listed[kTypes][2];

void ForgetAll() noexcept
{
    for (auto& weak : g_listed)
    {
        weak[0] = -1;
        weak[1] = 0;
    }
}

bool MakeReason() noexcept
{
    if (g_reason != 0)
    {
        return true;
    }
    std::int32_t* made = nullptr;
    if (!platform::CallGuarded(g_nameFromWide, made, &g_reason, kReasonText, facts::kNameAdd) || g_reason == 0)
    {
        g_reason = 0;
        platform::LogWarning(L"party fade: the reason name could not be made; party members fade as the game decides");
        return false;
    }
    return true;
}

void* ActorOfType(std::uint8_t type) noexcept
{
    void* manager = nullptr;
    void* actor   = nullptr;
    if (!Read(g_partyManager, manager) || manager == nullptr ||
        !platform::CallGuarded(g_getPlayerActor, actor, manager, std::uint8_t{0}, type, false))
    {
        return nullptr;
    }
    return actor;
}

/// The mod's reason is on the character's list.
bool Listed(void* character) noexcept
{
    const auto*   base = static_cast<const std::uint8_t*>(character);
    std::uint64_t* data = nullptr;
    std::int32_t  num  = 0;
    if (!Read(base + facts::kCharacterNoFadeReasons, data) || !Read(base + facts::kCharacterNoFadeReasonsNum, num) ||
        data == nullptr || num <= 0 || num > kMaxReasons)
    {
        return false;
    }
    for (std::int32_t i = 0; i < num; ++i)
    {
        std::uint64_t name = 0;
        if (Read(data + i, name) && name == g_reason)
        {
            return true;
        }
    }
    return false;
}

std::int32_t ReasonCount(void* character) noexcept
{
    std::int32_t num = -1;
    Read(static_cast<const std::uint8_t*>(character) + facts::kCharacterNoFadeReasonsNum, num);
    return num;
}

/// The mesh's near-fade switch: 1 on, 0 off, -1 unread.
int FadeOn(void* character) noexcept
{
    std::uint8_t on = 0;
    return Read(static_cast<const std::uint8_t*>(ReadPointer(character, facts::kCharacterMesh)) + facts::kMeshNearFarFadeEnabled, on)
               ? (on != 0 ? 1 : 0)
               : -1;
}

/// The game's own call, as the leader change makes it: enable 0 lists the reason (fade off), 1 takes it off.
bool Apply(void* character, bool list) noexcept
{
    return platform::CallGuarded(g_apply, character, static_cast<std::uint8_t>(list ? 0 : 1), &g_reason, std::uint8_t{1},
                                 std::uint8_t{0});
}

void Unlist(int type) noexcept
{
    if (void* old = players::weak_object::ObjectOf(g_listed[type]); old != nullptr)
    {
        const bool ok = Apply(old, false);
        platform::LogDebug(L"party fade: type {} {}: the mod's reason taken off (fade now {}, {} reason(s) left)", type,
                           ok ? L"released" : L"could not be released", FadeOn(old), ReasonCount(old));
    }
    g_listed[type][0] = -1;
    g_listed[type][1] = 0;
}

/// Every party member keeps the reason listed and its fade off; a member gone from its slot gets its reason back off.
void KeepAll() noexcept
{
    if (!MakeReason())
    {
        return;
    }
    int changed = 0;
    for (int type = 0; type < kTypes; ++type)
    {
        void* actor = ActorOfType(static_cast<std::uint8_t>(type));
        void* held  = players::weak_object::ObjectOf(g_listed[type]);
        if (held != nullptr && held != actor)
        {
            Unlist(type);  // the slot holds another actor now (or none)
        }
        if (actor == nullptr)
        {
            continue;
        }
        const bool listed = Listed(actor);
        const int  fadeOn = FadeOn(actor);
        if (listed && fadeOn == 0)
        {
            players::weak_object::WeakPairOf(actor, g_listed[type]);
            continue;
        }
        const bool ok = Apply(actor, true);
        players::weak_object::WeakPairOf(actor, g_listed[type]);
        ++changed;
        platform::LogDebug(L"party fade: type {}: reason {} (was listed {}, fade was {}) -> fade now {}, {} reason(s)", type,
                           ok ? L"listed" : L"could not be listed", listed, fadeOn, FadeOn(actor), ReasonCount(actor));
    }
    if (changed > 0)
    {
        platform::Log(L"party fade: {} party member(s) kept from vanishing near a camera while a further player is in", changed);
    }
}

void ReleaseAll() noexcept
{
    int released = 0;
    for (int type = 0; type < kTypes; ++type)
    {
        if (g_listed[type][0] >= 0)
        {
            Unlist(type);
            ++released;
        }
    }
    if (released > 0)
    {
        platform::Log(L"party fade: the mod's reason taken off {} party member(s); they fade near a camera as the game decides",
                      released);
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    ForgetAll();
    const auto apply   = platform::FindFunction(image, facts::kApplyCameraFade);
    const auto name    = platform::FindFunction(image, facts::kNameFromWide);
    const auto find    = platform::FindFunction(image, facts::kGetPlayerActor);
    const auto manager = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    if (!apply || !name || !find || !manager)
    {
        platform::LogWarning(L"party fade: a check failed; party members fade near a camera as the game decides");
        return false;
    }
    g_apply          = reinterpret_cast<ApplyCameraFadeFn>(*apply);
    g_nameFromWide   = reinterpret_cast<NameFromWideFn>(*name);
    g_getPlayerActor = reinterpret_cast<GetPlayerActorFn>(*find);
    g_partyManager   = reinterpret_cast<void**>(*manager);
    control::Register({.name = L"world.fade", .shape = control::Shape::Text,
                       .help = L"each party member's near-camera fade switch and whether the mod's reason keeps it off",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"party fade: installed; while a further player is in, party members do not vanish near a camera (the "
                  L"game's own switch for the player's character)");
    return true;
}

void Tick(int seatsIn) noexcept
{
    if (g_apply == nullptr)
    {
        return;
    }
    const bool anyIn   = seatsIn > 0;
    const bool changed = anyIn != g_wasIn;
    g_wasIn            = anyIn;
    if (!changed && ++g_ticks % kCheckEveryTicks != 0)
    {
        return;
    }
    if (anyIn)
    {
        KeepAll();
    }
    else if (changed)
    {
        ReleaseAll();
    }
}

std::wstring Reading() noexcept
try
{
    if (g_apply == nullptr)
    {
        return L"not installed";
    }
    std::wstring out;
    for (int type = 0; type < kTypes; ++type)
    {
        void* actor = ActorOfType(static_cast<std::uint8_t>(type));
        if (actor == nullptr)
        {
            continue;
        }
        out += std::format(L"{}type {}: fade {}, reasons {}, mod's listed {}", out.empty() ? L"" : L"; ", type,
                           FadeOn(actor), ReasonCount(actor), g_reason != 0 && Listed(actor));
    }
    return out.empty() ? L"no party actors" : out;
}
catch (...)
{
    return L"(the reading could not be formatted)";
}

}  // namespace lss::world::party_fade
