#pragma once

// The pure rules of each physical pad's input, as the pad table reads it (players/pad_table) and the player windows
// hear it (players/player_window). Host-tested (tests/pad_rules_test.cpp).
//   - the pad table: the game's pad slot N (its controller id N) is read by two readers, XInput (user N as the game
//     asks for it) and the PlayStation reader (its record N); each answers it with one of its own physical pads, a
//     permutation per reader; slot N is player N's (seat N), slot 3 the spare. Each slot's player plays on one of the
//     two pads there, the slot's live reader's; the other reader's pad there is muted while the live one is present;
//   - the window's opener, View (Create on a PlayStation pad) held a second: a shorter press still reaches the game,
//     as a tap at its let-go;
//   - B as a tap (let go within a second) or a hold (a second down), never both;
//   - Start only on a slot a player holds: a pad that plays no one never pauses the game (its Start joins instead);
//   - the let-go: a pad that changed slots or left a window stays idle to the game until it lets go of what it held.

#include <array>
#include <cstdint>
#include <string>

namespace lss::players::pad_rules {

inline constexpr int kSlots   = 4;  // the game's pad slots (controller ids 0..3), and each reader's pads
inline constexpr int kReaders = 2;  // XInput, then the PlayStation reader
inline constexpr int kPads    = kSlots * kReaders;  // pad p: reader p / kSlots, its number there p % kSlots

inline constexpr int kXInput      = 0;  // pads 0..3: XInput users 0..3
inline constexpr int kPlayStation = 1;  // pads 4..7: the PlayStation reader's pads by scePad user 1..4
inline constexpr int kKeyboard    = 2;  // as a player slot's live "reader": the keyboard plays that player, no pad there does
inline constexpr int kNoPad       = -1;  // PadOn of a slot the keyboard plays
inline constexpr int kSpareSlot   = kSlots - 1;  // the slot no player holds: the keyboard never plays it

constexpr int ReaderOf(int pad) noexcept
{
    return pad / kSlots;
}

/// The lane of `reader` on `slot`: the table's entry for which of that reader's pads answers the slot.
constexpr int Lane(int reader, int slot) noexcept
{
    return reader * kSlots + slot;
}

/// The keyboard (and mouse) as a device among the pads, in the player windows' input (players/keyboard_keys): its number
/// there, after the pads'.
inline constexpr int kKeyboardPad = kPads;
static_assert(kKeyboardPad / kSlots == kKeyboard, "the keyboard's number is its reader's");

/// "pad 2" for XInput user 1, "PlayStation pad 1" for the PlayStation reader's first pad, "the keyboard" for kNoPad
/// (a slot the keyboard plays) and kKeyboardPad: the log's and the console's names.
inline std::wstring PadName(int pad)
{
    if (pad < 0 || pad == kKeyboardPad)
    {
        return L"the keyboard";
    }
    return (ReaderOf(pad) == kPlayStation ? L"PlayStation pad " : L"pad ") + std::to_wstring(pad % kSlots + 1);
}

/// The name of the button that opens `pad`'s window, as its own device labels it: the one left of the centre on both
/// kinds of pad (View on an Xbox pad, Create on a PlayStation pad), Tab on the keyboard (the game's map key, as View is
/// the pad's).
constexpr const wchar_t* OpenerName(int pad) noexcept
{
    return ReaderOf(pad) == kKeyboard ? L"Tab" : ReaderOf(pad) == kPlayStation ? L"Create" : L"View";
}

/// The name of the button `pad` holds in its window to leave, for the log.
constexpr const wchar_t* LeaveName(int pad) noexcept
{
    return ReaderOf(pad) == kKeyboard ? L"Backspace" : ReaderOf(pad) == kPlayStation ? L"Circle" : L"B";
}

// XInput's button bits (XINPUT_GAMEPAD_*), kept here so the rules build without Windows headers.
inline constexpr std::uint16_t kDpadUp    = 0x0001;
inline constexpr std::uint16_t kDpadDown  = 0x0002;
inline constexpr std::uint16_t kDpadLeft  = 0x0004;
inline constexpr std::uint16_t kDpadRight = 0x0008;
inline constexpr std::uint16_t kStart     = 0x0010;  // START (Menu)
inline constexpr std::uint16_t kView      = 0x0020;  // BACK
inline constexpr std::uint16_t kA         = 0x1000;
inline constexpr std::uint16_t kB         = 0x2000;
inline constexpr std::uint16_t kX         = 0x4000;
inline constexpr std::uint16_t kY         = 0x8000;

// The PlayStation reader's button bits (ScePadData.buttons) and the XInput bit each one is to the rules: Options is
// Start, Cross A, Circle B (the keys the game itself sends for them), and Create is View, the window's opener, as the
// button in the same place on both kinds. The touchpad click, the game's map button on a PlayStation pad, has no XInput
// bit here, so it reaches the game as it is; so do L2 and R2 (their analog values are the triggers). Create, PS and Mute
// are the system buttons: the library hands them to the mod (players/pad_table) and never to the game.
inline constexpr std::uint32_t kCreate        = 0x000001;
inline constexpr std::uint32_t kSystemButtons = kCreate | 0x010000 | 0x200000;  // Create, PS, Mute

struct ButtonPair
{
    std::uint32_t playStation;
    std::uint16_t xinput;
};
inline constexpr std::array<ButtonPair, 14> kPlayStationButtons{{
    {kCreate, kView},
    {0x000002, 0x0040},  // L3: LEFT_THUMB
    {0x000004, 0x0080},  // R3: RIGHT_THUMB
    {0x000008, kStart},  // Options
    {0x000010, kDpadUp},
    {0x000020, kDpadRight},
    {0x000040, kDpadDown},
    {0x000080, kDpadLeft},
    {0x000400, 0x0100},  // L1: LEFT_SHOULDER
    {0x000800, 0x0200},  // R1: RIGHT_SHOULDER
    {0x001000, kY},      // triangle
    {0x002000, kB},      // circle
    {0x004000, kA},      // cross
    {0x008000, kX},      // square
}};

// The keyboard's keys in the rules, by Windows virtual-key code (FKeyEvent's key code): Tab, the opener, held a second
// (pad_rules::kOpenHoldMs), and while its player's window is open the keys that stand for the pad's: the arrows the
// d-pad (up and down the character, left and right the number), Q and E the shoulders, Enter A, Escape Start,
// Backspace B (its tap and its hold). The period key, pressed once while the keyboard plays player 2 or 3, brings that
// player's character back to player 1's (in a fight, into the fight), as a pad's double click of the left stick does.
inline constexpr std::uint32_t kOpenKey   = 0x09;  // VK_TAB
inline constexpr std::uint32_t kLeaveKey  = 0x08;  // VK_BACK
inline constexpr std::uint32_t kReturnKey = 0xBE;  // VK_OEM_PERIOD

/// A key event is the keyboard's: Slate gives a pad button's key event (which reaches the same handlers, with the pad's
/// user) no key code.
constexpr bool IsKeyboardKey(std::uint32_t keyCode) noexcept
{
    return keyCode != 0 && keyCode < 256;
}

/// A key as the rules' XInput bit while its player's window is open, or 0.
constexpr std::uint16_t FromKey(std::uint32_t virtualKey) noexcept
{
    switch (virtualKey)
    {
        case 0x25: return kDpadLeft;   // left
        case 0x26: return kDpadUp;     // up
        case 0x27: return kDpadRight;  // right
        case 0x28: return kDpadDown;   // down
        case 0x51: return 0x0100;  // Q: LEFT_SHOULDER
        case 0x45: return 0x0200;  // E: RIGHT_SHOULDER
        case 0x0D: return kA;      // Enter
        case 0x1B: return kStart;  // Escape
        case kLeaveKey: return kB;
        default: return 0;
    }
}

/// A PlayStation pad's buttons as XInput bits.
constexpr std::uint16_t FromPlayStation(std::uint32_t buttons) noexcept
{
    std::uint16_t out = 0;
    for (const ButtonPair& b : kPlayStationButtons)
    {
        out = static_cast<std::uint16_t>((buttons & b.playStation) != 0 ? out | b.xinput : out);
    }
    return out;
}

/// XInput bits as a PlayStation pad's buttons.
constexpr std::uint32_t ToPlayStation(std::uint16_t buttons) noexcept
{
    std::uint32_t out = 0;
    for (const ButtonPair& b : kPlayStationButtons)
    {
        out = (buttons & b.xinput) != 0 ? out | b.playStation : out;
    }
    return out;
}

/// The PlayStation buttons the game sees after the rules: `raw` less the XInput bits the rules held back, plus the ones
/// they added, less the system buttons (the opener's tap at its let-go is Create, which the game never sees); the
/// touchpad, L2, R2 and any other bit without an XInput twin pass as they are.
constexpr std::uint32_t PlayStationShown(std::uint32_t raw, std::uint16_t rawXInput, std::uint16_t shownXInput) noexcept
{
    const auto removed = static_cast<std::uint16_t>(rawXInput & ~shownXInput);
    const auto added   = static_cast<std::uint16_t>(shownXInput & ~rawXInput);
    return ((raw & ~ToPlayStation(removed)) | ToPlayStation(added)) & ~kSystemButtons;
}

/// The pad table: per lane (Lane(reader, slot)) the reader's physical pad answering the slot, and per slot its live
/// reader (whose pad there plays the slot's player). Identity, XInput live, until a join, the title screen or the
/// console changes it.
struct PadTable
{
    std::array<int, kPads>            lanes{0, 1, 2, 3, 4, 5, 6, 7};
    std::array<std::uint8_t, kSlots>  live{};

