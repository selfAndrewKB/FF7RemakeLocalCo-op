#include "views/mirror_player.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/game_state.h"
#include "players/local_player_list.h"
#include "players/own_controller.h"
#include "players/split_render.h"
#include "views/column_rules.h"
#include "views/scene_camera.h"

#include <algorithm>
#include <array>
#include <format>

namespace lss::views::mirror_player {

namespace {

using RC::Unreal::UObject;
namespace list = players::local_player_list;

constexpr std::int32_t  kMaxColumns   = column_rules::kMaxColumns;
constexpr std::int32_t  kMirrorsBuilt = kMaxColumns - 1;  // every column but player 1's may be a mirror's
constexpr std::uint32_t kStatusTicks  = 300;
constexpr std::uint32_t kRetryTicks   = 30;   // a refused add or removal tried again twice a second at 60
constexpr std::uint8_t  kStatePlay    = 6;    // the game state and the field step of a steady field (as the session's)
constexpr std::int32_t  kStepSteady   = 13;
static_assert(kMirrorsBuilt == scene_camera::kMirrorSlots, "one camera copy record per mirror");

// Set at install, then only read.
std::uintptr_t g_removeLocalPlayer = 0;

/// One mirror: its engine objects and the id it was created with. Its column is its place in the engine's list,
/// read each tick (a leave before it moves it up); its controller and camera manager are re-read with it.
struct Mirror
{
    UObject*     player       = nullptr;  // the ULocalPlayer this module added
    UObject*     controller   = nullptr;
    UObject*     manager      = nullptr;
    std::int32_t controllerId = 0;
    std::int32_t column       = 0;  // its index in the engine's list at the last tick
};

// Game thread.
bool                              g_enabled      = true;
std::array<Mirror, kMirrorsBuilt> g_mirrors;         // in the order added; the first `g_count` are in
std::int32_t                      g_count        = 0;
UObject*                          g_instance     = nullptr;  // the game instance, found once (it lives as long as the game)
UObject*                          g_managerOne   = nullptr;  // player 1's camera manager, every mirror's source
std::int32_t                      g_wanted       = 0;
std::int32_t                      g_columns      = 0;        // the columns shown at the last tick
std::int32_t                      g_columnsAlone = 0;        // the columns with player 1 alone (what a leave fills)
std::uint32_t                     g_seatIds      = 1;        // bit i: controller id i is a player's (bit 0 player 1)
std::uint32_t                     g_leaveFills   = 0;        // adds made the tick a further player left for a world change
std::uint32_t                     g_ticks        = 0;
std::uint32_t                     g_adds         = 0;
std::uint32_t                     g_removes      = 0;
std::uint32_t                     g_dropped      = 0;
std::uint32_t                     g_retryAt      = 0;        // no add or removal before this tick (after a refusal)
const wchar_t*                    g_waiting      = nullptr;  // why the add waits, logged once per reason
const wchar_t*                    g_fault        = nullptr;  // a refused add or removal, logged once until one succeeds

UObject* Field(const void* object, std::uint32_t offset) noexcept
{
    return static_cast<UObject*>(platform::ReadPointer(object, offset));
}

const wchar_t* YesNo(bool value) noexcept
{
    return value ? L"yes" : L"no";
}

std::uintptr_t Address(const void* object) noexcept
{
    return reinterpret_cast<std::uintptr_t>(object);
}

void Fault(const wchar_t* why) noexcept
{
    g_retryAt = g_ticks + kRetryTicks;
    if (g_fault != why)
    {
        g_fault = why;
        platform::Log(L"mirror player: {}; tried again twice a second", why);
    }
}

/// The game instance, found once (UE4SS's object walk is slow; the session keeps its own the same way).
UObject* Instance() noexcept
{
    if (g_instance == nullptr)
    {
        g_instance = list::GameInstance();
    }
    return g_instance;
}

/// Each mirror's camera copy told to the scene camera module, by the order added.
void TellSceneCamera() noexcept
{
    for (std::int32_t slot = 0; slot < kMirrorsBuilt; ++slot)
    {
        const Mirror& m = g_mirrors[slot];
        scene_camera::SetMirror(slot, slot < g_count ? m.controller : nullptr, slot < g_count ? m.manager : nullptr,
                                    slot < g_count ? g_managerOne : nullptr);
    }
}

/// Drops mirror `slot` from the record (the engine no longer lists it, or it was removed), closing the gap.
void ForgetSlot(std::int32_t slot) noexcept
{
    for (std::int32_t i = slot; i + 1 < g_count; ++i)
    {
        g_mirrors[i] = g_mirrors[i + 1];
    }
    --g_count;
    g_mirrors[g_count] = {};
    if (g_count == 0)
    {
        g_managerOne = nullptr;
    }
    TellSceneCamera();
}

/// The lowest controller id in 1..kMaxColumns-1 that no player (the session's ids) and no mirror holds; -1 when
/// every id is taken.
std::int32_t FreeControllerId() noexcept
{
    for (std::int32_t id = 1; id < kMaxColumns; ++id)
    {
        bool used = (g_seatIds >> id) & 1u;
        for (std::int32_t i = 0; i < g_count; ++i)
        {
            used = used || g_mirrors[i].controllerId == id;
        }
        if (!used)
        {
            return id;
        }
    }
    return -1;
}

/// Why a mirror cannot be added now, or null with player 1's controller filled.
/// `atLeave`: the tick a further player leaves for a world change. The field is leaving, not steady, and still running:
/// the mirror added then holds the column through the loading screen, where a steady-field add came too late (the
/// user, 2026-10-22 10:52: one view stretched over both monitors at a save load with player 2 in, mirrored on the
/// next load because the mirror from the first was still in).
const wchar_t* Refusal(std::int32_t column, UObject*& playerOneController, bool atLeave) noexcept
{
    const auto game = players::game_state::Current();
    if (atLeave ? !(game.valid && game.hasRoot && game.running)
                : !(game.valid && game.hasRoot && game.running && game.step == kStepSteady && game.state == kStatePlay))
    {
        return atLeave ? L"the field is already down" : L"the field is not steady (a load, or a scene change)";
    }
    UObject* instance = Instance();
    if (instance == nullptr)
    {
        return L"the game instance was not found";
    }
    if (list::Count(instance) != column)
    {
        return L"the local player count is not the mirror's column";
    }
    playerOneController = Field(list::At(instance, 0), facts::kLocalPlayerController);
    if (playerOneController == nullptr)
    {
        return L"player 1 has no controller yet";
    }
    if (!players::own_controller::UsesNativeClasses(playerOneController))
    {
        return L"player 1's controller or camera manager is not the End class the research read";
    }
    if (!players::own_controller::CanRename())
    {
        return L"the camera actor's name text cannot be changed";
    }
    return nullptr;
}

void RemoveSlot(std::int32_t slot, const wchar_t* why) noexcept;

/// A mirror into the next empty column (`column` = the local players listed now): CreatePlayer with a free
/// controller id, the views filled, the split effects on, its camera manager handed to the slot copy.
bool Add(std::int32_t column, bool atLeave) noexcept
{
    if (g_ticks < g_retryAt)
    {
        return false;
    }
    UObject* playerOne = nullptr;
    if (const wchar_t* why = Refusal(column, playerOne, atLeave); why != nullptr)
    {
        if (g_waiting != why)
        {
            g_waiting = why;
            platform::LogDebug(L"mirror player: waiting to fill column {}: {}", column + 1, why);
        }
        return false;
    }
    g_waiting = nullptr;
    const std::int32_t id = FreeControllerId();
    if (id < 0 || g_count >= kMirrorsBuilt)
    {
        Fault(L"no controller id is free for a further column (every id is a player's or a mirror's)");
        return false;
    }
    const list::Added added = list::Add(g_instance, playerOne, id);
    platform::Log(L"mirror player: {}", added.Text(L"the mirror's"));
    if (added.localPlayer == nullptr)
    {
        Fault(L"the engine did not add a local player for the mirror");
        return false;
    }
    Mirror& m      = g_mirrors[g_count++];
    m.player       = added.localPlayer;
    m.controller   = added.controller;
    m.manager      = added.cameraManager;
    m.controllerId = id;
    m.column       = column;
    g_managerOne   = Field(playerOne, facts::kControllerCameraManager);
    ++g_adds;
    if (!added.called)
    {
        platform::Log(L"mirror player: the engine raised an exception while creating the mirror; removing it");
        RemoveSlot(g_count - 1, L"the create raised");
        Fault(L"the create raised an exception");
        return false;
    }
    g_fault = nullptr;
    // Each view fills its column (the join does the same for both halves): the End camera actor constrains to 16:9.
    const bool filledOne    = list::FillView(g_managerOne);
    const bool filledMirror = list::FillView(m.manager);
    players::split_render::OnJoin();  // further views: the effects the split rule clears, back on
    TellSceneCamera();
    platform::Log(L"mirror player: in column {} (controller id {}, controller {:#x}, camera manager {:#x}, player 1's {:#x}; views "
                  L"filled: player 1 {}, the mirror {}); its camera manager takes every slot of player 1's before each update{}",
                  column + 1, id, Address(m.controller), Address(m.manager), Address(g_managerOne), YesNo(filledOne),
                  YesNo(filledMirror),
                  m.controller == nullptr || m.manager == nullptr ? L" (its controller or manager is not set yet: read again each tick)" : L"");
    return true;
}

void RemoveSlot(std::int32_t slot, const wchar_t* why) noexcept
{
    if (slot < 0 || slot >= g_count)
    {
        return;
    }
    const Mirror m = g_mirrors[slot];
    if (!list::Listed(g_instance, m.player))
    {
        platform::Log(L"mirror player: {}: the engine no longer lists the mirror of column {}; letting go without touching it", why,
                      m.column + 1);
        ForgetSlot(slot);
        return;
    }
    const auto leftovers = list::ReadLeftovers(m.player);
    bool       raised    = false;
    const bool removed   = list::Remove(g_instance, m.player, g_removeLocalPlayer, raised);
    if (!removed)
    {
        // Still in: its camera copy goes on untouched (the record is not cleared before a removal succeeds).
        Fault(raised ? L"the engine did NOT remove a mirror (the call raised an exception); kept"
                     : L"the engine did NOT remove a mirror; kept");
        return;
    }
    const auto gone = list::DestroyLeftovers(leftovers);
    ++g_removes;
    g_fault = nullptr;
    platform::Log(L"mirror player: {}: the mirror left column {} (controller id {}){}; leftovers destroyed: camera actor {}, "
                  L"battle-talk actor {} ({})",
                  why, m.column + 1, m.controllerId, raised ? L" (the call raised an exception after removing it)" : L"",
                  YesNo(gone.camera), YesNo(gone.talk), leftovers.Text());
    ForgetSlot(slot);
}

/// The mirrors as the engine lists them now: each one's column, controller and camera manager re-read from the
/// list (a rebuilt controller would leave stale pointers), player 1's manager from index 0; a mirror the engine
/// dropped is forgotten (logged). Skipped while the list cannot be read.
void FollowList() noexcept
{
    if (g_count == 0)
    {
        return;
    }
    const std::int32_t listed = list::Count(g_instance);
    if (listed < 0)
    {
        return;
    }
    bool changed = false;
    for (std::int32_t slot = 0; slot < g_count;)
    {
        Mirror& m     = g_mirrors[slot];
        bool    found = false;
        for (std::int32_t i = 0; i < listed && !found; ++i)
        {
            if (list::At(g_instance, i) == m.player)
            {
                found    = true;
                m.column = i;
                UObject* controller = Field(m.player, facts::kLocalPlayerController);
                UObject* manager    = Field(controller, facts::kControllerCameraManager);
                if (controller != m.controller || manager != m.manager)
                {
                    platform::Log(L"mirror player: the mirror of column {} has a new controller or camera manager ({:#x}, {:#x}); followed",
                                  i + 1, Address(controller), Address(manager));
                    m.controller = controller;
                    m.manager    = manager;
                    changed      = true;
                }
            }
        }
        if (!found)
        {
            ++g_dropped;
            platform::Log(L"mirror player: the engine dropped the mirror of column {} by itself (a world change?); it is added "
                          L"again once the field is steady",
                          m.column + 1);
            ForgetSlot(slot);
            continue;
        }
        ++slot;
    }
    if (g_count != 0)
    {
        UObject* managerOne = Field(Field(list::At(g_instance, 0), facts::kLocalPlayerController), facts::kControllerCameraManager);
        if (managerOne != nullptr && managerOne != g_managerOne)
        {
            platform::Log(L"mirror player: player 1's camera manager is {:#x} now; the mirrors copy from it", Address(managerOne));
            g_managerOne = managerOne;
            changed      = true;
        }
    }
    if (changed)
    {
        TellSceneCamera();
    }
}

/// Adds mirrors up to `wanted` (`atLeave`: all at once, for the loading screen; else one per tick). `playersIn`
/// the real players listed: the next mirror's column is after them and the mirrors in.
void Fill(std::int32_t playersIn, std::int32_t wanted, bool atLeave) noexcept
{
    while (g_count < wanted)
    {
        const std::int32_t column = playersIn + g_count;
        if (!Add(column, atLeave) || !atLeave)
        {
            break;
        }
        ++g_leaveFills;
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto remove = platform::FindFunction(image, facts::kRemoveLocalPlayer);
    if (!remove)
    {
        platform::Log(L"mirror player: RemoveLocalPlayer differs from the research; the mirror is unavailable");
        return false;
    }
    g_removeLocalPlayer = *remove;
    using control::Register;
    using control::Shape;
    using control::Value;
    Register({.name  = L"mirror-player",
              .shape = Shape::OnOff,
              .help  = L"an extra local player in every column no player fills (the span's columns, or view-columns) shows "
                       L"player 1's picture (the title, loading screens, the field, menus); off: the columns go to the players in",
              .set   = [](const Value& v) {
                  g_enabled = std::get<bool>(v);
                  if (!g_enabled)
                  {
                      Clear(L"switched off");
                  }
                  platform::Log(L"mirror player: {}", g_enabled ? L"on (fills an empty column once the field is steady)" : L"off");
                  return control::Outcome{};
              },
              .get = [] { return Value{g_enabled}; }});
    Register({.name = L"mirror-player.state", .shape = Shape::Text,
              .help = L"the mirror players: wanted, in which columns with which controller ids, their camera copies, why one waits",
              .get  = [] { return Value{Reading()}; }});
    platform::Log(L"mirror player: ready (on: a mirror fills every column no player has, up to {} columns; mirror-player on|off)",
                  kMaxColumns);
    return true;
}

void Tick(std::int32_t playersIn, std::uint32_t seatIds, std::int32_t columns, std::int32_t columnsAlone) noexcept
{
    if (g_removeLocalPlayer == 0)
    {
        return;
    }
    ++g_ticks;
    g_seatIds      = seatIds | 1u;
    g_columns      = columns;
    g_columnsAlone = columnsAlone;
    FollowList();
    g_wanted = g_enabled ? std::clamp(columns - playersIn, 0, kMirrorsBuilt) : 0;
    while (g_count > g_wanted && g_ticks >= g_retryAt)
    {
        const std::int32_t before = g_count;
        RemoveSlot(g_count - 1, !g_enabled ? L"switched off" : L"its column is a player's, or no longer shown");
        if (g_count == before)
        {
            break;  // the engine kept it; tried again after the retry wait
        }
    }
    if (g_count < g_wanted)
    {
        Fill(playersIn, g_wanted, false);
    }
    else
    {
        g_waiting = nullptr;
    }
    if (g_count != 0 && g_ticks % kStatusTicks == 0 && platform::Enabled(platform::Level::Debug))
    {
        platform::LogDebug(L"mirror player: {}", Reading());
    }
}

void FillAtLeave(const wchar_t* why, std::int32_t playersIn, std::uint32_t seatIds) noexcept
{
    if (g_removeLocalPlayer == 0 || !g_enabled)
    {
        return;
    }
    g_seatIds = seatIds | 1u;
    FollowList();
    // The columns with player 1 alone: a plain split (no span, no view-columns) fills nothing at a leave.
    const std::int32_t wanted = std::clamp(g_columnsAlone - playersIn, 0, kMirrorsBuilt);
    const std::int32_t before = g_count;
    if (g_count >= wanted)
    {
        return;
    }
    g_retryAt = 0;  // the leave's fill is not held back by an earlier refusal
    Fill(playersIn, wanted, true);
    if (g_count > before)
    {
        platform::Log(L"mirror player: {} column(s) filled as {} (before the loading screen)", g_count - before, why);
    }
    else
    {
        platform::Log(L"mirror player: no column filled as {}: {}; they are filled once the field is steady", why,
                      g_waiting != nullptr ? g_waiting : g_fault != nullptr ? g_fault : L"the add failed (see above)");
    }
}

bool InColumn(std::int32_t column) noexcept
{
    for (std::int32_t i = 0; i < g_count; ++i)
    {
        if (g_mirrors[i].column == column)
        {
            return true;
        }
    }
    return false;
}

void Clear(const wchar_t* why) noexcept
{
    FollowList();
    while (g_count > 0)
    {
        const std::int32_t before = g_count;
        RemoveSlot(g_count - 1, why);
        if (g_count == before)
        {
            break;
        }
    }
}

std::int32_t Count() noexcept
{
    return g_count;
}

std::wstring Reading()
{
    std::wstring in;
    for (std::int32_t i = 0; i < g_count; ++i)
    {
        in += std::format(L"{}column {} (controller id {})", i ? L", " : L"", g_mirrors[i].column + 1, g_mirrors[i].controllerId);
    }
    return std::format(L"{}; wanted {} of {} column(s); {}; adds {} ({} at a further player's leave for a world change), removes {}, dropped by "
                       L"the engine {}{}{}; camera copy: {}",
                       g_enabled ? L"on" : L"off", g_wanted, g_columns, g_count != 0 ? std::format(L"in {}", in) : L"out", g_adds,
                       g_leaveFills, g_removes, g_dropped, g_waiting != nullptr ? std::format(L"; waiting: {}", g_waiting) : L"",
                       g_fault != nullptr ? std::format(L"; refused: {}", g_fault) : L"", scene_camera::MirrorPlayerText());
}

}  // namespace lss::views::mirror_player
