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
//     not kept and one is (the last person takes what is left), or the opener again (View held) puts back the tables
//     from the opening.
// Any player's screen may be taken by any pad, player 1's too; the screens themselves stay where they are.

#include <array>
#include <cstdint>

namespace lss::players::pad_claim_rules {

inline constexpr int kSlots   = 4;  // the game's pad slots and the physical XInput users: 0..3
inline constexpr int kScreens = 3;  // views laid out: player 1 and two further players

// XInput's button bits (XINPUT_GAMEPAD_*), kept here so the rules build without Windows headers.
inline constexpr std::uint16_t kDpadLeft  = 0x0004;
inline constexpr std::uint16_t kDpadRight = 0x0008;
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

struct Claim
{
    int                         screens = 0;               // screens with a view: 1..kScreens
    std::array<int, kScreens>   seatOn{0, 1, 2};           // the player each screen shows (-1: none, a mirror)
    PadTable                    pads = kIdentity;          // the pad table
    std::array<int, kScreens>   takenBy{-1, -1, -1};       // the physical user that took each screen, or -1
    std::array<bool, kScreens>  kept{};                    // the screen's choice is kept
    // The tables at the opening, put back by a cancel.
    std::array<int, kScreens>   openSeatOn{0, 1, 2};
    PadTable                    openPads = kIdentity;
};

constexpr Claim Open(int screens, const std::array<int, kScreens>& seatOn, const PadTable& pads) noexcept
{
    Claim c{};
    c.screens = screens < 1 ? 1 : screens > kScreens ? kScreens : screens;
    c.seatOn  = seatOn;
    c.pads    = pads;
    for (int s = c.screens; s < kScreens; ++s)
    {
        c.seatOn[static_cast<std::size_t>(s)] = -1;
    }
    c.openSeatOn = c.seatOn;
    c.openPads   = c.pads;
    return c;
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

/// The claim is over: every screen with a player kept, or all but one with at least one kept (the last person takes
/// what is left). A single screen needs its keep.
constexpr bool Done(const Claim& c) noexcept
{
    int players = 0, kept = 0;
    for (int s = 0; s < c.screens; ++s)
    {
        if (c.seatOn[static_cast<std::size_t>(s)] >= 0)
        {
            ++players;
            kept += c.kept[static_cast<std::size_t>(s)] ? 1 : 0;
        }
    }
    return kept >= 1 && kept >= players - 1;
}

enum class Did : std::uint8_t
{
    Nothing,   // the press changes nothing (logged at Debug by the caller)
    Took,      // the pad took a screen (and that player's slot)
    Step,      // the pad asks for the next party member on its screen (`dir`); the caller steps the rank
    Kept,      // the pad's screen is kept
    Unkept,    // a kept screen is open again
    LetGo,     // the pad let its screen go
};

struct Result
{
    Did did    = Did::Nothing;
    int screen = -1;  // the screen it concerns
    int dir    = 0;   // Step: +1 right, -1 left
};

/// One button press (a down edge) of physical user `user`. `button` is one of the bits above.
constexpr Result Press(Claim& c, int user, std::uint16_t button) noexcept
{
    if (user < 0 || user >= kSlots)
    {
        return {};
    }
    const int mine = ScreenOf(c, user);
    if (mine < 0)
    {
        // Not on a screen yet: a screen's button takes it, if a player is there and no one else took it.
        for (int s = 0; s < c.screens; ++s)
        {
            const int seat = c.seatOn[static_cast<std::size_t>(s)];
            if (button != kScreenButton[static_cast<std::size_t>(s)] || seat < 0 || c.takenBy[static_cast<std::size_t>(s)] >= 0)
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

/// The opener again: the tables from the opening.
constexpr void Cancel(Claim& c) noexcept
{
    c.seatOn  = c.openSeatOn;
    c.pads    = c.openPads;
    c.takenBy = {-1, -1, -1};
    c.kept    = {};
}

}  // namespace lss::players::pad_claim_rules
