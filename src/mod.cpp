// LocalSplitScreen for FINAL FANTASY VII REMAKE: the UE4SS entry point.
//
// Verifies every engine fact it relies on, installs the session and the module table (modules.h), and turns the
// player-intent keys into joins and leaves on the game thread (the engine tick's pre-callback), so they never land
// inside a frame's drawing. A retry, load, jump or return to the title takes the further players out first and brings
// them back after (players/field_exit.h). Each tick: the keys and the console's lines, the modules' phase before the
// session, the session's tick, the columns' tick, the engine's list read once into the columns map, where each
// column's view comes from decided (views/column_source.h) and applied, then the modules' phase over the frame.
//   Ctrl+F7        player 2 joins with a controller and camera manager of its own, and takes its party member at once
//   Ctrl+F8        player 2 leaves
//   Ctrl+Shift+F7  player 3 joins (the third row of the party order), Ctrl+Shift+F8 player 3 leaves
//   Ctrl+F6        log every local player's controller, camera manager and camera actor
// The same intents, and every module's switches, are commands on the console (control/remote.h), driven from
// the host by tools/game.py.

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/Hooks/Hooks.hpp>

#include "battle/command_menu_tick.h"
#include "control/registry.h"
#include "control/remote.h"
#include "engine_facts.h"
#include "hud/overlay_pass.h"
#include "modules.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "players/camera_override.h"
#include "players/field_root.h"
#include "players/nav_area.h"
#include "players/operator_battle_mode.h"
#include "players/own_controller.h"
#include "players/seat_camera.h"
#include "players/scopes.h"
#include "players/session.h"
#include "players/single_view.h"
#include "players/split_switch.h"
#include "players/split_render.h"
#include "players/weak_object.h"
#include "views/column_rules.h"
#include "views/column_source.h"
#include "views/columns.h"
#include "views/mirror_player.h"
#include "views/scene_camera.h"
#include "views/whole_screen.h"
#include "views/window_span.h"
#include "world/shop_act.h"

#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <tuple>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#define LOCALSPLITSCREEN_API __declspec(dllexport)

namespace {

constexpr const wchar_t* kVersion = L"0.117.0-small-fixes";  // the one version string (the log and UE4SS)

using lss::platform::Log;
namespace Hook = RC::Unreal::Hook;

bool GameHasFocus() noexcept
{
    DWORD owner = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &owner);
    return owner == GetCurrentProcessId();
}

/// True on the tick a Ctrl+key (with Shift when `shift`, without it otherwise) goes down; `wasDown` carries the state
/// between ticks.
bool Pressed(int virtualKey, bool& wasDown, bool shift = false) noexcept
{
    const bool down    = GameHasFocus() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
                         ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) == shift && (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    const bool pressed = down && !wasDown;
    wasDown            = down;
    return pressed;
}

using lss::control::Outcome;
using lss::control::Shape;
using lss::control::Value;

/// A seat's console prefix: `p2` for the first further player, `p3` for the next (the user's names for them).
std::wstring SeatPrefix(int seat)
{
    return std::format(L"p{}", seat + 1);
}

/// The input holds the console can make: at the mod's own per-pad stick layer, so a scripted stick takes the
/// path a real pad takes.
class ModHolds final : public lss::control::Holds
{
public:
    explicit ModHolds(lss::players::Session& session) noexcept : session_(session) {}

    Outcome HoldStick(std::int32_t player, const float (&axes)[4]) override
    {
        if (!session_.In(player - 1))
        {
            return {false, L"stick holds act only while that player is in"};
        }
        lss::players::seat_camera::HoldStick(static_cast<unsigned>(player - 1), axes);
        return {};
    }
    void ReleaseStick(std::int32_t player) override
    {
        lss::players::seat_camera::ReleaseStick(static_cast<unsigned>(player - 1));
    }

private:
    lss::players::Session& session_;
};

