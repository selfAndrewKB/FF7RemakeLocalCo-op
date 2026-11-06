#include "hud/lead_mark.h"

#include "control/registry.h"
#include "hud/field_icons.h"
#include "hud/minimap.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "players/lead_gate.h"
#include "players/seat_camera.h"
#include "players/seats.h"

#include <Unreal/NameTypes.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>

namespace lss::hud::lead_mark {

namespace {

using players::kSeatsBuilt;
using players::SeatName;

constexpr std::uint8_t  kMainStory      = 2;   // EEndMenuMapIconKind MainStory: the main objective's diamond, pin and compass mark
constexpr float         kLineLength     = 60;  // cm: the diamond above a standing point (the house marker's height)
constexpr float         kVisibleStart   = 5000;  // cm: the diamond shows within 50 m of the seat's character
// The "new" flag clears within this of the instance's character. AddIcon sets it again on every call for a key the
// story registry lacks; at this range the first update clears it, so the mark looks like a seen objective (no rim, no
// "new" label), and the hidden entry's label in the game's instance stays empty (RE-lead-mark-check.md).
constexpr float         kSeenWithin     = 1.0e7f;  // cm
constexpr std::uint32_t kCheckEvery     = 10;  // ticks
constexpr float         kMovedCm        = 1;   // a point further than this from the placed one is a new point

/// One seat's marker.
struct Seat
{
    std::uint64_t key         = 0;      // LSS_LeadMark_<seat>, made at the first use
    bool          keyFailed   = false;  // the name could not be made: no marker for this seat (logged once)
    bool          onGame      = false;  // added to the game's instance
    bool          onOwn       = false;  // added to the seat's own instance
    bool          clearOwner  = false;  // removed: the diamond's owner is let go at the next check (the widget's last tick)
    float         at[3]       = {};
    std::int64_t  point       = 0;
    std::uint32_t resets      = 0;      // minimap::IconResets() when added
    std::uint32_t ticks       = 0;
    bool          ownMissing  = false;  // the seat's instance was not there (logged once per placement)
};
std::array<Seat, kSeatsBuilt + 1> g_seats;  // by seat ([0] unused)
bool                              g_installed = false;

/// The FName of `text`, made if new (FNAME_Add), packed as the game passes it (comparison index low, number high); a
/// free function so a fault is caught by the guard.
void MakeName(const wchar_t* text, std::uint64_t* out) noexcept
try
{
    const RC::Unreal::FName name(text, RC::Unreal::FNAME_Add);
    *out = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(name.GetNumber())) << 32) |
           static_cast<std::uint32_t>(name.GetComparisonIndex().ToUnstableInt());
}
catch (...)
{
    *out = 0;
}

bool KeyOf(int seat, Seat& s) noexcept
{
    if (s.key != 0 || s.keyFailed)
    {
        return s.key != 0;
    }
    const std::wstring text = std::format(L"LSS_LeadMark_{}", seat);
    if (!platform::CallGuarded(&MakeName, text.c_str(), &s.key) || s.key == 0)
    {
        s.key       = 0;
        s.keyFailed = true;
        platform::Log(L"lead mark: the name {} could not be made; {}'s kept moves get no marker", text, SeatName(seat));
        return false;
    }
    return true;
}

minimap::IconParams Params(bool hidden) noexcept
{
    minimap::IconParams p;
    p.lineLength         = kLineLength;
    p.visibleStart       = kVisibleStart;
    p.viewTargetDistance = kSeenWithin;
    p.hidden       = hidden ? 1 : 0;
    return p;
}

void Take(int seat, Seat& s, const wchar_t* why) noexcept
{
    const bool game = s.onGame && minimap::RemoveIconFrom(0, s.key);
    const bool own  = s.onOwn && minimap::RemoveIconFrom(seat, s.key);
    platform::Log(L"lead mark: {}'s marker at {} removed ({}): game's instance {}, {}'s own {}", SeatName(seat),
                  players::lead_gate::PointName(s.point), why, s.onGame ? (game ? L"yes" : L"NO") : L"not there",
                  SeatName(seat), s.onOwn ? (own ? L"yes" : L"NO") : L"not there");
    s.onGame = s.onOwn = false;
    s.point            = 0;
    s.clearOwner       = true;
}

/// Adds what is missing (both for a new point or after a reset; the game updates a key it holds in place). `what`: the
/// Info line's verb, null for a retry of a missing instance.
void Place(int seat, Seat& s, const players::lead_gate::Mark& mark, const wchar_t* what) noexcept
{
    // Before the first placement, so no other view sees it appear.
    if (!field_icons::SetOwner(s.key, seat))
    {
        platform::Log(L"lead mark: no free owner record for {}'s diamond; it shows in every view", SeatName(seat));
    }
    s.clearOwner = false;
    if (!s.onGame)
    {
        s.onGame = minimap::AddIconTo(0, s.key, kMainStory, mark.at, Params(true));
    }
    if (!s.onOwn)
    {
        s.onOwn = minimap::AddIconTo(seat, s.key, kMainStory, mark.at, Params(false));
    }
    s.at[0]  = mark.at[0];
    s.at[1]  = mark.at[1];
    s.at[2]  = mark.at[2];
    s.point  = mark.point;
    s.resets = minimap::IconResets();
    if (what != nullptr)
    {
        s.ownMissing = false;
        platform::Log(L"lead mark: {}'s marker {} at {} ({:.1f}, {:.1f}, {:.1f}) m: the diamond in {}'s view {}, the pin on {}'s "
                      L"minimap {}",
                      SeatName(seat), what, players::lead_gate::PointName(mark.point), mark.at[0] / 100, mark.at[1] / 100,
                      mark.at[2] / 100, SeatName(seat), s.onGame ? L"yes" : L"NO (tried again)", SeatName(seat),
                      s.onOwn ? L"yes" : L"NO (its minimap not made yet; tried again)");
    }
    else if (s.onOwn && s.ownMissing)
    {
        s.ownMissing = false;
        platform::Log(L"lead mark: {}'s pin placed on its minimap", SeatName(seat));
    }
    if (!s.onOwn)
    {
        s.ownMissing = true;
    }
}

