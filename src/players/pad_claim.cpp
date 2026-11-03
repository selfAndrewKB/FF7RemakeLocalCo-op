#include "players/pad_claim.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>

#include "engine_facts.h"
#include "hud/view_prompt.h"
#include "hud/view_window.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "players/field_root.h"
#include "players/pad_claim_rules.h"
#include "players/pad_claim_text.h"
#include "players/party_member.h"
#include "players/pad_table.h"
#include "players/scopes.h"
#include "players/seats.h"
#include "players/session.h"
#include "views/column_layout.h"
#include "views/whole_screen.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace lss::players::pad_claim {

namespace {

namespace rules = pad_claim_rules;
using views::column_rules::ListSeats;

constexpr int kScreens = rules::kScreens;

struct Event
{
    int           user    = 0;
    std::uint16_t pressed = 0;
    bool          open    = false;
    bool          bTap    = false;  // B let go within a second: Back
    bool          bHeld   = false;  // B held a second: the leave
};

/// A join the claim asked for, made by RunRequests before the session's tick.
struct JoinAsk
{
    int seat   = -1;
    int user   = -1;  // the joining pad
    int screen = -1;  // the open screen it goes on, -1 a new view (one window)
};

/// A line a screen shows for a while: a refusal, or a join that waits.
struct ScreenNote
{
    pad_claim_text::Note note  = pad_claim_text::Note::None;
    std::int64_t         until = 0;
};

constexpr std::int64_t kNoteMs = 3000;

// Noted by the pad table's listener during the engine's poll, taken in Tick. Game thread both.
constexpr std::size_t       kQueue = 32;
std::array<Event, kQueue>   g_events{};
std::size_t                 g_queued = 0;
std::uint32_t               g_dropped = 0;

// Game thread.
bool                                 g_installed = false;
bool                                 g_active    = false;
Session*                             g_session   = nullptr;
rules::Claim                         g_claim{};
std::array<int, kScreens>            g_openSeats{};  // the seats on screens at the opening, sorted (a join or leave ends it)
pad_claim_text::Seats                g_seats{};      // each seat's character and whether it can step, as last read
views::column_rules::Order           g_openOrder{};  // the column order at the opening, put back by a cancel
std::array<int, kScreens>            g_openRanks{};  // each further seat's rank at the opening, put back by a cancel
std::array<int, kScreens>            g_columnOn{-1, -1, -1};  // each screen's column (its place in the engine's list)
std::array<JoinAsk, kScreens>        g_joins{};      // asked, not yet made (seat -1: none)
std::array<int, kScreens>            g_leaves{-1, -1, -1};  // players asked to leave, not yet out
std::array<int, kScreens>            g_joinedBy{-1, -1, -1};  // by seat: the pad that joined it, to take its screen
std::array<ScreenNote, kScreens>     g_notes{};      // by screen
std::array<bool, rules::kSlots>      g_bSeen{};      // by pad: B pressed while the claim is open (arms its tap and hold)
int                                  g_startWhenSplit = -1;  // a spare pad's Start during one view: joins once split
bool                                 g_redraw = false;  // a join or leave answered outside Tick: the prompts again
unsigned long long                   g_gameClosedSeen = 0;  // view_window's count of windows the game closed, as last shown

std::int64_t NowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

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

/// The seat's character in capitals, from its class ("PC0001_00_Barret_Standard_C": BARRET), else "PLAYER n". A
/// further seat's is its row's (the rank's, so a step shows at once; the member follows a tick later).
std::wstring NameOf(int seat)
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
    return std::format(L"PLAYER {}", seat + 1);
}

std::wstring ScreenName(int screen, int screens)
{
    constexpr std::array<const wchar_t*, kScreens> kOf3{L"the first screen", L"the second screen", L"the third screen"};
    constexpr std::array<const wchar_t*, 2>        kOf2{L"the first screen", L"the second screen"};
    if (screens == 1)
    {
        return L"the screen";
    }
    return screens == 2 ? kOf2[static_cast<std::size_t>(screen & 1)] : kOf3[static_cast<std::size_t>(screen % kScreens)];
}