/// One seat's intents and readings: `join`/`leave` (or the prefixed ones) and `<prefix>.member`, `.in`, `.rejoin`,
/// `.state`.
void RegisterSeatCommands(lss::players::Session& session, int seat, const std::wstring& prefix, const std::wstring& join,
                          const std::wstring& leave)
{
    using lss::control::Register;
    const std::wstring name = lss::players::SeatName(seat);
    Register({.name = join,
              .help = std::format(L"{} joins on its member (the party order's row {}) with a controller and camera manager of its own ({}); "
                                  L"with the member not free, nothing is made yet and {} joins once it is ({}.state awaiting)",
                                  name, seat + 1, lss::players::JoinKey(seat), name, prefix),
              .set  = [&session, seat, name, leave](const Value&) {
                  session.Join(seat);
                  return session.In(seat) ? Outcome{}
                         : session.Awaiting(seat)
                             ? Outcome{true, std::format(L"{} joins once its member is free (see the log; '{}' stops the wait)", name, leave)}
                             : Outcome{false, std::format(L"{} did not join (see the log)", name)};
              }});
    Register({.name = leave,
              .help = std::format(L"{} leaves, or stops waiting for a member ({})", name, lss::players::LeaveKey(seat)),
              .set  = [&session, seat, name](const Value&) {
                  session.Leave(seat);
                  return session.In(seat) ? Outcome{false, std::format(L"{} is still in (see the log)", name)} : Outcome{};
              }});
    Register({.name = std::format(L"{}.member", prefix), .shape = Shape::Text,
              .help = std::format(L"the member {} plays (its type and row of the party order), or why it waits for one", name),
              .get  = [&session, seat] { return Value{session.MemberReading(seat)}; }});
    Register({.name = std::format(L"{}.in", prefix), .shape = Shape::OnOff, .help = std::format(L"{} is in", name),
              .get = [&session, seat] { return Value{session.In(seat)}; }});
    Register({.name = std::format(L"{}.rejoin", prefix), .shape = Shape::OnOff,
              .help = std::format(L"{} was taken out by a world change and joins again once the field is back ('{}' cancels)", name, leave),
              .get  = [&session, seat] { return Value{session.RejoinPending(seat)}; }});
    Register({.name  = std::format(L"{}.state", prefix),
              .shape = Shape::Word,
              .words = {L"out", L"awaiting", L"rejoining", L"in", L"playing", L"handed"},
              .help  = L"out, awaiting (asked for; joins once its member is free), rejoining (taken out by a world change; joins "
                       L"again once the field is back), in (between a seat and the next, momentary), playing (holds its member), "
                       L"or handed (its member is with its AI while the game's leader is: a scene, a field action, the lock; or "
                       L"for the member's own hold: a script, an animation, a guide the level waits on; the seat is kept)",
              .get   = [&session, seat] { return Value{std::wstring(lss::players::StateName(session.State(seat)))}; }});
}