bool SamePoint(const Seat& s, const players::lead_gate::Mark& mark) noexcept
{
    return s.point == mark.point && std::fabs(s.at[0] - mark.at[0]) <= kMovedCm && std::fabs(s.at[1] - mark.at[1]) <= kMovedCm &&
           std::fabs(s.at[2] - mark.at[2]) <= kMovedCm;
}

std::wstring Reading()
{
    std::wstring text;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        const Seat& s = g_seats[static_cast<std::size_t>(seat)];
        text += std::format(L"{}{}: ", seat == 1 ? L"" : L"; ", SeatName(seat));
        text += s.onGame || s.onOwn
                    ? std::format(L"at {} ({:.1f}, {:.1f}, {:.1f}) m, game's instance {}, own {}", players::lead_gate::PointName(s.point),
                                  s.at[0] / 100, s.at[1] / 100, s.at[2] / 100, s.onGame ? L"yes" : L"no", s.onOwn ? L"yes" : L"no")
                    : std::wstring(L"none");
    }
    return text;
}

}  // namespace

bool Install(const platform::GameImage&) noexcept
{
    control::Register({.name  = L"hud.lead-mark",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) each further player's marker for the lead it keeps: the point, and whether the "
                                L"game's instance (the diamond) and the player's own minimap hold it",
                       .get   = [] {
                           try
                           {
                               return control::Value{Reading()};
                           }
                           catch (...)
                           {
                               return control::Value{std::wstring(L"unreadable (an exception)")};
                           }
                       }});
    g_installed = true;
    players::lead_gate::SetMarking(true);
    platform::Log(L"lead mark: a further player's kept lead is marked in that player's view (the main-objective diamond, a pin on "
                  L"its own minimap)");
    return true;
}

void Remove() noexcept
{
    if (!g_installed)
    {
        return;
    }
    players::lead_gate::SetMarking(false);
    // The game's icon calls are the game thread's (the HUD tick uses the store and the widgets): from another thread
    // the icons are left to the next field end, which empties them; the records and owners go either way.
    const bool onGame = players::seat_camera::IsGameThread();
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        Seat& s = g_seats[static_cast<std::size_t>(seat)];
        if ((s.onGame || s.onOwn) && onGame)
        {
            Take(seat, s, L"the mod unloads");
        }
        else if (s.onGame || s.onOwn)
        {
            platform::Log(L"lead mark: the mod unloads off the game thread; {}'s marker is left to the next field end", SeatName(seat));
            s.onGame = s.onOwn = false;
        }
        if (s.key != 0)
        {
            field_icons::SetOwner(s.key, -1);
        }
    }
    g_installed = false;
}

void TickSeat(int seat, void* member) noexcept
{
    if (!g_installed || seat < 1 || seat > kSeatsBuilt)
    {
        return;
    }
    Seat& s = g_seats[static_cast<std::size_t>(seat)];
    if (++s.ticks % kCheckEvery != 0)
    {
        return;
    }
    if (s.clearOwner && !s.onGame && !s.onOwn)
    {
        field_icons::SetOwner(s.key, -1);
        s.clearOwner = false;
    }
    bool reset = false;
    if ((s.onGame || s.onOwn) && s.resets != minimap::IconResets())
    {
        s.onGame = s.onOwn = false;  // a field end or reset emptied every instance: nothing left to remove
        s.clearOwner       = true;
        reset              = true;
        platform::Log(L"lead mark: the icons were reset (a field end); {}'s marker at {} went with them", SeatName(seat),
                      players::lead_gate::PointName(s.point));
    }
    players::lead_gate::Mark mark;
    if (member == nullptr || !players::lead_gate::KeptMark(member, mark))
    {
        if (s.onGame || s.onOwn)
        {
            Take(seat, s, member == nullptr ? L"the player let go of the member" : L"the move is no longer the player's to keep");
        }
        return;
    }
    if (!KeyOf(seat, s))
    {
        return;
    }
    const wchar_t* what = nullptr;  // the Info line's verb; null: only a missing instance is retried
    if (reset)
    {
        what = L"added again after the icons were reset (a field end)";
    }
    else if (!s.onGame && !s.onOwn)
    {
        what = L"placed";
    }
    else if (!SamePoint(s, mark))
    {
        s.onGame = s.onOwn = false;  // the game updates a key it holds in place
        what               = L"moved (the level's next move)";
    }
    if (!s.onGame || !s.onOwn)
    {
        Place(seat, s, mark, what);
    }
}

}  // namespace lss::hud::lead_mark
