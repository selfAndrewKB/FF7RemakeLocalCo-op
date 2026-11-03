#pragma once

// The pure rules of the live pad assignment (players/pad_claim; plan/topics/input.md State "Planned", the user's
// design of 2026-10-30 18:43). Host-tested (tests/pad_claim_rules_test.cpp).
//
// A person is a pad and the screen in front of them: the screen first, then the character. Three tables:
//   - the pad table: the game's pad slot N (its controller id N, XInput user N as the game asks for it) answered by a
//     physical XInput user, always a permutation of 0..3;
//   - the screens: which player (seat 0..2) each screen position shows, left to right (in one window, three players:
//     the top, the bottom left, the bottom right), -1 for a screen no player has (a mirror's);
//   - the claims of one claim screen: which physical pad took each screen, and whether it is kept (locked).
// The steps:
//   - a pad takes a screen with that screen's button (screen 0 A, 1 X, 2 Y): it becomes that screen's player by
//     swapping pad slots with the pad there (the pad it displaced goes to the slot the pressing pad answered before),
//     so each player always has exactly one pad;
//   - on its screen the d-pad asks for the next party member no other human plays (the caller steps the player's
//     rank, players/party_rank_rules.h); player 1's character is the game's leader and does not step;
//   - A keeps it, B steps back (a kept screen unkept, a taken one let go); the claim ends when at most one screen is
//     not kept and one is (the last person takes what is left), or, while a screen is open, when every player's
//     screen is kept; the opener again (View held) puts back the tables from the opening;
//   - a spare pad (one on a slot no player holds) joins: an open screen's button (a mirror's screen) asks for a new
//     player there, Start for one on the first open screen (or, with none, a new view: one window); the caller makes
//     the player, and the claim re-bases on the new screens, the joiner's pad on the new player's screen;
//   - B held a second on a further player's pad asks for that player to leave (player 1 never does).
// Any player's screen may be taken by any pad, player 1's too; the screens themselves stay where they are.

#include <array>
#include <cstdint>

