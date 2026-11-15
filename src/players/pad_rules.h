#pragma once

// The pure rules of each physical pad's input, as the pad table reads it (players/pad_table) and the player windows
// hear it (players/player_window). Host-tested (tests/pad_rules_test.cpp).
//   - the pad table: the game's pad slot N (its controller id N, XInput user N as the game asks for it) answered by a
//     physical XInput user, always a permutation of 0..3; slot N is player N's (seat N), slot 3 the spare;
//   - the window's opener, View held a second: a shorter View press still reaches the game, as a tap at its let-go;
//   - B as a tap (let go within a second) or a hold (a second down), never both;
//   - Start only on a slot a player holds: a pad that plays no one never pauses the game (its Start joins instead);
//   - the let-go: a pad that changed slots or left a window stays idle to the game until it lets go of what it held.

#include <array>
#include <cstdint>

namespace lss::players::pad_rules {

inline constexpr int kSlots   = 4;  // the game's pad slots and the physical XInput users: 0..3

// XInput's button bits (XINPUT_GAMEPAD_*), kept here so the rules build without Windows headers.
inline constexpr std::uint16_t kDpadLeft  = 0x0004;
inline constexpr std::uint16_t kDpadRight = 0x0008;
inline constexpr std::uint16_t kStart     = 0x0010;  // START (Menu)
inline constexpr std::uint16_t kView      = 0x0020;  // BACK
inline constexpr std::uint16_t kA         = 0x1000;
inline constexpr std::uint16_t kB         = 0x2000;
inline constexpr std::uint16_t kX         = 0x4000;
inline constexpr std::uint16_t kY         = 0x8000;

/// The pad table: slot N's physical user. Identity until a join, the title screen or the console changes it.
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
// The opener: View held for kOpenHoldMs on one pad opens that player's window (View with Menu raised Steam's on-screen
// keyboard over the game). View alone opens the map at its press, so a View press is held back from the game: let go
// before kOpenHoldMs, the game gets it then as a one-poll tap (the map opens at the let-go, a tap's length late); held
// kOpenHoldMs, the opener fires once and the game never sees that press.

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
// A button that is a tap or a long press, not held back from the game (a window's B: a tap closes it, a hold leaves
// the game): the hold fires once after `ms` down, again only after a let-go; a let-go before then is the tap, told at
// the let-go, so a press that grows into the hold never also taps.

struct Held
{
    bool         down  = false;
    bool         fired = false;
    std::int64_t since = 0;
};

enum class Long : std::uint8_t
{
    None,
    Tap,   // let go before `ms`
    Hold,  // down `ms`
};

constexpr Long Hold(Held& h, bool down, std::int64_t nowMs, std::int64_t ms) noexcept
{
    if (!down)
    {
        const bool tapped = h.down && !h.fired;
        h                 = Held{};
        return tapped ? Long::Tap : Long::None;
    }
    if (!h.down)
    {
        h.down  = true;
        h.since = nowMs;
    }
    if (!h.fired && nowMs - h.since >= ms)
    {
        h.fired = true;
        return Long::Hold;
    }
    return Long::None;
}

/// `slot` holds a player while `playing` (bit N: slot N holds a player; slot 0, player 1's, always).
constexpr bool PlayerSlot(int slot, std::uint32_t playing) noexcept
{
    return slot == 0 || (slot >= 1 && slot < 32 && (playing & (1u << slot)) != 0);
}

/// What the game sees of a pad on `slot` while `playing`: Start only on a player's slot, so a pad that plays no one
/// never pauses the game; its Start joins instead (players/player_window).
constexpr std::uint16_t StartGate(std::uint16_t buttons, int slot, std::uint32_t playing) noexcept
{
    return PlayerSlot(slot, playing) ? buttons : static_cast<std::uint16_t>(buttons & ~kStart);
}

// ---------------------------------------------------------------------------------------------------------------------
// The let-go: after a pad left a window or changed slots, a pad still holding what it held then stays idle to the game
// until it lets go of that, so the press that closed a window never reaches the game. Only what was held at the first
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

/// B's hold: down this long on a player's pad with its window open.
inline constexpr std::int64_t kLeaveHoldMs = 1000;

}  // namespace lss::players::pad_rules
