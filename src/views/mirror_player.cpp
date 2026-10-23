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
#include "views/scene_camera_two.h"

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
constexpr std::uint8_t  kStatePlay    = 6;   // the game state and the field step of a steady field (as the session's)
constexpr std::int32_t  kStepSteady   = 13;

// Set at install, then only read.
std::uintptr_t g_removeLocalPlayer = 0;

/// One mirror: its engine objects and the id it was created with. Its column is its place in the engine's list,
/// read each tick (a leave before it moves it up).
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
UObject*                          g_gameInstance = nullptr;  // the instance the mirrors were added to
UObject*                          g_managerOne   = nullptr;  // player 1's camera manager, every mirror's source
std::int32_t                      g_wanted       = 0;
std::int32_t                      g_columns      = 0;  // the columns shown at the last tick
std::uint32_t                     g_leaveFills   = 0;  // adds made the tick player 2 left for a world change
std::uint32_t                     g_ticks        = 0;
std::uint32_t                     g_adds         = 0;
std::uint32_t                     g_removes      = 0;
std::uint32_t                     g_dropped      = 0;
const wchar_t*                    g_waiting      = nullptr;  // why the add waits, logged once per reason

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

/// Each mirror's camera copy told to the scene camera module, by the order added.
void TellSceneCamera() noexcept
{
    for (std::int32_t slot = 0; slot < kMirrorsBuilt; ++slot)
    {
        const Mirror& m = g_mirrors[slot];
        scene_camera_two::SetMirror(slot, slot < g_count ? m.controller : nullptr, slot < g_count ? m.manager : nullptr,
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
        g_gameInstance = nullptr;
        g_managerOne   = nullptr;
    }
    TellSceneCamera();
}

/// The lowest controller id in 1..kMaxColumns-1 that no player in (ids 1..playersIn-1, the seats in order) and no
/// mirror holds; -1 when every id is taken.
std::int32_t FreeControllerId(std::int32_t playersIn) noexcept
{
    for (std::int32_t id = std::max(playersIn, 1); id < kMaxColumns; ++id)
    {
        bool used = false;
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

/// Why a mirror cannot be added now, or null with player 1's controller and the instance filled.
/// `atLeave`: the tick player 2 leaves for a world change. The field is leaving, not steady, and still running:
/// the mirror added then holds the column through the loading screen, where a steady-field add came too late (the
/// user, 2026-10-22 10:52: one view stretched over both monitors at a save load with player 2 in, mirrored on the
/// next load because the mirror from the first was still in).
const wchar_t* Refusal(std::int32_t column, UObject*& playerOneController, UObject*& gameInstance, bool atLeave) noexcept
{
    const auto game = players::game_state::Current();
    if (atLeave ? !(game.valid && game.hasRoot && game.running)
                : !(game.valid && game.hasRoot && game.running && game.step == kStepSteady && game.state == kStatePlay))
    {
        return atLeave ? L"the field is already down" : L"the field is not steady (a load, or a scene change)";
    }
    gameInstance = list::GameInstance();
    if (list::Count(gameInstance) != column)
    {
        return L"the local player count is not the mirror's column";
    }
    if (g_count != 0 && gameInstance != g_gameInstance)
    {
        return L"the game instance is not the one the other mirror was added to";
    }
    playerOneController = Field(list::At(gameInstance, 0), facts::kLocalPlayerController);
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
bool Add(std::int32_t playersIn, std::int32_t column, bool atLeave) noexcept
{
    UObject* playerOne    = nullptr;
    UObject* gameInstance = nullptr;
    if (const wchar_t* why = Refusal(column, playerOne, gameInstance, atLeave); why != nullptr)
    {
        if (g_waiting != why)
        {
            g_waiting = why;
            platform::LogDebug(L"mirror player: waiting to fill column {}: {}", column + 1, why);
        }
        return false;
    }
    g_waiting = nullptr;
    const std::int32_t id = FreeControllerId(playersIn);
    if (id < 0 || g_count >= kMirrorsBuilt)
    {
        platform::Log(L"mirror player: no controller id is free for column {} ({} mirror(s) in, {} player(s))", column + 1, g_count,
                      playersIn);
        return false;
    }
    const list::Added added = list::Add(playerOne, id);
    platform::Log(L"mirror player: {}", added.Text(L"the mirror's"));
    if (added.localPlayer == nullptr)
    {
        platform::Log(L"mirror player: the engine did not add a local player (count now {})", list::Count(gameInstance));
        return false;
    }
    Mirror& m      = g_mirrors[g_count++];
    m.player       = added.localPlayer;
    m.controller   = added.controller;
    m.manager      = added.cameraManager;
    m.controllerId = id;
    m.column       = column;
    g_gameInstance = gameInstance;
    g_managerOne   = Field(playerOne, facts::kControllerCameraManager);
    ++g_adds;
    if (!added.called)
    {
        platform::Log(L"mirror player: the engine raised an exception while creating the mirror; removing it");
        RemoveSlot(g_count - 1, L"the create raised");
        return false;
    }
    // Each view fills its column (the join does the same for both halves): the End camera actor constrains to 16:9.
    const bool filledOne    = list::FillView(g_managerOne);
    const bool filledMirror = list::FillView(m.manager);
    players::split_render::OnJoin();  // further views: the effects the split rule clears, back on
    TellSceneCamera();
    platform::Log(L"mirror player: in column {} (controller id {}, controller {:#x}, camera manager {:#x}, player 1's {:#x}; views "
                  L"filled: player 1 {}, the mirror {}); its camera manager takes every slot of player 1's before each update",
                  column + 1, id, Address(m.controller), Address(m.manager), Address(g_managerOne), YesNo(filledOne),
                  YesNo(filledMirror));
    return true;
}

void RemoveSlot(std::int32_t slot, const wchar_t* why) noexcept
{
    if (slot < 0 || slot >= g_count)
    {
        return;
    }
    const Mirror m = g_mirrors[slot];
    if (!list::Listed(g_gameInstance, m.player))
    {
        platform::Log(L"mirror player: {}: the engine no longer lists the mirror of column {}; letting go without touching it", why,
                      m.column + 1);
        ForgetSlot(slot);
        return;
    }
    scene_camera_two::SetMirror(slot, nullptr, nullptr, nullptr);
    const auto leftovers = list::ReadLeftovers(m.player);
    bool       raised    = false;
    const bool removed   = list::Remove(g_gameInstance, m.player, g_removeLocalPlayer, raised);
    if (!removed)
    {
        platform::Log(L"mirror player: {}: the engine did NOT remove the mirror of column {}{}; kept, tried again later", why,
                      m.column + 1, raised ? L" (the call raised an exception)" : L"");
        TellSceneCamera();  // still in: its view stays player 1's
        return;
    }
    const auto gone = list::DestroyLeftovers(leftovers);
    ++g_removes;
    platform::Log(L"mirror player: {}: the mirror left column {} (controller id {}){}; leftovers destroyed: camera actor {}, "
                  L"battle-talk actor {} ({})",
                  why, m.column + 1, m.controllerId, raised ? L" (the call raised an exception after removing it)" : L"",
                  YesNo(gone.camera), YesNo(gone.talk), leftovers.Text());
    ForgetSlot(slot);
}

/// The mirrors' columns as the engine lists them now; a mirror the engine dropped is forgotten (logged).
void FollowList() noexcept
{
    for (std::int32_t slot = 0; slot < g_count;)
    {
        Mirror& m = g_mirrors[slot];
        if (!list::Listed(g_gameInstance, m.player))
        {
            ++g_dropped;
            platform::Log(L"mirror player: the engine dropped the mirror of column {} by itself (a world change?); it is added "
                          L"again once the field is steady",
                          m.column + 1);
            ForgetSlot(slot);
            continue;
        }
        const std::int32_t listed = list::Count(g_gameInstance);
        for (std::int32_t i = 0; i < listed; ++i)
        {
            if (list::At(g_gameInstance, i) == m.player)
            {
                m.column = i;
            }
        }
        ++slot;
    }
}

/// Adds mirrors up to `wanted` (`atLeave`: all at once, for the loading screen; else one per tick).
void Fill(std::int32_t playersIn, std::int32_t wanted, bool atLeave) noexcept
{
    while (g_count < wanted)
    {
        const std::int32_t column = playersIn + g_count;
        if (!Add(playersIn, column, atLeave) || !atLeave)
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

void Tick(std::int32_t playersIn, std::int32_t columns) noexcept
{
    if (g_removeLocalPlayer == 0)
    {
        return;
    }
    ++g_ticks;
    FollowList();
    g_columns = columns;
    g_wanted  = g_enabled ? std::clamp(columns - playersIn, 0, kMirrorsBuilt) : 0;
    while (g_count > g_wanted)
    {
        const std::int32_t before = g_count;
        RemoveSlot(g_count - 1, !g_enabled ? L"switched off" : L"its column is a player's, or no longer shown");
        if (g_count == before)
        {
            break;  // the engine kept it; tried again next tick
        }
    }
    if (g_count < g_wanted)
    {
        Fill(playersIn, g_wanted, false);
    }
    else if (g_wanted == 0)
    {
        g_waiting = nullptr;
    }
    if (g_count != 0 && g_ticks % kStatusTicks == 0 && platform::Enabled(platform::Level::Debug))
    {
        platform::LogDebug(L"mirror player: {}", Reading());
    }
}

void FillAtLeave(const wchar_t* why) noexcept
{
    // Player 2 is out as of this call (the session took it out first), so the real players are one.
    constexpr std::int32_t playersIn = 1;
    if (g_removeLocalPlayer == 0 || !g_enabled)
    {
        return;
    }
    FollowList();
    const std::int32_t wanted = std::clamp(g_columns - playersIn, 0, kMirrorsBuilt);
    const std::int32_t before = g_count;
    if (g_count >= wanted)
    {
        return;
    }
    Fill(playersIn, wanted, true);
    if (g_count > before)
    {
        platform::Log(L"mirror player: {} column(s) filled as {} (before the loading screen)", g_count - before, why);
    }
    else
    {
        platform::Log(L"mirror player: no column filled as {}: {}; they are filled once the field is steady", why,
                      g_waiting != nullptr ? g_waiting : L"the add failed (see above)");
    }
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
    return std::format(L"{}; wanted {} of {} column(s); {}; adds {} ({} at player 2's leave for a world change), removes {}, dropped by "
                       L"the engine {}{}; camera copy: {}",
                       g_enabled ? L"on" : L"off", g_wanted, g_columns, g_count != 0 ? std::format(L"in {}", in) : L"out", g_adds,
                       g_leaveFills, g_removes, g_dropped, g_waiting != nullptr ? std::format(L"; waiting: {}", g_waiting) : L"",
                       scene_camera_two::MirrorPlayerText());
}

}  // namespace lss::views::mirror_player