/// The session's commands and readings on the console. The modules register their own switches.
void RegisterSessionCommands(lss::players::Session& session)
{
    using lss::control::Register;
    // The first further player's intents are `join` and `leave` (the user's aliases); every seat's are prefixed.
    for (int seat = 1; seat <= lss::players::kSeatsBuilt; ++seat)
    {
        const std::wstring prefix = SeatPrefix(seat);
        RegisterSeatCommands(session, seat, prefix, seat == 1 ? L"join" : prefix + L".join", seat == 1 ? L"leave" : prefix + L".leave");
    }
    Register({.name = L"party.ai", .shape = Shape::Text,
              .help = L"each deployed member's status, PLAYER slot holder, actor mode and field AI layers (the take test's view)",
              .get  = [&session] { return Value{session.FormationReading()}; }});
    Register({.name  = L"showflags",
              .shape = Shape::Hex,
              .max   = 0xFFFFFFFF,
              .help  = L"the show flags forced on in every view while a further player is in (0 = the engine's split rule; "
                       L"players/split_render.h)",
              .set   = [&session](const Value& value) {
                  lss::players::split_render::SetMask(static_cast<std::uint32_t>(std::get<std::int64_t>(value)),
                                                      session.Count() != 0);
                  return Outcome{};
              },
              .get = [] { return Value{static_cast<std::int64_t>(lss::players::split_render::Mask())}; }});
    Register({.name = L"console", .shape = Shape::Text, .help = L"an engine console command, as typed in the console",
              .set  = [](const Value& value) {
                  const auto& command = std::get<std::wstring>(value);
                  Log(L"console command '{}'", command);
                  return lss::players::Session::Console(command.c_str())
                             ? Outcome{}
                             : Outcome{false, L"the console command could not be called"};
              }});
    Register({.name = L"report", .help = L"log every local player's controller, pawn, camera and call counts (Ctrl+F6)",
              .set  = [&session](const Value&) {
                  session.Status();
                  return Outcome{};
              }});
    Register({.name = L"quit", .help = L"quit the game through the engine (every further player must be out)",
              .set  = [&session](const Value&) {
                  if (session.Count() != 0)
                  {
                      return Outcome{false, L"a further player is still in ('leave' first)"};
                  }
                  return lss::players::Session::Console(L"quit") ? Outcome{}
                                                                  : Outcome{false, L"the console command could not be called"};
              }});
    Register({.name = L"version", .shape = Shape::Text, .help = L"the mod's version",
              .get  = [] { return Value{std::wstring(kVersion)}; }});
}

class LocalSplitScreenMod final : public RC::CppUserModBase
{
public:
    LocalSplitScreenMod()
    {
        ModName        = STR("LocalSplitScreen");
        ModVersion     = kVersion;
        ModDescription = STR("Two-player split-screen for FF7 Remake");
        ModAuthors     = STR("Andrew");
    }

    ~LocalSplitScreenMod() override
    {
        if (tickCallback_ != Hook::ERROR_ID)
        {
            Hook::UnregisterCallback(tickCallback_);
        }
        remote_.Close();
        LogSingleViewChange(singleView_.Release());  // the mod's switch cleared first: the split back for the game
        lss::views::mirror_player::Clear(L"the mod unloads");
        lss::players::split_render::Remove();  // after the mirrors: the mask back to zero so a reinstall finds it free
        lss::modules::Remove();                // the table in reverse (modules.h)
        if (installed_)
        {
            session_.Remove();  // the field-exit calls go back whether or not a further player is in
        }
        // With a further player in, its camera manager was built through the replacement; the DLL is pinned, so the
        // replacement stays valid. Otherwise the engine's function goes back, with the field hooks the seats'
        // members use (the table's rows removed with the players out) and the session's own camera modules.
        if (installed_ && session_.Count() == 0)
        {
            lss::players::seat_camera::RemoveInputTick();
            lss::modules::RemoveWithPlayersOut();
            lss::players::nav_area::Remove();
            lss::players::operator_battle_mode::Remove();
            lss::players::camera_override::Remove();
            lss::players::own_controller::Remove();
        }
    }