std::wstring Character(int seat)
{
    return seat >= 0 && seat < kScreens ? g_seats.names[static_cast<std::size_t>(seat)] : std::wstring(L"nobody");
}

int FreeSeat();

/// Each seat's character and whether its player can step now; whether Start can add a view (no screen open, a seat
/// free).
void ReadSeats()
{
    for (int seat = 0; seat < kScreens; ++seat)
    {
        g_seats.names[static_cast<std::size_t>(seat)]   = NameOf(seat);
        g_seats.canStep[static_cast<std::size_t>(seat)] = seat >= 1 && g_session != nullptr && g_session->CanStepMember(seat);
    }
    g_seats.startJoins = rules::FirstOpen(g_claim) < 0 && FreeSeat() >= 0 &&
                         std::none_of(g_claim.seatOn.begin(), g_claim.seatOn.begin() + g_claim.screens, [](int s) { return s < 0; });
}

/// "the first screen CLOUD (pad 2), the second screen BARRET (pad 1)".
std::wstring Summary(const rules::Claim& c)
{
    std::wstring text;
    for (int s = 0; s < c.screens; ++s)
    {
        const int seat = c.seatOn[static_cast<std::size_t>(s)];
        if (seat < 0)
        {
            continue;
        }
        text += std::format(L"{}{} {} (pad {}){}", text.empty() ? L"" : L", ", ScreenName(s, c.screens), Character(seat),
                            c.pads[static_cast<std::size_t>(seat)] + 1, c.kept[static_cast<std::size_t>(s)] ? L", kept" : L"");
    }
    return text.empty() ? L"no player" : text;
}

/// The seats on screens, sorted, -1 for none: what a join or a leave changes.
std::array<int, kScreens> SeatsShown(int screens, const ListSeats& seats)
{
    std::array<int, kScreens> shown{-1, -1, -1};
    for (int place = 0; place < screens && place < kScreens; ++place)
    {
        shown[static_cast<std::size_t>(place)] = seats[static_cast<std::size_t>(place)];
    }
    std::sort(shown.begin(), shown.end());
    return shown;
}

/// Each screen's player now: the engine's list placed by the column order, as the layout places it. `columnOn`: each
/// screen's place in the list (its column), -1 for none.
std::array<int, kScreens> SeatOnScreens(const Frame& frame, std::array<int, kScreens>* columnOn = nullptr)
{
    std::array<int, kScreens> seatOn{-1, -1, -1};
    std::array<int, kScreens> column{-1, -1, -1};
    const int  screens = std::clamp(frame.screens, 1, kScreens);
    const auto slots   = views::column_rules::ScreenSlots(views::column_layout::Views(), views::column_layout::CurrentOrder(),
                                                          frame.listSeats);
    for (int place = 0; place < screens; ++place)
    {
        const int slot = slots[static_cast<std::size_t>(place)];
        if (slot >= 0 && slot < kScreens)
        {
            seatOn[static_cast<std::size_t>(slot)] = frame.listSeats[static_cast<std::size_t>(place)];
            column[static_cast<std::size_t>(slot)] = place;
        }
    }
    if (columnOn != nullptr)
    {
        *columnOn = column;
    }
    return seatOn;
}

/// A further seat no player holds, waits for, rejoins or is joining now (the lowest), or -1.
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

/// A note on `screen`, or, for a join that had no screen (a new view in one window), on player 1's.
void SetNote(int screen, pad_claim_text::Note note)
{
    screen = screen >= 0 ? screen : rules::ScreenShowing(g_claim, 0);
    if (screen >= 0 && screen < kScreens)
    {
        g_notes[static_cast<std::size_t>(screen)] = {note, NowMs() + kNoteMs};
    }
}

