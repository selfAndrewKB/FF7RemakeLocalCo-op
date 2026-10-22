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
#include "views/scene_camera_two.h"

#include <algorithm>
#include <format>

namespace lss::views::mirror_player {

namespace {

using RC::Unreal::UObject;
namespace list = players::local_player_list;

constexpr std::int32_t  kMirrorsBuilt = 1;    // one empty column today (two columns, player 2 out)
constexpr std::uint32_t kStatusTicks  = 300;
constexpr std::uint8_t  kStatePlay    = 6;    // the game state and the field step of a steady field (as the session's)
constexpr std::int32_t  kStepSteady   = 13;

// Set at install, then only read.
std::uintptr_t g_removeLocalPlayer = 0;

// Game thread.
bool           g_enabled      = true;
UObject*       g_player       = nullptr;  // the mirror's ULocalPlayer, only ever one this module added
UObject*       g_gameInstance = nullptr;  // the instance whose list it was added to
UObject*       g_controller   = nullptr;
UObject*       g_manager      = nullptr;
UObject*       g_managerOne   = nullptr;  // player 1's camera manager, the mirror's source
std::int32_t   g_seat         = 0;        // its controller id = its index in the list
std::int32_t   g_wanted       = 0;
std::uint32_t  g_ticks        = 0;
std::uint32_t  g_adds         = 0;
std::uint32_t  g_removes      = 0;
std::uint32_t  g_dropped      = 0;
const wchar_t* g_waiting      = nullptr;  // why the add waits, logged once per reason

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

void Forget() noexcept
{
    scene_camera_two::SetMirror(nullptr, nullptr, nullptr);
    g_player       = nullptr;
    g_gameInstance = nullptr;
    g_controller   = nullptr;
    g_manager      = nullptr;
    g_managerOne   = nullptr;
    g_seat         = 0;
}

/// Why the mirror cannot be added now, or null with player 1's controller and the instance filled.
const wchar_t* Refusal(std::int32_t seat, UObject*& playerOneController, UObject*& gameInstance) noexcept
{
    const auto game = players::game_state::Current();
    if (!(game.valid && game.hasRoot && game.running && game.step == kStepSteady && game.state == kStatePlay))
    {
        return L"the field is not steady (a load, or a scene change)";
    }
    gameInstance = list::GameInstance();
    if (list::Count(gameInstance) != seat)
    {
        return L"the local player count is not the mirror's column";
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

void Remove(const wchar_t* why) noexcept;

/// The mirror into the column after the real players (`seat` = their count): CreatePlayer, the views filled, the
/// split effects back on, its camera manager handed to the slot copy.
bool Add(std::int32_t seat) noexcept
{
    UObject* playerOne    = nullptr;
    UObject* gameInstance = nullptr;
    if (const wchar_t* why = Refusal(seat, playerOne, gameInstance); why != nullptr)
    {
        if (g_waiting != why)
        {
            g_waiting = why;
            platform::LogDebug(L"mirror player: waiting to fill column {}: {}", seat + 1, why);
        }
        return false;
    }
    g_waiting = nullptr;
    const list::Added added = list::Add(playerOne, seat);
    platform::Log(L"mirror player: {}", added.Text(L"the mirror's"));
    if (added.localPlayer == nullptr)
    {
        platform::Log(L"mirror player: the engine did not add a local player (count now {})", list::Count(gameInstance));
        return false;
    }
    g_player       = added.localPlayer;
    g_gameInstance = gameInstance;
    g_controller   = added.controller;
    g_manager      = added.cameraManager;
    g_managerOne   = Field(playerOne, facts::kControllerCameraManager);
    g_seat         = seat;
    ++g_adds;
    if (!added.called)
    {
        platform::Log(L"mirror player: the engine raised an exception while creating the mirror; removing it");
        Remove(L"the create raised");
        return false;
    }
    // Each view fills its column (the join does the same for both halves): the End camera actor constrains to 16:9.
    const bool filledOne    = list::FillView(g_managerOne);
    const bool filledMirror = list::FillView(g_manager);
    players::split_render::OnJoin();  // two views: the effects the split rule clears, back on
    scene_camera_two::SetMirror(g_controller, g_manager, g_managerOne);
    platform::Log(L"mirror player: in column {} (controller id {}, controller {:#x}, camera manager {:#x}, player 1's {:#x}; views "
                  L"filled: player 1 {}, the mirror {}); its camera manager takes every slot of player 1's before each update",
                  seat + 1, seat, Address(g_controller), Address(g_manager), Address(g_managerOne), YesNo(filledOne),
                  YesNo(filledMirror));
    return true;
}

void Remove(const wchar_t* why) noexcept
{
    if (g_player == nullptr)
    {
        return;
    }
    scene_camera_two::SetMirror(nullptr, nullptr, nullptr);
    if (!list::Listed(g_gameInstance, g_player))
    {
        platform::Log(L"mirror player: {}: the engine no longer lists the mirror; letting go without touching it", why);
        Forget();
        players::split_render::OnLeave();
        return;
    }
    const auto leftovers = list::ReadLeftovers(g_player);
    bool       raised    = false;
    const bool removed   = list::Remove(g_gameInstance, g_player, g_removeLocalPlayer, raised);
    if (!removed)
    {
        platform::Log(L"mirror player: {}: the engine did NOT remove the mirror{}; kept, tried again later", why,
                      raised ? L" (the call raised an exception)" : L"");
        scene_camera_two::SetMirror(g_controller, g_manager, g_managerOne);  // still in: its view stays player 1's
        return;
    }
    const auto gone = list::DestroyLeftovers(leftovers);
    ++g_removes;
    platform::Log(L"mirror player: {}: the mirror left column {}{}; leftovers destroyed: camera actor {}, battle-talk actor {} ({})",
                  why, g_seat + 1, raised ? L" (the call raised an exception after removing it)" : L"", YesNo(gone.camera),
                  YesNo(gone.talk), leftovers.Text());
    Forget();
    players::split_render::OnLeave();
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
              .help  = L"while the span is held, an extra local player in every empty column shows player 1's picture "
                       L"(the title, loading screens, the field, menus); off: the one view across the row",
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
              .help = L"the mirror player: wanted, in which column, its camera manager's slot copy, why it waits",
              .get  = [] { return Value{Reading()}; }});
    platform::Log(L"mirror player: ready (on: with the span held and player 2 out, the mirror fills the second column; "
                  L"mirror-player on|off)");
    return true;
}

void Tick(std::int32_t playersIn, std::int32_t columns) noexcept
{
    if (g_removeLocalPlayer == 0)
    {
        return;
    }
    ++g_ticks;
    if (g_player != nullptr && !list::Listed(g_gameInstance, g_player))
    {
        ++g_dropped;
        platform::Log(L"mirror player: the engine dropped the mirror from column {} by itself (a world change?); it is added "
                      L"again once the field is steady",
                      g_seat + 1);
        Forget();
        players::split_render::OnLeave();
    }
    g_wanted = g_enabled ? std::clamp(columns - playersIn, 0, kMirrorsBuilt) : 0;
    if (g_wanted == 0)
    {
        if (g_player != nullptr)
        {
            Remove(!g_enabled ? L"switched off" : columns == 0 ? L"the span is not held" : L"its column is a player's");
        }
        else
        {
            g_waiting = nullptr;
        }
    }
    else if (g_player == nullptr)
    {
        Add(playersIn);
    }
    if (g_player != nullptr && g_ticks % kStatusTicks == 0 && platform::Enabled(platform::Level::Debug))
    {
        platform::LogDebug(L"mirror player: {}", Reading());
    }
}

void Clear(const wchar_t* why) noexcept
{
    Remove(why);
}

std::int32_t Count() noexcept
{
    return g_player != nullptr ? 1 : 0;
}

std::wstring Reading() noexcept
{
    return std::format(L"{}; wanted {}; {}; adds {}, removes {}, dropped by the engine {}{}; camera copy: {}", g_enabled ? L"on" : L"off",
                       g_wanted, g_player != nullptr ? std::format(L"in column {} (controller id {})", g_seat + 1, g_seat) : L"out",
                       g_adds, g_removes, g_dropped, g_waiting != nullptr ? std::format(L"; waiting: {}", g_waiting) : L"",
                       scene_camera_two::MirrorPlayerText());
}

}  // namespace lss::views::mirror_player