    auto on_unreal_init() -> void override
    {
        Log(L"v{} starting", kVersion);
        lss::platform::settings::Load();  // the settings file, read once; each option's owner applies its word at install
        const auto image = lss::platform::GameImage::Locate();
        if (image)
        {
            lss::players::weak_object::Install(*image);  // first: every module that keeps a game object between ticks uses it
            lss::players::field_root::Install(*image);   // second: the field root, the battle system and the party leader, read by most
        }
        if (!image || !session_.Install(*image))
        {
            Log(L"standing aside: an engine check failed (see above); nothing installed, the game runs unmodified");
            return;
        }
        installed_ = true;
        lss::modules::Install(*image);  // every module after the session, in table order (modules.cpp)
        if (lss::modules::MirrorPlayerInstalled())
        {
            session_.SetMirrorPlayers({&lss::views::mirror_player::Count, &lss::views::mirror_player::Clear,
                                       &lss::views::mirror_player::FillAtLeave});
        }
        RegisterSessionCommands(session_);
        lss::views::column_source::RegisterCommands(singleViewChoice_);
        remote_.Open();
        const Hook::FCallbackOptions options{false, false, STR("LocalSplitScreen"), STR("player intent")};
        tickCallback_ = Hook::RegisterEngineTickPreCallback(
            [this](Hook::TCallbackIterationData<void>&, RC::Unreal::UEngine*, float, bool) { OnEngineTick(); }, options);
        Log(L"ready: every engine check passed. Ctrl+F7 adds player 2 (it takes the 2nd party slot's member), Ctrl+F8 removes player 2, "
            L"Ctrl+Shift+F7 adds player 3 (the 3rd slot's member), Ctrl+Shift+F8 removes player 3, Ctrl+F6 reports, Ctrl+F2 toggles "
            L"the window span, Ctrl+F9 switches the battle menus' world slow-down and grey; with window-span.hold on the mirror player fills the empty columns");
    }

private:
    void OnEngineTick() noexcept
    {
        // Ctrl+F7/F8 for the first further player, Ctrl+Shift+F7/F8 for the next (players/seats.h names the keys).
        static_assert(lss::players::kSeatsBuilt <= 2, "the keys: Ctrl+F7/F8 and Ctrl+Shift+F7/F8, one pair per built seat");
        for (int seat = 1; seat <= lss::players::kSeatsBuilt; ++seat)
        {
            const auto at = static_cast<std::size_t>(seat);
            if (Pressed(VK_F7, joinWasDown_[at], seat != 1))
            {
                session_.Join(seat);
            }
            if (Pressed(VK_F8, leaveWasDown_[at], seat != 1))
            {
                session_.Leave(seat);
            }
        }
        if (Pressed(VK_F6, reportWasDown_))
        {
            session_.Status();
        }
        if (Pressed(VK_F2, spanWasDown_))
        {
            lss::views::window_span::Toggle();
        }
        if (Pressed(VK_F9, menuSlowWasDown_))
        {
            lss::battle::command_menu_tick::SetMenuSlow(!lss::battle::command_menu_tick::MenuSlow(), L"Ctrl+F9");
        }
        // The console's lines (joins and leaves among them) before the session and the column map, as the keys, so
        // this frame's HUD passes never name a player the console just took out.
        remote_.Tick();
        lss::modules::TickBeforeSession(CameraManagerOf(0));
        session_.Tick();
        // The columns' tick (modules.h): the span, the layout and the mirrors toward the columns the frame shows.
        const lss::modules::Columns columns = lss::modules::TickColumns({.playersIn = 1 + session_.Count(),
                                                                         .seatIds   = session_.SeatIds(),
                                                                         .listSeats = ListSeatsOf(MapOfList())});
        // The engine's list as columns, after every join, leave and mirror change of this tick (views/columns.h): the one
        // map every module that runs per column reads (the HUD passes, the listeners, this frame's passes).
        columns_ = MapOfList();
        lss::hud::overlay_pass::SetMap(columns_);
        // Where each column's view comes from this tick (views/column_source.h decides; this is the one place that
        // applies it): the engine's single view through the viewport's switch (players/single_view.h), a seat on
        // player 1's picture gets the every-slot camera copy and no HUD pass of its own.
        lss::views::column_source::Moment moment;
        moment.columnsHeld = columns.columnsHeld;
        moment.choice      = singleViewChoice_;
        moment.map         = columns_;
        moment.wholeScreen = session_.Count() > 0 && lss::views::whole_screen::Shown();
        for (int seat = 1; seat <= lss::players::kSeatsBuilt; ++seat)
        {
            moment.seats[static_cast<std::size_t>(seat)]   = session_.State(seat);
            moment.ownShop[static_cast<std::size_t>(seat)] = lss::world::shop_act::OwnsScene(seat);
        }
        lss::views::column_source::Shown shown = lss::views::column_source::Decide(moment);
        LogSingleViewChange(singleView_.Update(shown.Single()));
        if (shown.Single() && !singleView_.Single())
        {
            // The switch could not be set (or read): the columns stay on screen, so the same moment is answered as
            // with columns held: player 1's camera and HUD in every further column (never a split with a menu over it).
            moment.columnsHeld = true;
            shown              = lss::views::column_source::Decide(moment);
        }
        // The other way round (the split decided while the switch stays set: a clear that failed, or someone else's
        // switch) the screen stays single; the policy's log says so, and the HUD follows the engine's view sizes.
        FollowShown(shown);
        // The next shop a further player opens: its camera on the screen's one view in one window (where the single
        // view can show it), on the opener's on a row or under the console's off (world/shop_act.h).
        lss::world::shop_act::SetOneWindow(!columns.columnsHeld &&
                                           singleViewChoice_ != lss::views::column_source::Override::Split);
        lss::modules::TickAfterSession(FrameNow());
    }

