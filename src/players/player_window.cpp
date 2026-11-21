#include "players/player_window.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>

#include "engine_facts.h"
#include "hud/save_notice.h"
#include "hud/view_prompt.h"
#include "hud/view_window.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/steady_ms.h"
#include "players/field_root.h"
#include "players/game_state.h"
#include "players/pad_presence.h"
#include "players/pad_rules.h"
#include "players/pad_table.h"
#include "players/party_member.h"
#include "players/player_window_rules.h"
#include "players/player_window_text.h"
#include "players/scopes.h"
#include "players/seats.h"
#include "players/session.h"
#include "views/column_layout.h"
#include "views/whole_screen.h"
#include "views/whole_screen_rules.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <format>
#include <optional>
#include <string>
#include <utility>

namespace lss::players::player_window {

namespace {

namespace pad   = pad_rules;
namespace rules = player_window_rules;
namespace text  = player_window_text;

constexpr int kSeats = rules::kSeats;
constexpr int kTitleClaimedSeconds = 4;  // the notice after a press on the title: which controller plays player 1
constexpr unsigned kStepCheckTicks = 15;  // ticks between re-checks of an open window's character arrows

struct Event
{
    int           user    = 0;
    std::uint16_t pressed = 0;
    bool          open    = false;  // View held a second
    bool          bTap    = false;  // B let go within a second
    bool          bHeld   = false;  // B held a second
};

/// A join a pad asked for, made by RunRequests before the session's tick.
struct JoinAsk
{
    int seat = -1;
    int user = -1;  // the joining pad
};

/// A line a window shows for a while: a join that waits, a refusal.
struct SeatNote
{
    text::Note   note  = text::Note::None;
    int          about = 0;
    std::int64_t until = 0;
    std::wstring aboutName;  // the character the note names, when it names one
};

constexpr std::int64_t kNoteMs = 3000;
/// A join's wait note: longer, and said again at each Start of the waiting pad.
constexpr std::int64_t kWaitNoteMs = 6000;

// Noted by the pad table's listener during the engine's pad poll, taken in Tick. Game thread both.
constexpr std::size_t     kQueue = 32;
std::array<Event, kQueue> g_events{};
std::size_t               g_queued  = 0;
std::uint32_t             g_dropped = 0;

// Game thread.
bool                             g_installed = false;
Session*                         g_session   = nullptr;
std::array<bool, kSeats>         g_open{};                // by seat: the player's window is open
std::array<SeatNote, kSeats>     g_notes{};               // by seat
std::array<bool, pad::kPads>     g_bSeen{};               // by pad: B went down while its window was open (arms B)
std::array<JoinAsk, kSeats>      g_joins{};               // asked, not yet made
std::array<int, kSeats>          g_leaves{-1, -1, -1};    // players asked to leave, not yet out
std::array<bool, kSeats>         g_waiting{};             // by seat: a join that waits; its window opens once in
std::array<bool, kSeats>         g_stepShown{};           // by seat: the window shows the character arrows
unsigned                         g_stepCheckTicks = 0;    // ticks since the arrows were last checked
int                              g_startWhenSplit = -1;   // a spare pad's Start during one view: joins once split
bool                             g_redraw         = false;
bool                             g_title          = false;
int                              g_titleClaimed   = -1;   // the pad the title's notice last said (-1: none this visit)
std::uint32_t                    g_playing        = 1;    // the slots a player holds, as of the last tick
std::uint32_t                    g_idlePads       = 0;    // the pads read idle now (the open windows' pads)
unsigned long long               g_gameClosedSeen = 0;    // view_window's count of windows the game closed
int                              g_lastScreens    = 0;    // the views as last shown
views::column_rules::ListSeats   g_lastList{};

using platform::NowMs;

void Note(int user, std::uint16_t pressed, bool open, bool bTap, bool bHeld) noexcept
{
    if (g_queued < kQueue)
    {
        g_events[g_queued++] = {user, pressed, open, bTap, bHeld};
    }
    else
    {
        ++g_dropped;
    }
}

void ClassText(void* object, std::wstring* out) noexcept
try
{
    *out = static_cast<RC::Unreal::UObject*>(object)->GetClassPrivate()->GetName();
}
catch (...)
{
    out->clear();
}

/// The seat's character in capitals, from its class ("PC0001_00_Barret_Standard_C": BARRET), else empty. Player 2 or
/// 3's is its row's (the rank's, so a step shows at once; the member follows a tick later).
std::wstring CharacterOf(int seat)
{
    void* member = seat == 0 ? players::field_root::PartyLeader(facts::kLeaderCurrent)
                   : g_session != nullptr ? g_session->RowActor(seat)
                                          : nullptr;
    member = member != nullptr || seat == 0 ? member : scopes::Member(seat);
    std::wstring cls;
    if (member != nullptr)
    {
        platform::CallGuarded(&ClassText, member, &cls);
    }
    // PC<number>_<variant>_<Name>_...
    if (cls.starts_with(L"PC"))
    {
        const auto first  = cls.find(L'_');
        const auto second = first == std::wstring::npos ? first : cls.find(L'_', first + 1);
        const auto third  = second == std::wstring::npos ? second : cls.find(L'_', second + 1);
        if (third != std::wstring::npos && third > second + 1)
        {
            std::wstring name = cls.substr(second + 1, third - second - 1);
            for (wchar_t& c : name)
            {
                c = static_cast<wchar_t>(std::towupper(c));
            }
            return name;
        }
    }
    return {};
}

/// The seat's character in capitals, else "PLAYER n".
std::wstring NameOf(int seat)
{
    std::wstring name = CharacterOf(seat);
    return name.empty() ? std::format(L"PLAYER {}", seat + 1) : name;
}

bool Plays(int seat) noexcept
{
    return rules::Plays(g_playing, seat);
}

/// Another player is in: the d-pad's left/right trade numbers.
bool CanTrade(int seat) noexcept
{
    return rules::TradePartner(seat, -1, g_playing) >= 0 || rules::TradePartner(seat, +1, g_playing) >= 0;
}

/// The lowest seat of players 2 and 3 that no player holds, waits for, rejoins or is joining now, or -1.
int FreeSeat()
{
    for (int seat = 1; g_session != nullptr && seat <= kSeatsBuilt; ++seat)
    {
        const bool asked = std::any_of(g_joins.begin(), g_joins.end(), [seat](const JoinAsk& j) { return j.seat == seat; });
        if (!asked && !g_session->In(seat) && !g_session->Awaiting(seat) && !g_session->RejoinPending(seat))
        {
            return seat;
        }
    }
    return -1;
}

void SetNote(int seat, text::Note note, int about)
{
    g_notes[static_cast<std::size_t>(seat)] = {note, about, NowMs() + kNoteMs, {}};
    g_redraw                                = true;
}

/// The member of a waiting join is with the game: a script, a cutscene, an animation, a field action, or the game's
/// leader handed to its AI. The other waits (another controller on it, no actor yet) name no character.
bool WaitsOnGameAi(party_member::Wait why) noexcept
{
    using party_member::Wait;
    return party_member::ScriptHold(why) || why == Wait::FieldAction || why == Wait::LeaderHanded ||
           why == Wait::PushButtonEvent;
}

/// Player 1's window says for a while that `seat`'s join waits: naming the character when the game holds it.
void SetWaitNote(int seat)
{
    const bool         onAi = g_session != nullptr && WaitsOnGameAi(g_session->WaitReason(seat));
    const std::wstring name = onAi ? CharacterOf(seat) : std::wstring();
    g_notes[0] = {name.empty() ? text::Note::Waits : text::Note::WaitsOnAi, seat, NowMs() + kWaitNoteMs, name};
    g_redraw   = true;
}

/// The seat of players 2 and 3 whose player is out for now with `user`'s pad on it (a join that waits, a player the
/// session put back into a wait, a rejoin after a world change), or -1. Such a pad plays no one yet but is no spare.
int WaitingSeatOf(int user) noexcept
{
    const auto table = pad_table::Table();
    for (int seat = 1; seat < kSeats; ++seat)
    {
        const bool out = g_waiting[static_cast<std::size_t>(seat)] ||
                         (g_session != nullptr && (g_session->Awaiting(seat) || g_session->RejoinPending(seat)));
        if (out && !Plays(seat) && pad::PadOn(table, seat) == user)
        {
            return seat;
        }
    }
    return -1;
}

bool AnyRequest()
{
    return std::any_of(g_joins.begin(), g_joins.end(), [](const JoinAsk& j) { return j.seat >= 0; }) ||
           std::any_of(g_leaves.begin(), g_leaves.end(), [](int seat) { return seat >= 0; });
}

/// Each screen's seat now (-1 for a copy's): the engine's list placed by the column order, as the layout places it.
std::array<int, kSeats> SeatOnScreens(const Frame& frame)
{
    std::array<int, kSeats> seatOn{-1, -1, -1};
    const int  screens = std::clamp(frame.screens, 1, kSeats);
    const auto slots   = views::column_rules::ScreenSlots(views::column_layout::Views(), views::column_layout::CurrentOrder(),
                                                          frame.listSeats);
    for (int place = 0; place < screens; ++place)
    {
        const int slot = slots[static_cast<std::size_t>(place)];
        if (slot >= 0 && slot < kSeats)
        {
            seatOn[static_cast<std::size_t>(slot)] = frame.listSeats[static_cast<std::size_t>(place)];
        }
    }
    return seatOn;
}

void Open(int seat, const std::wstring& why)
{
    g_open[static_cast<std::size_t>(seat)] = true;
    g_redraw                               = true;
    hud::view_window::Play(hud::view_window::Sound::Decide);
    platform::Log(L"player window: player {}'s opens ({})", seat + 1, why);
}

void Close(int seat, const wchar_t* why)
{
    if (!g_open[static_cast<std::size_t>(seat)])
    {
        return;
    }
    g_open[static_cast<std::size_t>(seat)] = false;
    g_redraw                               = true;
    platform::Log(L"player window: player {}'s closes ({})", seat + 1, why);
}

/// Every window and note in each player's own view (hud/view_window, else the engine-font prompt); the rest closed.
void ShowWindows(const Frame& frame)
{
    constexpr int kViews = hud::view_window::kViews;
    hud::view_prompt::ClearAll();
    std::array<bool, kViews> shown{};
    const int screens = std::clamp(frame.screens, 1, kSeats);
    for (int seat = 0; seat < kSeats; ++seat)
    {
        const SeatNote& note = g_notes[static_cast<std::size_t>(seat)];
        if (!Plays(seat) || (!g_open[static_cast<std::size_t>(seat)] && note.note == text::Note::None))
        {
            continue;
        }
        text::Window w;
        w.seat       = seat;
        w.name       = NameOf(seat);
        w.open       = g_open[static_cast<std::size_t>(seat)];
        w.canStep    = seat >= 1 && g_session != nullptr && g_session->CanStepMember(seat);
        w.canTrade   = CanTrade(seat);
        w.canMove    = screens >= 2;
        w.startJoins = seat == 0 && FreeSeat() >= 0;
        w.note       = note.note;
        w.about      = note.about;
        w.aboutName  = note.aboutName;
        const text::ScreenText t = text::TextFor(w);
        shown[static_cast<std::size_t>(seat)]       = true;
        g_stepShown[static_cast<std::size_t>(seat)] = w.canStep;
        if (!hud::view_window::Show(seat, t.markup))
        {
            hud::view_prompt::Prompt p;
            p.lines     = t.plain;
            p.highlight = 0;
            hud::view_prompt::Show(seat, p);
        }
    }
    for (int view = 0; view < kViews; ++view)
    {
        if (!shown[static_cast<std::size_t>(view)])
        {
            hud::view_window::Close(view);
        }
    }
}

/// The pads of the open windows read idle to the game; the others play on.
void SetIdlePads()
{
    std::uint32_t pads    = 0;
    const auto    table   = pad_table::Table();
    const auto    present = pad_table::PresentPads();
    for (int seat = 0; seat < kSeats; ++seat)
    {
        const int on = pad::PlayingPad(table, seat, present);
        if (g_open[static_cast<std::size_t>(seat)] && on >= 0)
        {
            pads |= 1u << on;
        }
    }
    if (pads != g_idlePads)
    {
        g_idlePads = pads;
        pad_table::SetIdlePads(pads, L"the open player windows");
    }
}

/// The seat pad `which` plays, or -1 (player_window_rules::SeatOf, with the pads answering now).
int SeatOfPad(int which)
{
    return rules::SeatOf(pad_table::Table(), g_playing, pad_table::PresentPads(), which);
}

/// The column order set and saved (the Co-op page's Screen order).
void ShowOrder(const views::column_rules::Order& order, const wchar_t* why)
{
    views::column_layout::ChangeOrder(order, why);
    platform::settings::Save(platform::settings::kColumnOrder, rules::OrderWord(order.player));
    g_redraw = true;
}

/// `seat`'s view one screen `dir`: swapped with what is there.
void MoveView(const Frame& frame, int seat, int dir)
{
    using hud::view_window::Sound;
    const int  screens = std::clamp(frame.screens, 1, kSeats);
    const auto moved   = rules::Moved(SeatOnScreens(frame), screens, seat, dir);
    const auto order   = moved ? views::column_rules::OrderShowing(*moved) : std::nullopt;
    if (!order)
    {
        hud::view_window::Play(Sound::Cancel);
        platform::LogDebug(L"player window: player {}'s view cannot move {} (the row's edge, or one screen)", seat + 1,
                           dir < 0 ? L"left" : L"right");
        return;
    }
    ShowOrder(*order, L"a player moved their view");
    hud::view_window::Play(Sound::Cursor);
    platform::Log(L"player window: player {}'s view moves {}; the screen order is now {} {} {}", seat + 1,
                  dir < 0 ? L"left" : L"right", order->player[0], order->player[1], order->player[2]);
}

/// `seat`'s player trades numbers with the nearest player `dir`: the two pads trade slots and the two screens trade
/// numbers, so each person keeps their pad and their screen and plays the other number, its character and its window.
/// Held while a join or a leave is on its way (those name a number).
void TradeNumber(const Frame& frame, int seat, int dir)
{
    using hud::view_window::Sound;
    const int  other = AnyRequest() ? -1 : rules::TradePartner(seat, dir, g_playing);
    const auto order = other >= 0 ? views::column_rules::OrderShowing(rules::Traded(SeatOnScreens(frame), seat, other))
                                  : std::nullopt;
    const auto table = pad_table::Table();
    if (!order || !pad_table::Set(rules::WithPadsTraded(table, seat, other), L"two players traded numbers"))
    {
        hud::view_window::Play(Sound::Cancel);
        platform::LogDebug(L"player window: player {} trades numbers with no one {} (the end of the numbers, or a join or "
                           L"leave on its way)",
                           seat + 1, dir < 0 ? L"lower" : L"higher");
        return;
    }
    ShowOrder(*order, L"two players traded numbers");
    std::swap(g_open[static_cast<std::size_t>(seat)], g_open[static_cast<std::size_t>(other)]);
    std::swap(g_notes[static_cast<std::size_t>(seat)], g_notes[static_cast<std::size_t>(other)]);
    hud::view_window::Play(Sound::Cursor);
    platform::Log(L"player window: players {} and {} trade numbers: {} plays player {}, {} player {}; the screen order is "
                  L"now {} {} {}",
                  seat + 1, other + 1, pad::PadName(pad::PadOn(table, seat)), other + 1,
                  pad::PadName(pad::PadOn(table, other)), seat + 1, order->player[0], order->player[1], order->player[2]);
}

/// A spare pad's Start: takes back the lowest player whose pad is gone (a pad that died and came back on another
/// number), else a join on the lowest seat out (during one view, once the views split).
void SpareStart(const Frame& frame, int user)
{
    using hud::view_window::Sound;
    if (std::any_of(g_joins.begin(), g_joins.end(), [user](const JoinAsk& j) { return j.user == user; }))
    {
        return;  // asked already: its join is made before the session's next tick
    }
    // A pad whose player is out for now plays no one yet: its Start says the wait again (in the field), never a second
    // join.
    if (const int waiting = WaitingSeatOf(user); waiting >= 0)
    {
        if (!frame.single && !views::whole_screen::Shown())
        {
            SetWaitNote(waiting);
        }
        hud::view_window::Play(Sound::Cursor);
        platform::LogDebug(L"player window: {} pressed Start while its join as player {} waits", pad::PadName(user), waiting + 1);
        return;
    }
    // One pad read by both readers (Steam Input's XInput copy of a PlayStation pad the game also reads) presses Start on
    // both at once: that Start is the player's whose pad the other copy is, else one join, the PlayStation copy's.
    if (const int twin = pad_table::TwinOf(user, pad::kStart); twin >= 0 && pad::TwinStartDropped(user, SeatOfPad(twin) >= 0))
    {
        const int seat = SeatOfPad(twin);
        platform::Log(L"player window: {} pressed Start at the same moment as {}: one pad read twice (Steam Input); {}",
                      pad::PadName(user), pad::PadName(twin),
                      seat >= 0 ? std::format(L"the Start is player {}'s, no join", seat + 1)
                                : std::format(L"only {} joins", pad::PadName(twin)));
        return;
    }
    const auto table = pad_table::Table();
    if (const int seat = rules::SeatWithPadGone(table, g_playing, pad_presence::GonePads()); seat >= 0)
    {
        if (pad_table::Set(rules::WithPadOn(table, user, seat), L"a spare pad takes back a player whose pad is gone"))
        {
            g_bSeen[static_cast<std::size_t>(user)] = false;
            g_redraw                                = true;
            hud::view_window::Play(Sound::Decide);
            platform::Log(L"player window: {} pressed Start: it takes back player {}, whose {} is gone", pad::PadName(user),
                          seat + 1, pad::PadName(pad::PadOn(table, seat)));
        }
        return;
    }
    const int free = FreeSeat();
    if (free < 0)
    {
        SetNote(0, text::Note::AllIn, 0);
        hud::view_window::Play(Sound::Cancel);
        platform::Log(L"player window: {} pressed Start, but every player is in or on the way", pad::PadName(user));
        return;
    }
    if (frame.single || views::whole_screen::Shown())
    {
        g_startWhenSplit = user;
        platform::Log(L"player window: {} pressed Start during one view (a scene or a menu); it joins once the views split",
                      pad::PadName(user));
        return;
    }
    for (JoinAsk& j : g_joins)
    {
        if (j.seat < 0)
        {
            j = {free, user};
            break;
        }
    }
    hud::view_window::Play(Sound::Decide);
    platform::Log(L"player window: {} pressed Start: it joins as player {}", pad::PadName(user), free + 1);
}

/// The session's answer to a join: in (its window opens), waiting for a member that is busy now, or refused (no member
/// free: the wait stopped). Said on player 1's window.
void AnswerJoin(const JoinAsk& j)
{
    using party_member::Wait;
    g_redraw = true;
    g_bSeen[static_cast<std::size_t>(j.user)] = false;  // a B from before the join never closes or leaves
    if (g_session->In(j.seat))
    {
        g_open[static_cast<std::size_t>(j.seat)] = true;
        platform::Log(L"player window: player {} is in on {}; its window opens", j.seat + 1, pad::PadName(j.user));
        return;
    }
    if (g_session->Awaiting(j.seat))
    {
        const Wait why = g_session->WaitReason(j.seat);
        if (why == Wait::NoSlot || why == Wait::Support || why == Wait::Unread)
        {
            g_session->Leave(j.seat);  // a wait that no moment of play ends: refused
            SetNote(0, text::Note::NoMember, j.seat);
            platform::Log(L"player window: player {}'s join refused: {}", j.seat + 1, party_member::Describe(why));
        }
        else
        {
            g_waiting[static_cast<std::size_t>(j.seat)] = true;
            SetWaitNote(j.seat);
            platform::Log(L"player window: player {} joins once its member is free ({})", j.seat + 1, party_member::Describe(why));
        }
        return;
    }
    SetNote(0, text::Note::Failed, j.seat);
    platform::Log(L"player window: player {}'s join failed (see the session's lines above)", j.seat + 1);
}

/// B held on `seat`'s pad with its window open: player 2 or 3 leaves; player 1 is refused.
void HoldLeave(int seat, int user)
{
    using hud::view_window::Sound;
    hud::view_window::Play(Sound::Cancel);
    if (seat == 0)
    {
        SetNote(0, text::Note::PlayerOneStays, 0);
        platform::Log(L"player window: {} held {}, but player 1 stays in", pad::PadName(user), pad::LeaveName(user));
        return;
    }
    if (std::find(g_leaves.begin(), g_leaves.end(), seat) == g_leaves.end())
    {
        for (int& s : g_leaves)
        {
            if (s < 0)
            {
                s = seat;
                break;
            }
        }
        platform::Log(L"player window: {} held {}: player {} leaves", pad::PadName(user), pad::LeaveName(user), seat + 1);
    }
}

/// On the title screen: the pad that pressed becomes player 1's, said on the game's save-data notice (the title's one
/// notice) for a few seconds, once per pad that takes the place (its own later presses, the title menu's, say nothing
/// more; another pad's press says that pad).
void TitlePress(int user, std::uint16_t pressed)
{
    // One pad read by both readers: its PlayStation copy takes player 1 (the game's own read of it: its button art,
    // trigger effects and light bar), its XInput copy stays muted beside it.
    if (pad::ReaderOf(user) == pad::kXInput && pressed != 0 &&
        pad_table::TwinOf(user, static_cast<std::uint16_t>(pressed & (~pressed + 1u))) >= 0)
    {
        return;
    }
    const auto table = pad_table::Table();
    if (pad::PadOn(table, 0) != user)
    {
        pad_table::Set(rules::WithPlayerOne(table, user), L"a pad pressed on the title screen: it plays player 1");
    }
    if (user != g_titleClaimed)
    {
        g_titleClaimed = user;
        hud::save_notice::Show(text::TitleClaimed(user), kTitleClaimedSeconds);
    }
}

/// One pad's input while its player's window is open.
void WindowInput(const Frame& frame, const Event& e, int seat)
{
    const auto at = static_cast<std::size_t>(e.user);
    if ((e.pressed & pad::kB) != 0)
    {
        g_bSeen[at] = true;
    }
    for (const std::uint16_t button : {rules::kDpadUp, rules::kDpadDown, rules::kDpadLeft, rules::kDpadRight, rules::kLB,
                                       rules::kRB, pad::kA, pad::kStart})
    {
        if ((e.pressed & button) == 0 || !g_open[static_cast<std::size_t>(seat)])
        {
            continue;
        }
        const rules::Action a = rules::OnPress(seat, button);
        switch (a.act)
        {
        case rules::Act::Step:
            if (g_session != nullptr && g_session->StepMember(seat, a.dir))
            {
                hud::view_window::Play(hud::view_window::Sound::Cursor);
                g_redraw = true;
                platform::Log(L"player window: player {} steps to {}", seat + 1, NameOf(seat));
            }
            else
            {
                platform::LogDebug(L"player window: player {}'s step changes nothing (no other party member free; in a fight, no "
                                   L"other player to trade with whose character and theirs are both standing and between actions; "
                                   L"or standing in for player 1's former character)",
                                   seat + 1);
            }
            break;
        case rules::Act::Trade:
            TradeNumber(frame, seat, a.dir);
            seat = SeatOfPad(e.user);  // the press's later buttons are the new number's
            if (seat < 0)
            {
                return;
            }
            break;
        case rules::Act::Move: MoveView(frame, seat, a.dir); break;
        case rules::Act::Close:
            hud::view_window::Play(hud::view_window::Sound::Decide);
            Close(seat, L"done");
            break;
        default: break;
        }
    }
    if (!g_bSeen[at] || !g_open[static_cast<std::size_t>(seat)])
    {
        return;
    }
    const rules::Action b = rules::OnB(seat, e.bTap, e.bHeld);
    if (b.act == rules::Act::Close)
    {
        hud::view_window::Play(hud::view_window::Sound::Cancel);
        Close(seat, L"B");
    }
    else if (b.act == rules::Act::Leave || b.act == rules::Act::Refused)
    {
        HoldLeave(seat, e.user);
    }
}

}  // namespace

bool Install() noexcept
{
    pad_table::Listen(&Note);
    g_installed = true;
    platform::Log(L"player window: Start on a pad that plays no one takes back a player whose pad is gone, else adds a "
                  L"player on it; View (Create on a PlayStation pad) held a second opens that player's own window (d-pad up/down: character, left/right: "
                  L"trade player numbers, LB/RB: move the view, A: done, B held: leave); on the title screen the pad that "
                  L"presses plays player 1");
    return true;
}

void SetSession(Session* session) noexcept
{
    g_session = session;
}

bool PlayerOneShown() noexcept
{
    return g_open[0] || g_notes[0].note != text::Note::None;
}

void RunRequests() noexcept
try
{
    if (!g_installed || g_session == nullptr || !AnyRequest())
    {
        return;
    }
    if (views::whole_screen::Shown())
    {
        return;  // a menu or a scene over the screen: the joins and leaves wait for the field
    }
    for (int& seat : g_leaves)
    {
        if (seat >= 1)
        {
            g_session->Leave(seat);
        }
        seat = -1;
    }
    for (JoinAsk& j : g_joins)
    {
        if (j.seat < 0)
        {
            continue;
        }
        pad_table::Set(rules::WithPadOn(pad_table::Table(), j.user, j.seat), L"a pad joins as a new player");
        g_session->Join(j.seat);
        AnswerJoin(j);
        j = JoinAsk{};
    }
}
catch (...)
{
    platform::Log(L"player window: an exception while joining or leaving; the requests are dropped");
    g_joins  = {};
    g_leaves = {-1, -1, -1};
}

void Remove() noexcept
{
    pad_table::Listen(nullptr);
    pad_table::SetIdlePads(0, L"the mod's end");
    hud::view_prompt::ClearAll();
    g_open           = {};
    g_notes          = {};
    g_waiting        = {};
    g_joins          = {};
    g_leaves         = {-1, -1, -1};
    g_startWhenSplit = -1;
    g_idlePads       = 0;
    g_installed      = false;
}

void Tick(const Frame& frame) noexcept
try
{
    if (!g_installed)
    {
        return;
    }
    const std::size_t count = g_queued;
    g_queued                = 0;
    if (g_dropped != 0)
    {
        platform::LogDebug(L"player window: {} pad press(es) dropped (more than {} in one tick)", g_dropped, kQueue);
        g_dropped = 0;
    }
    g_playing = frame.seatIds | 1u;
    hud::save_notice::Tick();  // the title's notice of which controller plays player 1, while it is due
    const players::game_state::Snapshot& state = players::game_state::Current();
    const bool title = state.valid && views::whole_screen::IsTitle(state.location);
    if (title != g_title)
    {
        g_title         = title;
        g_titleClaimed  = -1;  // the next visit's first press is said again
        if (!title)
        {
            hud::save_notice::End();
        }
        platform::LogDebug(L"player window: {} the title screen", title ? L"on" : L"off");
    }

    // Players gone (a leave, Ctrl+F8, a field exit) take their windows; one view (a scene, a menu) closes them all.
    for (int seat = 1; seat < kSeats; ++seat)
    {
        if (!Plays(seat))
        {
            Close(seat, L"the player is out");
            g_notes[static_cast<std::size_t>(seat)] = {};
        }
        // A join that waited is in now: its window opens. In or stopped, player 1's note of the wait goes.
        const bool wasWaiting = g_waiting[static_cast<std::size_t>(seat)];
        if (wasWaiting && Plays(seat))
        {
            g_waiting[static_cast<std::size_t>(seat)] = false;
            Open(seat, L"its join was waiting for a party member");
        }
        else if (wasWaiting && g_session != nullptr && !g_session->Awaiting(seat))
        {
            g_waiting[static_cast<std::size_t>(seat)] = false;  // the wait was stopped (Ctrl+F8, a field exit)
        }
        SeatNote& one = g_notes[0];
        if (wasWaiting && !g_waiting[static_cast<std::size_t>(seat)] && one.about == seat &&
            (one.note == text::Note::Waits || one.note == text::Note::WaitsOnAi))
        {
            one      = {};
            g_redraw = true;
        }
    }
    if (frame.single || title)
    {
        for (int seat = 0; seat < kSeats; ++seat)
        {
            Close(seat, frame.single ? L"the views became one (a scene or a menu)" : L"the title screen");
            if (g_notes[static_cast<std::size_t>(seat)].note != text::Note::None)
            {
                g_notes[static_cast<std::size_t>(seat)] = {};
                g_redraw                                = true;
            }
        }
    }
    // The views changed (a join's new view, a leave, a move): every window again in its player's view.
    if (frame.screens != g_lastScreens || frame.listSeats != g_lastList)
    {
        g_lastScreens = frame.screens;
        g_lastList    = frame.listSeats;
        g_redraw      = true;
    }

    // A spare pad's Start remembered over one view: the join once the views are split.
    if (g_startWhenSplit >= 0 && !frame.single && !views::whole_screen::Shown())
    {
        const int user   = g_startWhenSplit;
        g_startWhenSplit = -1;
        SpareStart(frame, user);
    }

    for (std::size_t i = 0; i < count; ++i)
    {
        const Event e = g_events[i];
        if (e.user < 0 || e.user >= pad::kPads)
        {
            continue;
        }
        if (title)
        {
            if (e.pressed != 0)
            {
                TitlePress(e.user, e.pressed);
            }
            continue;
        }
        const int seat = SeatOfPad(e.user);
        if (e.open)
        {
            if (seat < 0)
            {
                continue;  // a pad that plays no one: Start joins
            }
            if (g_open[static_cast<std::size_t>(seat)])
            {
                hud::view_window::Play(hud::view_window::Sound::Cancel);
                Close(seat, pad::ReaderOf(e.user) == pad::kPlayStation ? L"Create held again" : L"View held again");
            }
            else if (frame.single)
            {
                platform::Log(L"player window: {} held {}, but the views are one now (a scene or a menu)", pad::PadName(e.user),
                              pad::OpenerName(e.user));
            }
            else
            {
                g_bSeen[static_cast<std::size_t>(e.user)] = false;
                Open(seat, std::format(L"{} held {}", pad::PadName(e.user), pad::OpenerName(e.user)));
            }
            continue;
        }
        if (seat < 0)
        {
            if ((e.pressed & pad::kStart) != 0 && g_session != nullptr)
            {
                SpareStart(frame, e.user);
            }
            continue;
        }
        if (g_open[static_cast<std::size_t>(seat)])
        {
            WindowInput(frame, e, seat);
        }
    }

    // Notes run out.
    const std::int64_t now = NowMs();
    for (SeatNote& n : g_notes)
    {
        if (n.note != text::Note::None && now >= n.until)
        {
            n        = {};
            g_redraw = true;
        }
    }
    // The game closed a window under a player's (a script's clear of its message windows): shown again.
    if (hud::view_window::ClosedByGame() != g_gameClosedSeen)
    {
        g_gameClosedSeen = hud::view_window::ClosedByGame();
        g_redraw         = true;
    }
    // A member that cannot be taken now, or can again, under an open window: the arrows follow, a few times a second.
    if (++g_stepCheckTicks >= kStepCheckTicks && g_session != nullptr)
    {
        g_stepCheckTicks = 0;
        for (int seat = 1; seat < kSeats; ++seat)
        {
            const auto at = static_cast<std::size_t>(seat);
            if (g_open[at] && Plays(seat) && g_session->CanStepMember(seat) != g_stepShown[at])
            {
                g_redraw = true;
            }
        }
    }
    SetIdlePads();
    if (std::exchange(g_redraw, false))
    {
        ShowWindows(frame);
    }
}
catch (...)
{
    platform::Log(L"player window: an exception; every window closes");
    g_open = {};
    hud::view_window::CloseAll();
    hud::view_prompt::ClearAll();
    pad_table::SetIdlePads(0, L"the player windows' exception");
    g_idlePads = 0;
}

}  // namespace lss::players::player_window