namespace lss::players::pad_claim_rules {

inline constexpr int kSlots   = 4;  // the game's pad slots and the physical XInput users: 0..3
inline constexpr int kScreens = 3;  // views laid out: player 1 and two further players

// XInput's button bits (XINPUT_GAMEPAD_*), kept here so the rules build without Windows headers.
inline constexpr std::uint16_t kDpadLeft  = 0x0004;
inline constexpr std::uint16_t kDpadRight = 0x0008;
inline constexpr std::uint16_t kStart     = 0x0010;  // START (Menu)
inline constexpr std::uint16_t kView      = 0x0020;  // BACK
inline constexpr std::uint16_t kA         = 0x1000;
inline constexpr std::uint16_t kB         = 0x2000;
inline constexpr std::uint16_t kX         = 0x4000;
inline constexpr std::uint16_t kY         = 0x8000;

/// Each screen position's button: the first A, the second X, the third Y.
inline constexpr std::array<std::uint16_t, kScreens> kScreenButton{kA, kX, kY};

/// The pad table: slot N's physical user. Identity until a claim changes it.
using PadTable = std::array<int, kSlots>;
inline constexpr PadTable kIdentity{0, 1, 2, 3};

constexpr bool IsPermutation(const PadTable& table) noexcept
{
    std::array<bool, kSlots> seen{};
    for (int slot = 0; slot < kSlots; ++slot)
    {
        const int user = table[static_cast<std::size_t>(slot)];
        if (user < 0 || user >= kSlots || seen[static_cast<std::size_t>(user)])
        {
            return false;
        }
        seen[static_cast<std::size_t>(user)] = true;
    }
    return true;
}

/// The slot `user` answers, or -1 (never for a permutation).
constexpr int SlotOf(const PadTable& table, int user) noexcept
{
    for (int slot = 0; slot < kSlots; ++slot)
    {
        if (table[static_cast<std::size_t>(slot)] == user)
        {
            return slot;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------------------------------------------------
// The opener: View held for kOpenHoldMs on one pad (the user, 2026-10-30 21:4x: "Scrap the view plus menu mapping";
// View+Menu raised Steam's on-screen keyboard over the game). View alone opens the map at its press, so a View press is
// held back from the game: let go before kOpenHoldMs, the game gets it then as a one-poll tap (the map opens at the
// let-go, a tap's length late); held kOpenHoldMs, the opener fires once and the game never sees that press.

inline constexpr std::uint16_t kOpenButton = kView;
inline constexpr std::int64_t  kOpenHoldMs = 1000;

struct OpenHold
{
    bool         down  = false;  // View is down and held back from the game
    bool         fired = false;  // the opener fired on this press: hidden until View is up
    std::int64_t since = 0;      // when it went down
};

struct Gated
{
    std::uint16_t buttons = 0;      // what the game sees
    bool          open    = false;  // the opener fired on this poll
};

/// One poll of one physical pad: `raw` its buttons, `nowMs` a steady clock.
constexpr Gated Gate(OpenHold& h, std::uint16_t raw, std::int64_t nowMs) noexcept
{
    const bool view = (raw & kOpenButton) != 0;
    auto       out  = static_cast<std::uint16_t>(raw & ~kOpenButton);
    if (!view)
    {
        // Let go: a press held back and not fired reaches the game as a tap (pressed on this poll, up on the next).
        if (h.down && !h.fired)
        {
            out = static_cast<std::uint16_t>(out | kOpenButton);
        }
        h = OpenHold{};
        return {out, false};
    }
    if (!h.down)
    {
        h.down  = true;
        h.since = nowMs;
    }
    if (!h.fired && nowMs - h.since >= kOpenHoldMs)
    {
        h.fired = true;
        return {out, true};
    }
    return {out, false};
}

// ---------------------------------------------------------------------------------------------------------------------
// A long press that is not held back from the game (the leave's B): it fires once after `ms` down, again only after a
// let-go.

struct Held
{
    bool         down  = false;
    bool         fired = false;
    std::int64_t since = 0;
};

constexpr bool Hold(Held& h, bool down, std::int64_t nowMs, std::int64_t ms) noexcept
{
    if (!down)
    {
        h = Held{};
        return false;
    }
    if (!h.down)
    {
        h.down  = true;
        h.since = nowMs;
    }
    if (!h.fired && nowMs - h.since >= ms)
    {
        h.fired = true;
        return true;
    }
    return false;
}

/// What the game sees of a pad on `slot` while `playing` (bit N: slot N holds a player): Start only on a player's slot
/// (slot 0 always), so a pad that plays no one never pauses the game; its Start joins instead (players/pad_claim).
constexpr std::uint16_t StartGate(std::uint16_t buttons, int slot, std::uint32_t playing) noexcept
{
    const bool player = slot == 0 || (slot >= 1 && slot < 32 && (playing & (1u << slot)) != 0);
    return player ? buttons : static_cast<std::uint16_t>(buttons & ~kStart);
}

// ---------------------------------------------------------------------------------------------------------------------
// The let-go: after the pads went idle or changed slots, a pad still holding what it held then stays idle to the game
// until it lets go of that, so the press that ended a claim never reaches the game. Only what was held at the first
// poll counts (a stuck or resting input never holds a pad for good): a button when its bit goes up, a trigger when it
// drops to the threshold or by the threshold from where it was. A trigger that still has not let go kLetGoCapMs after
// every button did (a worn trigger resting high) lets the pad go anyway, and so does a button still down
// kLetGoButtonCapMs after the change (a stuck bit; a hold that long is no press meant for the game).

inline constexpr std::uint8_t kTriggerThreshold = 30;  // XINPUT_GAMEPAD_TRIGGER_THRESHOLD
inline constexpr std::int64_t kLetGoCapMs       = 2000;
inline constexpr std::int64_t kLetGoButtonCapMs = 10000;

struct LetGo
{
    bool          waiting   = false;
    bool          armed     = false;  // the first poll's inputs are stored
    std::uint16_t held      = 0;      // buttons held at the first poll, not yet up
    bool          leftHeld  = false, rightHeld = false;
    std::uint8_t  leftAt    = 0, rightAt = 0;  // the triggers at the first poll
    std::int64_t  armedAt   = 0;               // the first poll
    std::int64_t  buttonsUp = -1;              // when every held button was up while a trigger still held
};

enum class Wait : std::uint8_t
{
    None,           // not waiting: the pad reaches the game
    Holding,        // still holding what it held: idle to the game
    LetGoNow,       // let go on this poll: idle once more (the last of a held-back tap is not delivered), then free
    TriggerRests,   // every button up, a trigger still high after the cap: idle once more, then free
    ButtonStuck,    // a button held at the change still down after its cap: idle once more, then free
};

constexpr void AwaitLetGo(LetGo& w) noexcept
{
    w = LetGo{.waiting = true};
}

constexpr bool TriggerLetGo(std::uint8_t now, std::uint8_t at) noexcept
{
    return now <= kTriggerThreshold || static_cast<int>(now) + kTriggerThreshold <= static_cast<int>(at);
}

/// One poll of a pad that may be waiting.
constexpr Wait Poll(LetGo& w, std::uint16_t buttons, std::uint8_t left, std::uint8_t right, std::int64_t nowMs) noexcept
{
    if (!w.waiting)
    {
        return Wait::None;
    }
    if (!w.armed)
    {
        w.armed     = true;
        w.armedAt   = nowMs;
        w.held      = buttons;
        w.leftAt    = left;
        w.rightAt   = right;
        w.leftHeld  = left > kTriggerThreshold;
        w.rightHeld = right > kTriggerThreshold;
    }
    w.held      = static_cast<std::uint16_t>(w.held & buttons);
    w.leftHeld  = w.leftHeld && !TriggerLetGo(left, w.leftAt);
    w.rightHeld = w.rightHeld && !TriggerLetGo(right, w.rightAt);
    if (w.held == 0 && !w.leftHeld && !w.rightHeld)
    {
        w = LetGo{};
        return Wait::LetGoNow;
    }
    if (w.held != 0)
    {
        w.buttonsUp = -1;
        if (nowMs - w.armedAt >= kLetGoButtonCapMs)
        {
            w = LetGo{};
            return Wait::ButtonStuck;
        }
        return Wait::Holding;
    }
    if (w.buttonsUp < 0)
    {
        w.buttonsUp = nowMs;
    }
    if (nowMs - w.buttonsUp >= kLetGoCapMs)
    {
        w = LetGo{};
        return Wait::TriggerRests;
    }
    return Wait::Holding;
}

// ---------------------------------------------------------------------------------------------------------------------
// The claim screen.

/// The leave's hold: B down this long on a further player's pad.
inline constexpr std::int64_t kLeaveHoldMs = 1000;

struct Claim
{
    int                         screens = 0;               // screens with a view: 1..kScreens
    std::array<int, kScreens>   seatOn{0, 1, 2};           // the player each screen shows (-1: none, a mirror)
    PadTable                    pads = kIdentity;          // the pad table
    std::uint32_t               playing = 1;               // the pad slots a player holds (bit N: slot N; bit 0 always)
    std::array<int, kScreens>   takenBy{-1, -1, -1};       // the physical user that took each screen, or -1
    std::array<bool, kScreens>  kept{};                    // the screen's choice is kept
    std::array<int, kScreens>   joiningBy{-1, -1, -1};     // the spare pad whose join onto each open screen is under way
    // The tables at the opening (or the last join or leave), put back by a cancel.
    std::array<int, kScreens>   openSeatOn{0, 1, 2};
    PadTable                    openPads = kIdentity;
};

constexpr Claim Open(int screens, const std::array<int, kScreens>& seatOn, const PadTable& pads,
                     std::uint32_t playing = 1) noexcept
{
    Claim c{};
    c.screens = screens < 1 ? 1 : screens > kScreens ? kScreens : screens;
    c.seatOn  = seatOn;
    c.pads    = pads;
    c.playing = playing | 1u;
    for (int s = c.screens; s < kScreens; ++s)
    {
        c.seatOn[static_cast<std::size_t>(s)] = -1;
    }
    c.openSeatOn = c.seatOn;
    c.openPads   = c.pads;
    return c;
}

/// `user`'s pad answers a slot no player holds: it plays no one, and may join.
constexpr bool Spare(const Claim& c, int user) noexcept
{
    const int slot = SlotOf(c.pads, user);
    return slot >= 1 && slot < 32 && (c.playing & (1u << slot)) == 0;
}

/// The first open screen (no player, no join under way), or -1.
constexpr int FirstOpen(const Claim& c) noexcept
{
    for (int s = 0; s < c.screens; ++s)
    {
        if (c.seatOn[static_cast<std::size_t>(s)] < 0 && c.joiningBy[static_cast<std::size_t>(s)] < 0)
        {
            return s;
        }
    }
    return -1;
}

/// Every player's screen taken by the pad that answers that player, and kept: the claim opened by a join, where only
/// the joiner chooses (each person may still press B to change).
constexpr void KeepAll(Claim& c) noexcept
{
    for (int s = 0; s < c.screens; ++s)
    {
        const int seat = c.seatOn[static_cast<std::size_t>(s)];
        if (seat >= 0)
        {
            c.takenBy[static_cast<std::size_t>(s)] = c.pads[static_cast<std::size_t>(seat)];
            c.kept[static_cast<std::size_t>(s)]    = true;
        }
    }
}

/// After a join or a leave: the claim on the screens now, each player's take and keep carried by player, a join under
/// way on a screen still open carried; the opening re-based (a cancel puts back what changed since).
constexpr Claim Rebase(const Claim& c, int screens, const std::array<int, kScreens>& seatOn, const PadTable& pads,
                       std::uint32_t playing) noexcept
{
    Claim n = Open(screens, seatOn, pads, playing);
    for (int s = 0; s < n.screens; ++s)
    {
        const auto at   = static_cast<std::size_t>(s);
        const int  seat = n.seatOn[at];
        if (seat < 0)
        {
            n.joiningBy[at] = s < c.screens && c.seatOn[at] < 0 ? c.joiningBy[at] : -1;
            continue;
        }
        for (int o = 0; o < c.screens; ++o)
        {
            if (c.seatOn[static_cast<std::size_t>(o)] == seat)
            {
                n.takenBy[at] = c.takenBy[static_cast<std::size_t>(o)];
                n.kept[at]    = c.kept[static_cast<std::size_t>(o)];
            }
        }
    }
    return n;
}

/// The screen `user` took in this claim, or -1.
constexpr int ScreenOf(const Claim& c, int user) noexcept
{
    for (int s = 0; s < c.screens; ++s)
    {
        if (c.takenBy[static_cast<std::size_t>(s)] == user)
        {
            return s;
        }
    }
    return -1;
}

/// The screen showing `seat`, or -1.
constexpr int ScreenShowing(const Claim& c, int seat) noexcept
{
    for (int s = 0; s < c.screens; ++s)
    {
        if (c.seatOn[static_cast<std::size_t>(s)] == seat)
        {
            return s;
        }
    }
    return -1;
}

/// The claim is over: no join under way, and every screen with a player kept, or, with no screen open, all but one
/// with at least one kept (the last person takes what is left; while a screen is open someone may still join there).
/// A single screen needs its keep.
constexpr bool Done(const Claim& c) noexcept
{
    int  players = 0, kept = 0;
    bool open    = false;
    for (int s = 0; s < c.screens; ++s)
    {
        const auto at = static_cast<std::size_t>(s);
        if (c.joiningBy[at] >= 0)
        {
            return false;
        }
        if (c.seatOn[at] >= 0)
        {
            ++players;
            kept += c.kept[at] ? 1 : 0;
        }
        else
        {
            open = true;
        }
    }
    return kept >= 1 && kept >= (open ? players : players - 1);
}

enum class Did : std::uint8_t
{
    Nothing,   // the press changes nothing (logged at Debug by the caller)
    Took,      // the pad took a screen (and that player's slot)
    Step,      // the pad asks for the next party member on its screen (`dir`); the caller steps the rank
    Kept,      // the pad's screen is kept
    Unkept,    // a kept screen is open again
    LetGo,     // the pad let its screen go
    Join,      // a spare pad asks for a new player on `screen` (-1: a new view, no screen open); the caller makes it
    Leave,     // a further player's pad held B: the caller takes `seat` out
    Refused,   // player 1's pad held B: player 1 never leaves (said on `screen`)
};

struct Result
{
    Did did    = Did::Nothing;
    int screen = -1;  // the screen it concerns
    int dir    = 0;   // Step: +1 right, -1 left
    int seat   = -1;  // Leave, Refused: the player
};

/// One button press (a down edge) of physical user `user`. `button` is one of the bits above.
constexpr Result Press(Claim& c, int user, std::uint16_t button) noexcept
{
    if (user < 0 || user >= kSlots)
    {
        return {};
    }
    const bool joining = c.joiningBy[0] == user || c.joiningBy[1] == user || c.joiningBy[2] == user;
    if (joining)
    {
        return {};  // a pad whose join is under way waits for it (it then takes the new player's screen)
    }
    if (button == kStart)
    {
        // A spare pad's Start: a new player on the first open screen, or a new view where none is open.
        if (!Spare(c, user))
        {
            return {};
        }
        const int s = FirstOpen(c);
        if (s >= 0)
        {
            c.joiningBy[static_cast<std::size_t>(s)] = user;
        }
        return {Did::Join, s};
    }
    const int mine = ScreenOf(c, user);
    if (mine < 0)
    {
        // Not on a screen yet: a screen's button takes it, if a player is there and no one else took it; an open
        // screen's button asks a spare pad's join there.
        for (int s = 0; s < c.screens; ++s)
        {
            const auto at   = static_cast<std::size_t>(s);
            const int  seat = c.seatOn[at];
            if (button == kScreenButton[at] && seat < 0 && c.joiningBy[at] < 0 && Spare(c, user))
            {
                c.joiningBy[at] = user;
                return {Did::Join, s};
            }
            if (button != kScreenButton[at] || seat < 0 || c.takenBy[at] >= 0)
            {
                continue;
            }
            const int from = SlotOf(c.pads, user);
            if (from < 0)
            {
                return {};
            }
            // The swap: the pad goes to the player's slot, the pad there to the slot this one answered.
            const int there                          = c.pads[static_cast<std::size_t>(seat)];
            c.pads[static_cast<std::size_t>(seat)]  = user;
            c.pads[static_cast<std::size_t>(from)]  = there;
            c.takenBy[static_cast<std::size_t>(s)]  = user;
            return {Did::Took, s};
        }
        return {};
    }
    const auto m = static_cast<std::size_t>(mine);
    if (button == kB)
    {
        if (c.kept[m])
        {
            c.kept[m] = false;
            return {Did::Unkept, mine};
        }
        c.takenBy[m] = -1;
        return {Did::LetGo, mine};
    }
    if (c.kept[m])
    {
        return {};
    }
    if (button == kA)
    {
        c.kept[m] = true;
        return {Did::Kept, mine};
    }
    if ((button != kDpadLeft && button != kDpadRight) || c.seatOn[m] < 1)
    {
        return {};  // player 1's character is the game's leader
    }
    return {Did::Step, mine, button == kDpadRight ? 1 : -1};
}

/// B held a second on physical user `user`'s pad: the player whose slot it answers leaves (player 1 never does).
constexpr Result Leave(const Claim& c, int user) noexcept
{
    const int slot = SlotOf(c.pads, user);
    if (slot < 0 || slot >= kScreens || (c.playing & (1u << slot)) == 0)
    {
        return {};
    }
    return {slot == 0 ? Did::Refused : Did::Leave, ScreenShowing(c, slot), 0, slot};
}

/// The opener again: the tables from the opening (or the last join or leave).
constexpr void Cancel(Claim& c) noexcept
{
    c.seatOn    = c.openSeatOn;
    c.pads      = c.openPads;
    c.takenBy   = {-1, -1, -1};
    c.kept      = {};
    c.joiningBy = {-1, -1, -1};
}

}  // namespace lss::players::pad_claim_rules