    /// The engine's local-player list as columns (views/columns.h): each listed place's seat (0 player 1, then the
    /// seats by the player each holds), a mirror's (views/mirror_player.h) or unknown; as many as are listed.
    lss::views::columns::Map MapOfList() const noexcept
    {
        namespace columns = lss::views::columns;
        std::array<int, columns::kMaxColumns> places{};
        int                                   listed = 0;
        for (int place = 0; place < columns::kMaxColumns; ++place)
        {
            auto* player = session_.LocalPlayer(place);
            if (player == nullptr)
            {
                break;  // the list ends here (the engine closes it up at a removal)
            }
            listed   = place + 1;
            int seat = place == 0 ? 0 : columns::kUnknown;
            for (int s = 1; seat == columns::kUnknown && s <= lss::players::kSeatsBuilt; ++s)
            {
                seat = session_.SeatPlayer(s) == player ? s : seat;
            }
            if (seat == columns::kUnknown && lss::views::mirror_player::InColumn(place))
            {
                seat = columns::kMirror;
            }
            places[static_cast<std::size_t>(place)] = seat;
        }
        return columns::MapOf(std::span<const int>(places.data(), static_cast<std::size_t>(listed)));
    }

    /// The map's places as the column order's rule takes them (views/column_rules.h: a seat, or -1 for no seat).
    static lss::views::column_rules::ListSeats ListSeatsOf(const lss::views::columns::Map& map) noexcept
    {
        static_assert(std::tuple_size_v<lss::views::column_rules::ListSeats> <= lss::views::columns::kMaxColumns,
                      "the layout's places are columns");
        lss::views::column_rules::ListSeats seats{0, -1, -1};  // place 0 is player 1's, listed or not
        for (std::size_t place = 1; place < seats.size(); ++place)
        {
            seats[place] = map.seatAt[place] >= 0 ? map.seatAt[place] : -1;
        }
        return seats;
    }

    /// Applies this tick's decision (views/column_source.h): each seat's change logged once with its reason, and the
    /// scene camera module told which seats' managers take player 1's every slot.
    void FollowShown(const lss::views::column_source::Shown& shown) noexcept
    {
        for (int seat = 1; seat <= lss::players::kSeatsBuilt; ++seat)
        {
            const auto at  = static_cast<std::size_t>(seat);
            const auto now = shown.seatIn[at] ? shown.seats[at] : lss::views::column_source::SeatSource{};
            const auto was = shown_.seatIn[at] ? shown_.seats[at] : lss::views::column_source::SeatSource{};
            if (const std::wstring text = lss::views::column_source::Describe(seat, was, now); !text.empty() && shown.seatIn[at])
            {
                Log(L"column source: {}", text);  // a seat in the game; a leave says nothing here (the session logs it)
            }
            lss::views::scene_camera::SetSource(seat, shown.OnPlayerOne(seat), ManagerOfSeat(seat));
        }
        shown_ = shown;
    }