bool AnyNote()
{
    return std::any_of(g_notes.begin(), g_notes.end(), [](const ScreenNote& n) { return n.note != pad_claim_text::Note::None; });
}

bool AnyRequest()
{
    return std::any_of(g_joins.begin(), g_joins.end(), [](const JoinAsk& j) { return j.seat >= 0; }) ||
           std::any_of(g_leaves.begin(), g_leaves.end(), [](int seat) { return seat >= 0; });
}

/// Each screen's text from the claim (players/pad_claim_text.h): in the game's message window in that screen's view (a
/// player's, or a copy's own: hud/view_window MirrorView), or, where no window can be shown, the engine-font prompt (a
/// player's view only). A view no longer on a screen has neither.
void ShowPrompts()
{
    constexpr int kViews = hud::view_window::kViews;
    hud::view_prompt::ClearAll();
    std::array<bool, kViews> shown{};
    for (int s = 0; s < g_claim.screens; ++s)
    {
        const auto at     = static_cast<std::size_t>(s);
        const int  seat   = g_claim.seatOn[at];
        const int  column = g_columnOn[at];
        const int  view   = seat >= 0 ? seat : column >= 1 ? hud::view_window::MirrorView(column) : -1;
        if (view < 0 || view >= kViews || seat >= kScreens)
        {
            continue;
        }
        shown[static_cast<std::size_t>(view)] = true;
        const pad_claim_text::ScreenText text = pad_claim_text::TextFor(g_claim, s, g_seats, g_notes[at].note);
        if (!hud::view_window::Show(view, text.markup) && seat >= 0)
        {
            hud::view_prompt::Prompt p;
            p.lines     = text.plain;
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

/// Every prompt gone: the windows close the game's way (Out and its sound).
void ClearPrompts() noexcept
{
    hud::view_window::CloseAll();
    hud::view_prompt::ClearAll();
}

void End(const wchar_t* why, bool putBack)
{
    if (putBack)
    {
        g_joins = {};  // asked, not yet made: dropped with the rest
        rules::Cancel(g_claim);
        pad_table::Set(g_claim.pads, L"the pad claim, put back");
        if (!(views::column_layout::CurrentOrder() == g_openOrder))
        {
            views::column_layout::ChangeOrder(g_openOrder, L"the pad claim, put back");  // the order itself, join order included
        }
        // The characters stepped to: each seat's rank back (the opening's ranks were distinct, so they are again); the
        // session's follow gives the member back and seats the old one, as after a step.
        for (int seat = 1; seat < kScreens; ++seat)
        {
            const int rank = g_openRanks[static_cast<std::size_t>(seat)];
            if (party_member::Rank(seat) != rank)
            {
                party_member::SetRank(seat, rank);
                platform::Log(L"pad claim: player {} back on rank {} of the party order", seat + 1, rank);
            }
        }
    }
    g_active = false;
    g_notes  = {};
    ClearPrompts();
    pad_table::SetIdle(false, L"the pad claim ended");
    platform::Log(L"pad claim: {}: {}", why, Summary(g_claim));
}

/// The opening's tables (what a cancel puts back): the screens, the order and the ranks now.
void TakeOpening(const Frame& frame)
{
    g_openSeats = SeatsShown(std::clamp(frame.screens, 1, kScreens), frame.listSeats);
    g_openOrder = views::column_layout::CurrentOrder();
    for (int seat = 1; seat < kScreens; ++seat)
    {
        g_openRanks[static_cast<std::size_t>(seat)] = party_member::Rank(seat);
    }
}

/// `why` for the log: "pad 2 held View".
void Open(const Frame& frame, const std::wstring& why)
{
    if (frame.single)
    {
        platform::Log(L"pad claim: {}, but the views are one now (a scene or a menu): hold View again once they are split", why);
        return;
    }
    const int screens = std::clamp(frame.screens, 1, kScreens);
    g_claim           = rules::Open(screens, SeatOnScreens(frame, &g_columnOn), pad_table::Table(), frame.seatIds);
    TakeOpening(frame);
    g_notes    = {};
    g_bSeen    = {};
    g_joinedBy = {-1, -1, -1};
    ReadSeats();
    g_active         = true;
    g_gameClosedSeen = hud::view_window::ClosedByGame();
    pad_table::SetIdle(true, L"the pad claim");
    pad_table::LogDevices();
    platform::Log(L"pad claim: open ({}); {}", why, Summary(g_claim));
    ShowPrompts();
}

/// A join or a leave changed the screens: the claim goes on over them, each player's take and keep carried, a joiner's
/// pad on its new player's screen; the opening re-based.
void Rebase(const Frame& frame)
{
    const int screens = std::clamp(frame.screens, 1, kScreens);
    g_claim           = rules::Rebase(g_claim, screens, SeatOnScreens(frame, &g_columnOn), pad_table::Table(), frame.seatIds);
    for (int seat = 1; seat < kScreens; ++seat)
    {
        const int user   = g_joinedBy[static_cast<std::size_t>(seat)];
        const int screen = rules::ScreenShowing(g_claim, seat);
        if (user >= 0 && screen >= 0)
        {
            g_claim.takenBy[static_cast<std::size_t>(screen)] = user;
            g_claim.kept[static_cast<std::size_t>(screen)]    = false;
            g_notes[static_cast<std::size_t>(screen)]         = {};
            g_joinedBy[static_cast<std::size_t>(seat)]        = -1;
        }
    }
    TakeOpening(frame);
    platform::Log(L"pad claim: the players on the screens changed; it goes on: {}", Summary(g_claim));
}

/// A spare pad's Start outside the claim: the claim opens with everyone in kept (only the joiner chooses; anyone may
/// still press B to change); the caller then presses the Start into it.
void OpenForJoin(const Frame& frame, int user)
{
    Open(frame, std::format(L"pad {} pressed Start to join", user + 1));
    if (g_active)
    {
        rules::KeepAll(g_claim);
    }
}

/// One pad's press on the open claim.
void Press(int user, std::uint16_t button)
{
    const rules::Claim before = g_claim;
    const rules::Result r     = rules::Press(g_claim, user, button);
    if (r.did == rules::Did::Nothing)
    {
        platform::LogDebug(L"pad claim: pad {}'s press {:#06x} changes nothing", user + 1, button);
        return;
    }
    if (g_claim.pads != before.pads)
    {
        pad_table::Set(g_claim.pads, L"the pad claim");
    }
    const int    seat  = r.screen >= 0 ? g_claim.seatOn[static_cast<std::size_t>(r.screen)] : -1;
    const auto   where = ScreenName(r.screen, g_claim.screens);
    // Each step with the game's own menu sound for it: a take or a keep decides, a step moves, a let-go cancels.
    using hud::view_window::Sound;
    switch (r.did)
    {
    case rules::Did::Took:
        hud::view_window::Play(Sound::Decide);
        platform::Log(L"pad claim: pad {} took {}: it plays player {} ({})", user + 1, where, seat + 1, Character(seat));
        break;
    case rules::Did::Step:
        if (g_session == nullptr || !g_session->StepMember(seat, r.dir))
        {
            platform::LogDebug(L"pad claim: pad {}'s step on {} changes nothing (no other party member free, in battle, or "
                               L"standing in for player 1's former character)",
                               user + 1, where);
            break;
        }
        hud::view_window::Play(Sound::Cursor);
        ReadSeats();
        platform::Log(L"pad claim: pad {} stepped to {} on {}", user + 1, Character(seat), where);
        break;
    case rules::Did::Kept:
        hud::view_window::Play(Sound::Decide);
        platform::Log(L"pad claim: pad {} keeps {} on {}", user + 1, Character(seat), where);
        break;
    case rules::Did::Unkept:
        hud::view_window::Play(Sound::Cancel);
        platform::Log(L"pad claim: pad {} opens {} again ({})", user + 1, where, Character(seat));
        break;
    case rules::Did::LetGo:
        hud::view_window::Play(Sound::Cancel);
        platform::Log(L"pad claim: pad {} lets {} go", user + 1, where);
        break;
    case rules::Did::Join:
    {
        const bool openPending = std::any_of(g_claim.seatOn.begin(), g_claim.seatOn.begin() + g_claim.screens,
                                             [](int shown) { return shown < 0; });
        const bool asked = std::any_of(g_joins.begin(), g_joins.end(), [user](const JoinAsk& j) { return j.user == user; });
        if (asked || (r.screen < 0 && openPending))
        {
            if (r.screen >= 0 && asked)
            {
                g_claim.joiningBy[static_cast<std::size_t>(r.screen)] = -1;
            }
            platform::LogDebug(L"pad claim: pad {}'s join press: it already joins, or every open screen has a join under way", user + 1);
            break;  // a new view only where no screen is open (one window); the open ones are being joined
        }
        const int free = FreeSeat();
        if (free < 0 || (r.screen < 0 && g_claim.screens >= kScreens))
        {
            if (r.screen >= 0)
            {
                g_claim.joiningBy[static_cast<std::size_t>(r.screen)] = -1;
            }
            SetNote(r.screen, pad_claim_text::Note::AllIn);
            hud::view_window::Play(Sound::Cancel);
            platform::Log(L"pad claim: pad {} asked to join{}, but every player is in or on the way", user + 1,
                          r.screen >= 0 ? L" on " + where : std::wstring());
            break;
        }
        for (JoinAsk& j : g_joins)
        {
            if (j.seat < 0)
            {
                j = {free, user, r.screen};
                break;
            }
        }
        hud::view_window::Play(Sound::Decide);
        platform::Log(L"pad claim: pad {} joins as player {}{}", user + 1, free + 1,
                      r.screen >= 0 ? L" on " + where : std::wstring(L" (a new view)"));
        break;
    }
    case rules::Did::Leave:
    case rules::Did::Refused:
    case rules::Did::Nothing:
        break;
    }
}

/// B held a second on a pad whose B the claim saw pressed: the pad's player leaves (player 1 never does).
void HoldLeave(int user)
{
    if (user < 0 || user >= rules::kSlots || !g_bSeen[static_cast<std::size_t>(user)])
    {
        return;
    }
    const rules::Result r = rules::Leave(g_claim, user);
    using hud::view_window::Sound;
    if (r.did == rules::Did::Refused)
    {
        SetNote(r.screen, pad_claim_text::Note::PlayerOneStays);
        hud::view_window::Play(Sound::Cancel);
        platform::Log(L"pad claim: pad {} held B, but player 1 stays in", user + 1);
    }
    else if (r.did == rules::Did::Leave && std::find(g_leaves.begin(), g_leaves.end(), r.seat) == g_leaves.end())
    {
        for (int& seat : g_leaves)
        {
            if (seat < 0)
            {
                seat = r.seat;
                break;
            }
        }
        hud::view_window::Play(Sound::Cancel);
        platform::Log(L"pad claim: pad {} held B: player {} leaves", user + 1, r.seat + 1);
    }
}

/// The session's answer to a join: in (its pad takes the new screen at the re-base), waiting for a member that is busy
/// now, or refused (no member free: the wait stopped).
void AnswerJoin(const JoinAsk& j)
{
    g_redraw = true;  // the screen's "Joining..." gives way to its note or its new player
    using party_member::Wait;
    if (j.screen >= 0)
    {
        g_claim.joiningBy[static_cast<std::size_t>(j.screen)] = -1;
    }
    if (g_session->In(j.seat))
    {
        g_joinedBy[static_cast<std::size_t>(j.seat)] = j.user;
        return;
    }
    if (g_session->Awaiting(j.seat))
    {
        const Wait why = g_session->WaitReason(j.seat);
        if (why == Wait::NoSlot || why == Wait::Support || why == Wait::Unread)
        {
            g_session->Leave(j.seat);  // a wait that no moment of play ends: refused, said on the screen
            SetNote(j.screen, pad_claim_text::Note::NoMember);
            platform::Log(L"pad claim: player {}'s join refused: {}", j.seat + 1, party_member::Describe(why));
        }
        else
        {
            g_joinedBy[static_cast<std::size_t>(j.seat)] = j.user;  // its screen is its pad's once the member frees up
            SetNote(j.screen, pad_claim_text::Note::Waits);
            platform::Log(L"pad claim: player {} joins once its member is free ({})", j.seat + 1, party_member::Describe(why));
        }
        return;
    }
    SetNote(j.screen, pad_claim_text::Note::Failed);
    platform::Log(L"pad claim: player {}'s join failed (see the session's lines above)", j.seat + 1);
}

}  // namespace

bool Install() noexcept
{
    pad_table::Listen(&Note);
    g_installed = true;
    platform::Log(L"pad claim: View held a second on any pad opens the pad claim (each screen's button takes it, the "
                  L"d-pad picks the character, A selects, B let go steps back, B held a second leaves); Start on a pad that "
                  L"plays no one joins, "
                  L"and so does a copy's screen's button in the claim");
    return true;
}

void SetSession(Session* session) noexcept
{
    g_session = session;
}

bool Active() noexcept
{
    return g_active;
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
        // The joiner's pad onto the new player's slot (the pad there to the slot the joiner answered).
        rules::PadTable table = pad_table::Table();
        const int       from  = rules::SlotOf(table, j.user);
        if (from >= 0 && from != j.seat)
        {
            std::swap(table[static_cast<std::size_t>(from)], table[static_cast<std::size_t>(j.seat)]);
            pad_table::Set(table, L"a pad joins");
            g_claim.pads = pad_table::Table();
        }
        // The new view on the screen chosen: the column order shows that player there (put back if the join is
        // refused).
        const views::column_rules::Order before = views::column_layout::CurrentOrder();
        if (j.screen >= 0 && g_claim.screens >= 2)
        {
            std::array<int, kScreens> seatOn = g_claim.seatOn;
            seatOn[static_cast<std::size_t>(j.screen)] = j.seat;
            if (const auto order = views::column_rules::OrderShowing(seatOn); order && !(*order == views::column_layout::CurrentOrder()))
            {
                views::column_layout::ChangeOrder(*order, L"a pad joins on that screen");
            }
        }
        g_session->Join(j.seat);
        AnswerJoin(j);
        if (!g_session->In(j.seat) && !g_session->Awaiting(j.seat) && !(views::column_layout::CurrentOrder() == before))
        {
            views::column_layout::ChangeOrder(before, L"the join was refused: put back");
        }
        j = JoinAsk{};
    }
}
catch (...)
{
    platform::Log(L"pad claim: an exception while joining or leaving; the requests are dropped");
    g_joins  = {};
    g_leaves = {-1, -1, -1};
}

void Remove() noexcept
{
    pad_table::Listen(nullptr);
    if (g_active)
    {
        g_active = false;
        hud::view_prompt::ClearAll();
        pad_table::SetIdle(false, L"the mod's end");
    }
    g_joins          = {};
    g_leaves         = {-1, -1, -1};
    g_startWhenSplit = -1;
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
        platform::LogDebug(L"pad claim: {} pad press(es) dropped (more than {} in one tick)", g_dropped, kQueue);
        g_dropped = 0;
    }
    bool changed = std::exchange(g_redraw, false);
    if (g_active)
    {
        const int screens = std::clamp(frame.screens, 1, kScreens);
        if (frame.single)
        {
            End(L"the views became one (a scene or a menu); ended with the choices so far", false);
        }
        else if (screens != g_claim.screens || SeatsShown(screens, frame.listSeats) != g_openSeats ||
                 (frame.seatIds | 1u) != g_claim.playing)
        {
            Rebase(frame);
            changed = true;
        }
    }
    // A spare pad's Start remembered over one view (a scene or a menu): the join once the views are split.
    if (!g_active && g_startWhenSplit >= 0 && !frame.single && !views::whole_screen::Shown())
    {
        const int user   = g_startWhenSplit;
        g_startWhenSplit = -1;
        OpenForJoin(frame, user);
        if (g_active)
        {
            Press(user, rules::kStart);
            changed = true;
        }
    }
    bool opened = false;  // presses of the poll that opened it reached the game too (idle starts at the next poll)
    for (std::size_t i = 0; i < count; ++i)
    {
        const Event e = g_events[i];
        if (e.open)
        {
            if (g_active)
            {
                End(L"cancelled (View held again): the pads, screens and characters from the opening or the last join", true);
            }
            else
            {
                Open(frame, std::format(L"pad {} held View", e.user + 1));
                opened = g_active;
            }
            continue;
        }
        // A spare pad's Start with the claim shut: the claim opens onto its join (a scene or a menu: remembered).
        if (!g_active && (e.pressed & rules::kStart) != 0 && g_session != nullptr)
        {
            const rules::Claim probe = rules::Open(1, {0, -1, -1}, pad_table::Table(), frame.seatIds);
            if (rules::Spare(probe, e.user) && FreeSeat() >= 0)
            {
                if (frame.single || views::whole_screen::Shown())
                {
                    g_startWhenSplit = e.user;
                    platform::Log(L"pad claim: pad {} pressed Start to join during one view; it joins once the views split",
                                  e.user + 1);
                }
                else
                {
                    OpenForJoin(frame, e.user);
                    if (g_active)
                    {
                        Press(e.user, rules::kStart);
                        changed = true;
                    }
                }
            }
            continue;
        }
        if (!g_active || opened)
        {
            continue;
        }
        const bool known = e.user >= 0 && e.user < rules::kSlots;
        if ((e.pressed & rules::kB) != 0 && known)
        {
            g_bSeen[static_cast<std::size_t>(e.user)] = true;
        }
        // B's own press waits for its let-go: Back then, or the leave's hold instead, never both.
        for (const std::uint16_t button : {rules::kA, rules::kX, rules::kY, rules::kDpadLeft, rules::kDpadRight, rules::kStart})
        {
            if ((e.pressed & button) != 0 && g_active)
            {
                Press(e.user, button);
                changed = true;
            }
        }
        if (e.bTap && g_active && known && g_bSeen[static_cast<std::size_t>(e.user)])
        {
            Press(e.user, rules::kB);
            changed = true;
        }
        if (e.bHeld && g_active)
        {
            HoldLeave(e.user);
            changed = true;
        }
    }
    // Notes run out; the claim ends once everything is kept and nothing is under way or still said.
    const std::int64_t now = NowMs();
    for (ScreenNote& n : g_notes)
    {
        if (n.note != pad_claim_text::Note::None && now >= n.until)
        {
            n       = {};
            changed = true;
        }
    }
    if (g_active && rules::Done(g_claim) && !AnyRequest() && !AnyNote())
    {
        End(L"done", false);
    }
    // The game closed a window under the claim (a script's clear of its message windows): shown again.
    if (g_active && hud::view_window::ClosedByGame() != g_gameClosedSeen)
    {
        g_gameClosedSeen = hud::view_window::ClosedByGame();
        platform::Log(L"pad claim: the game closed a prompt window; the prompts are shown again");
        changed = true;
    }
    if (g_active && changed)
    {
        ReadSeats();  // a battle start or a party change since the last press: the arrows follow
        ShowPrompts();
    }
}
catch (...)
{
    platform::Log(L"pad claim: an exception; the claim ends with the choices so far");
    g_active = false;
    ClearPrompts();
    pad_table::SetIdle(false, L"the pad claim's exception");
}

}  // namespace lss::players::pad_claim