    constexpr bool operator==(const PadTable&) const noexcept = default;
};
inline constexpr PadTable kIdentity{};

/// A table whose XInput lanes are `xinput` (the PlayStation lanes identity, XInput live everywhere).
constexpr PadTable FromXInput(const std::array<int, kSlots>& xinput) noexcept
{
    PadTable table{};
    for (int slot = 0; slot < kSlots; ++slot)
    {
        table.lanes[static_cast<std::size_t>(slot)] = xinput[static_cast<std::size_t>(slot)];
    }
    return table;
}

/// Each lane holds one of its own reader's pads, each pad once; each slot's live reader is a reader, or the keyboard
/// on one player slot at most.
constexpr bool IsValid(const PadTable& table) noexcept
{
    std::array<bool, kPads> seen{};
    for (int lane = 0; lane < kPads; ++lane)
    {
        const int pad = table.lanes[static_cast<std::size_t>(lane)];
        if (pad < 0 || pad >= kPads || ReaderOf(pad) != ReaderOf(lane) || seen[static_cast<std::size_t>(pad)])
        {
            return false;
        }
        seen[static_cast<std::size_t>(pad)] = true;
    }
    int keyboards = 0;
    for (int slot = 0; slot < kSlots; ++slot)
    {
        const int reader = table.live[static_cast<std::size_t>(slot)];
        if (reader == kKeyboard && slot != kSpareSlot)
        {
            ++keyboards;
        }
        else if (reader >= kReaders)
        {
            return false;
        }
    }
    return keyboards <= 1;
}

/// The slot `pad` answers, or -1 (never for a valid table).
constexpr int SlotOf(const PadTable& table, int pad) noexcept
{
    for (int lane = 0; lane < kPads; ++lane)
    {
        if (table.lanes[static_cast<std::size_t>(lane)] == pad)
        {
            return lane % kSlots;
        }
    }
    return -1;
}

/// The pad on `slot` that plays its player: the slot's live reader's; kNoPad when the keyboard plays it.
constexpr int PadOn(const PadTable& table, int slot) noexcept
{
    const int reader = table.live[static_cast<std::size_t>(slot)];
    return reader >= kReaders ? kNoPad : table.lanes[static_cast<std::size_t>(Lane(reader, slot))];
}

/// The slot the keyboard plays (its player's alone), or -1: then it plays beside player 1's pad, as in single play.
constexpr int KeyboardSlot(const PadTable& table) noexcept
{
    for (int slot = 0; slot < kSlots; ++slot)
    {
        if (table.live[static_cast<std::size_t>(slot)] == kKeyboard)
        {
            return slot;
        }
    }
    return -1;
}

/// The table with the keyboard back beside player 1 (the slot it played back to its XInput pad).
constexpr PadTable Unkeyboarded(PadTable table) noexcept
{
    if (const int before = KeyboardSlot(table); before >= 0)
    {
        table.live[static_cast<std::size_t>(before)] = static_cast<std::uint8_t>(kXInput);
    }
    return table;
}

/// The table with the keyboard playing `slot`'s player alone (the slot it played before goes back to its XInput pad):
/// every pad on that slot is muted while it does, so their Start adds a player. A pad taking the slot (WithPadOn) ends
/// it; so does Unkeyboarded.
constexpr PadTable WithKeyboardOn(PadTable table, int slot) noexcept
{
    if (slot < 0 || slot >= kSpareSlot)
    {
        return table;
    }
    table                                      = Unkeyboarded(table);
    table.live[static_cast<std::size_t>(slot)] = static_cast<std::uint8_t>(kKeyboard);
    return table;
}

/// The table with pad `which` on `slot` (swapped, in its own reader, with the pad there) and playing its player: the
/// slot's live reader becomes the pad's.
constexpr PadTable WithPadOn(PadTable table, int which, int slot) noexcept
{
    const int from = SlotOf(table, which);
    if (from < 0 || slot < 0 || slot >= kSlots)
    {
        return table;
    }
    const int  reader = ReaderOf(which);
    const auto at     = static_cast<std::size_t>(Lane(reader, slot));
    table.lanes[static_cast<std::size_t>(Lane(reader, from))] = table.lanes[at];
    table.lanes[at]                                            = which;
    table.live[static_cast<std::size_t>(slot)]                 = static_cast<std::uint8_t>(reader);
    return table;
}

// ---------------------------------------------------------------------------------------------------------------------
// The opener: View held for kOpenHoldMs on one pad opens that player's window (View with Menu raised Steam's on-screen
// keyboard over the game). View alone opens the map at its press, so a View press is held back from the game: let go
// before kOpenHoldMs, the game gets it then as a one-poll tap (the map opens at the let-go, a tap's length late); held
// kOpenHoldMs, the opener fires once and the game never sees that press. A PlayStation pad's opener is Create, which
// the game never sees either way; its map button, the touchpad, is not held back.

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

/// `pad` is muted: it answers (`present`, bit p: pad p answers) on a player's slot (`playing`) whose live reader is
/// another one, while that reader's pad there answers too, or the keyboard plays that slot. It reads idle to the game and plays no one, so its Start
/// joins. A slot's other pad while the live one is absent plays on (one person, one pad, whichever kind it is).
constexpr bool Muted(const PadTable& table, std::uint32_t playing, std::uint32_t present, int pad) noexcept
{
    const int slot = SlotOf(table, pad);
    if ((present >> pad & 1u) == 0 || slot < 0 || !PlayerSlot(slot, playing) ||
        table.live[static_cast<std::size_t>(slot)] == ReaderOf(pad))
    {
        return false;
    }
    const int live = PadOn(table, slot);
    return live == kNoPad || (present >> live & 1u) != 0;  // the keyboard is always there
}

// One pad read by both readers: Steam Input shows a PlayStation pad as an XInput pad while the game also reads it
// itself, so each press comes from two pads, of different readers, in the same poll or the next.
inline constexpr std::int64_t kTwinMs = 100;

/// Two presses of one button (their polls' clock, 0 for none) are one press read twice.
constexpr bool Twins(std::int64_t at, std::int64_t otherAt) noexcept
{
    return at != 0 && otherAt != 0 && (at > otherAt ? at - otherAt : otherAt - at) <= kTwinMs;
}

/// A Start read twice: `pad`'s copy joins no one when its twin plays a player (the Start is that player's), or when it
/// is the XInput copy (one join, the PlayStation copy's: the game's own read of the pad, with its button art).
constexpr bool TwinStartDropped(int pad, bool twinPlays) noexcept
{
    return twinPlays || ReaderOf(pad) == kXInput;
}

/// The pad that plays `slot`'s player now: the live reader's while it answers (`present`, bit p: pad p answers), else
/// the other reader's pad there when that one answers (Muted lets it play on), else the live reader's; kNoPad while
/// the keyboard plays the slot.
constexpr int PlayingPad(const PadTable& table, int slot, std::uint32_t present) noexcept
{
    const int live = PadOn(table, slot);
    if (live == kNoPad || (present >> live & 1u) != 0)
    {
        return live;
    }
    for (int reader = 0; reader < kReaders; ++reader)
    {
        const int other = table.lanes[static_cast<std::size_t>(Lane(reader, slot))];
        if (other != live && (present >> other & 1u) != 0)
        {
            return other;
        }
    }
    return live;
}

/// The muted pads (bit p: pad p).
constexpr std::uint32_t MutedPads(const PadTable& table, std::uint32_t playing, std::uint32_t present) noexcept
{
    std::uint32_t pads = 0;
    for (int pad = 0; pad < kPads; ++pad)
    {
        pads |= Muted(table, playing, present, pad) ? 1u << pad : 0u;
    }
    return pads;
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