    /// The single view's change (players/single_view.h), logged once per change.
    static void LogSingleViewChange(lss::players::SingleViewChange change) noexcept
    {
        if (const std::wstring text = lss::players::Describe(change); !text.empty())
        {
            Log(L"{}", text);
        }
    }

    /// This tick's facts for the modules (modules.h), read once from the session: each seat's word, player and
    /// objects, player 1's camera manager and pawn, the columns map and the decision, each column's local player.
    lss::modules::Frame FrameNow() const noexcept
    {
        lss::modules::Frame frame;
        frame.count      = session_.Count();
        frame.managerOne = CameraManagerOf(0);
        frame.pawnOne    = PawnOf(0);
        frame.map        = columns_;
        frame.shown      = shown_;
        for (int column = 0; column < lss::views::columns::kMaxColumns; ++column)
        {
            const auto at           = static_cast<std::size_t>(column);
            frame.localPlayer[at]   = session_.LocalPlayer(column);
            frame.controller[at]    = ControllerOf(column);
        }
        for (int seat = 1; seat <= lss::players::kSeatsBuilt; ++seat)
        {
            auto& s       = frame.seats[static_cast<std::size_t>(seat)];
            s.state       = session_.State(seat);
            s.seated      = session_.Seated(seat);
            s.localPlayer = session_.SeatPlayer(seat);
            s.controller  = ControllerOfSeat(seat);
            s.manager     = ManagerOfSeat(seat);
            s.member      = session_.In(seat) ? lss::players::scopes::Member(seat) : nullptr;
        }
        return frame;
    }

    /// A local player's camera manager (null if unread).
    void* CameraManagerOf(int index) const noexcept
    {
        return lss::platform::ReadPointer(ControllerOf(index), lss::facts::kControllerCameraManager);
    }

    /// A local player's pawn (player 1's is the character the game centres the NPCs' senses on), or null.
    void* PawnOf(int index) const noexcept
    {
        return lss::platform::ReadPointer(ControllerOf(index), lss::facts::kControllerPawn);
    }

    /// A local player's controller, or null.
    void* ControllerOf(int index) const noexcept
    {
        return lss::platform::ReadPointer(session_.LocalPlayer(index), lss::facts::kLocalPlayerController);
    }

    /// A seat's own controller while its player is in (whatever its column), or null.
    void* ControllerOfSeat(int seat) const noexcept
    {
        return lss::platform::ReadPointer(session_.SeatPlayer(seat), lss::facts::kLocalPlayerController);
    }

    /// A seat's own camera manager while its player is in, or null.
    void* ManagerOfSeat(int seat) const noexcept
    {
        return lss::platform::ReadPointer(ControllerOfSeat(seat), lss::facts::kControllerCameraManager);
    }

    // Declaration order matters: holds_ and remote_ hold references into session_, so it comes first.
    lss::players::Session               session_;
    ModHolds                            holds_{session_};
    lss::control::Remote                remote_{holds_};
    lss::views::columns::Map            columns_;  // this tick's map (set before the modules that read it)
    lss::views::column_source::Shown    shown_;    // this tick's decision
    lss::views::column_source::Override singleViewChoice_ = lss::views::column_source::Override::Auto;
    lss::players::SingleView            singleView_{{&lss::players::split_switch::Read, &lss::players::split_switch::Write}};
    bool                                installed_         = false;
    std::array<bool, lss::players::scopes::kMaxPlayers> joinWasDown_{};   // by seat (index 0 unused)
    std::array<bool, lss::players::scopes::kMaxPlayers> leaveWasDown_{};
    bool                                reportWasDown_     = false;
    bool                                spanWasDown_       = false;
    bool                                menuSlowWasDown_   = false;
    Hook::GlobalCallbackId              tickCallback_      = Hook::ERROR_ID;
};

}  // namespace

extern "C"
{
    LOCALSPLITSCREEN_API RC::CppUserModBase* start_mod() { return new LocalSplitScreenMod(); }
    LOCALSPLITSCREEN_API void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
}
