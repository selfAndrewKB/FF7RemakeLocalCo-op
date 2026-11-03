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

#include <algorithm>
#include <array>
#include <cwctype>
#include <cstdint>
#include <format>
#include <string>

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
};

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
unsigned long long                   g_gameClosedSeen = 0;  // view_window's count of windows the game closed, as last shown

void Note(int user, std::uint16_t pressed, bool open) noexcept
{
    if (g_queued < kQueue)
    {
        g_events[g_queued++] = {user, pressed, open};
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

/// Each seat's character and whether its player can step now.
void ReadSeats()
{
    for (int seat = 0; seat < kScreens; ++seat)
    {
        g_seats.names[static_cast<std::size_t>(seat)]   = NameOf(seat);
        g_seats.canStep[static_cast<std::size_t>(seat)] = seat >= 1 && g_session != nullptr && g_session->CanStepMember(seat);
    }
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

/// Each screen's player now: the engine's list placed by the column order, as the layout places it.
std::array<int, kScreens> SeatOnScreens(const Frame& frame)
{
    std::array<int, kScreens> seatOn{-1, -1, -1};
    const int  screens = std::clamp(frame.screens, 1, kScreens);
    const auto slots   = views::column_rules::ScreenSlots(views::column_layout::Views(), views::column_layout::CurrentOrder(),
                                                          frame.listSeats);
    for (int place = 0; place < screens; ++place)
    {
        const int slot = slots[static_cast<std::size_t>(place)];
        if (slot >= 0 && slot < kScreens)
        {
            seatOn[static_cast<std::size_t>(slot)] = frame.listSeats[static_cast<std::size_t>(place)];
        }
    }
    return seatOn;
}

/// Each screen's text from the claim (players/pad_claim_text.h): in the game's message window in that player's view, or,
/// where no window can be shown, the engine-font prompt. A player no longer on a screen has neither.
void ShowPrompts()
{
    hud::view_prompt::ClearAll();
    std::array<bool, kScreens> shown{};  // by seat
    for (int s = 0; s < g_claim.screens; ++s)
    {
        const int seat = g_claim.seatOn[static_cast<std::size_t>(s)];
        if (seat < 0 || seat >= kScreens)
        {
            continue;
        }
        shown[static_cast<std::size_t>(seat)] = true;
        const pad_claim_text::ScreenText text = pad_claim_text::TextFor(g_claim, s, g_seats);
        if (!hud::view_window::Show(seat, text.markup))
        {
            hud::view_prompt::Prompt p;
            p.lines     = text.plain;
            p.highlight = 0;
            hud::view_prompt::Show(seat, p);
        }
    }
    for (int seat = 0; seat < kScreens; ++seat)
    {
        if (!shown[static_cast<std::size_t>(seat)])
        {
            hud::view_window::Close(seat);
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
    ClearPrompts();
    pad_table::SetIdle(false, L"the pad claim ended");
    platform::Log(L"pad claim: {}: {}", why, Summary(g_claim));
}

void Open(const Frame& frame, int user)
{
    if (frame.single)
    {
        platform::Log(L"pad claim: View held on pad {}, but the views are one now (a scene or a menu): hold it again once "
                      L"they are split",
                      user + 1);
        return;
    }
    const int screens = std::clamp(frame.screens, 1, kScreens);
    g_claim           = rules::Open(screens, SeatOnScreens(frame), pad_table::Table());
    g_openSeats       = SeatsShown(screens, frame.listSeats);
    g_openOrder       = views::column_layout::CurrentOrder();
    for (int seat = 1; seat < kScreens; ++seat)
    {
        g_openRanks[static_cast<std::size_t>(seat)] = party_member::Rank(seat);
    }
    ReadSeats();
    g_active         = true;
    g_gameClosedSeen = hud::view_window::ClosedByGame();
    pad_table::SetIdle(true, L"the pad claim");
    pad_table::LogDevices();
    platform::Log(L"pad claim: open (pad {} held View); {}", user + 1, Summary(g_claim));
    ShowPrompts();
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
    case rules::Did::Nothing:
        break;
    }
}

}  // namespace

bool Install() noexcept
{
    pad_table::Listen(&Note);
    g_installed = true;
    platform::Log(L"pad claim: View held a second on any pad opens the pad claim (each screen's button takes it, the "
                  L"d-pad picks the character, A keeps)");
    return true;
}

void SetSession(Session* session) noexcept
{
    g_session = session;
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
    g_installed = false;
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
    if (g_active)
    {
        const int screens = std::clamp(frame.screens, 1, kScreens);
        if (frame.single)
        {
            End(L"the views became one (a scene or a menu); ended with the choices so far", false);
        }
        else if (screens != g_claim.screens || SeatsShown(screens, frame.listSeats) != g_openSeats)
        {
            End(L"a player joined or left; ended with the choices so far", false);
        }
    }
    bool changed = false;
    bool opened  = false;  // presses of the poll that opened it reached the game too (idle starts at the next poll)
    for (std::size_t i = 0; i < count; ++i)
    {
        const Event e = g_events[i];
        if (e.open)
        {
            if (g_active)
            {
                End(L"cancelled (View held again): the pads and screens from the opening", true);
            }
            else
            {
                Open(frame, e.user);
                opened = g_active;
            }
            continue;
        }
        if (!g_active || opened)
        {
            continue;
        }
        for (const std::uint16_t button : {rules::kA, rules::kB, rules::kX, rules::kY, rules::kDpadLeft, rules::kDpadRight})
        {
            if ((e.pressed & button) != 0 && g_active)
            {
                Press(e.user, button);
                changed = true;
            }
        }
        if (g_active && rules::Done(g_claim))
        {
            End(L"done", false);
        }
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
