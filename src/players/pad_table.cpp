#include "players/pad_table.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/vtable_entry.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Xinput.h>

namespace lss::players::pad_table {

namespace {

namespace rules = pad_rules;

using GetStateFn     = DWORD(WINAPI*)(DWORD user, XINPUT_STATE* state);
using SetStateFn     = DWORD(WINAPI*)(DWORD user, XINPUT_VIBRATION* vibration);
using ReadStateFn    = int (*)(std::int32_t handle, std::uint8_t* data);
using SetVibrationFn = int (*)(std::int32_t handle, const std::uint8_t* motors);

constexpr int kSlots = rules::kSlots;
constexpr int kPads  = rules::kPads;

constexpr std::uint32_t kAllPads      = (1u << kPads) - 1u;
constexpr std::uint32_t kSkipFrameBit = 0x80000000u;  // set in a read's buttons, the record skips the whole frame
constexpr std::uint8_t  kStickCentre  = 0x80;

/// One physical pad, as the game's poll last read it. Game thread (the polls and the console all run there).
struct Pad
{
    rules::OpenHold hold;      // View: held back from the game; a second down opens the player's window
    rules::Held     leave;     // B: a window's close (a tap) or its leave (a hold)
    rules::LetGo    letGo;     // idle to the game until it lets go of what it held at a change or its idle's end
    std::uint16_t   last = 0;  // the buttons (XInput bits) on its last poll (the listener's presses)
    std::array<std::int64_t, 16> downAt{};  // per XInput bit: the poll that last saw it go down (TwinOf)
};

/// What the game sees of one read: idle, or these buttons (XInput bits).
struct Shown
{
    bool          idle    = false;
    std::uint16_t buttons = 0;
};

platform::VTableEntry   g_getEntry;
platform::VTableEntry   g_setEntry;
platform::EntryRedirect g_read;       // the PlayStation reader's read
platform::EntryRedirect g_vibration;  // its rumble send
GetStateFn              g_get          = nullptr;  // what the import slots held: XInput's own, or whatever was there first
SetStateFn              g_set          = nullptr;
std::uintptr_t          g_appGlobal    = 0;
std::uintptr_t          g_sonyVTable   = 0;
bool                    g_installed    = false;
bool                    g_playStation  = false;    // the PlayStation reader's read and rumble go through the table
std::uint8_t*           g_systemButtons = nullptr;  // the library's system-button switch, while the mod holds it on

std::array<std::atomic<int>, kPads>           g_lanes{0, 1, 2, 3, 4, 5, 6, 7};
std::array<std::atomic<std::uint8_t>, kSlots> g_live{};
std::atomic<Listener>                         g_listener{nullptr};
std::atomic<std::uint32_t>                    g_windowIdle{0};  // bit p: pad p is held idle by its open window
std::atomic<std::uint32_t>                    g_muted{0};       // bit p: pad p is muted (pad_rules::Muted)
std::atomic<std::uint32_t>                    g_present{0};     // bit p: pad p answered its last read
std::atomic<std::uint32_t>                    g_playing{1};     // the slots a player holds (SetPlaying)

// Game thread.
std::array<Pad, kPads>            g_pads{};
std::array<std::int32_t, kSlots>  g_handles{};       // by scePad user 1..4: its handle, as the records showed it (0: not yet)
std::array<std::uint8_t*, kSlots> g_records{};       // by slot: the PlayStation reader's record, as its reads showed it
std::uint32_t                     g_forgetSony   = 0;  // bit N: record N's last rumble to forget at its next read
std::uint32_t                     g_unplacedSaid = 0;  // bit N: record N's lane waits for an unseen handle (said)
std::uint32_t g_opens = 0, g_waited = 0, g_rechecks = 0, g_changes = 0, g_startsHeld = 0, g_sonyMoves = 0;
std::uint32_t g_unseenSlots    = 0;      // the slots whose pad answered while the engine counted them empty (bit N)
std::uint32_t g_unseenRechecks = 0;      // the re-checks set for them
bool          g_recheckFailing = false;  // the last re-check write failed (its warning said)
bool          g_sonyLive       = false;  // the PlayStation reader's first read was seen (said once)
bool          g_badRecordSaid  = false;  // a PlayStation read with an unreadable or out-of-range record (said once)
std::wstring  g_last           = L"none yet";

std::int64_t NowMs() noexcept
{
    return static_cast<std::int64_t>(GetTickCount64());
}

bool IsIdle(int pad) noexcept
{
    return ((g_windowIdle.load(std::memory_order_relaxed) | g_muted.load(std::memory_order_relaxed)) >> pad & 1u) != 0;
}

std::wstring PadsText(std::uint32_t pads)
{
    std::wstring text;
    for (int pad = 0; pad < kPads; ++pad)
    {
        if ((pads >> pad & 1u) != 0)
        {
            text += (text.empty() ? L"" : L", ") + rules::PadName(pad);
        }
    }
    return text.empty() ? std::wstring(L"none") : text;
}

/// "player 1 pad 1, player 2 PlayStation pad 1 (beside it pad 2), ...": each slot's player pad, and the other reader's
/// pad there when it answers.
std::wstring TableText(const rules::PadTable& table)
{
    static constexpr const wchar_t* kNames[kSlots] = {L"player 1", L"player 2", L"player 3", L"spare"};
    const std::uint32_t             present        = g_present.load(std::memory_order_relaxed);
    std::wstring                    text;
    for (int slot = 0; slot < kSlots; ++slot)
    {
        const int live = table.live[static_cast<std::size_t>(slot)];
        text += std::format(L"{}{} {}", slot == 0 ? L"" : L", ", kNames[slot], rules::PadName(rules::PadOn(table, slot)));
        for (int reader = 0; reader < rules::kReaders; ++reader)
        {
            const int other = table.lanes[static_cast<std::size_t>(rules::Lane(reader, slot))];
            if (reader != live && (present >> other & 1u) != 0)
            {
                text += std::format(L" (beside it {})", rules::PadName(other));
            }
        }
    }
    return text;
}

/// The engine's XInput interface, or null.
std::uint8_t* XInputDevice() noexcept
{
    void* const app = g_appGlobal != 0 ? platform::ReadPointer(reinterpret_cast<const void*>(g_appGlobal), 0) : nullptr;
    return static_cast<std::uint8_t*>(app != nullptr ? platform::ReadPointer(app, facts::kAppXInputInterface) : nullptr);
}

/// The engine's own "poll every slot again" byte, as its WM_DEVICECHANGE handling sets it.
bool FlagRecheck() noexcept
{
    std::uint8_t* const xi = XInputDevice();
    const std::uint8_t  on = 1;
    if (xi == nullptr || !platform::WriteGuarded(xi + facts::kXInputNeedsStateUpdate, &on, 1))
    {
        if (!g_recheckFailing)  // said once per run of failures (RecheckAnswering asks every second)
        {
            platform::LogWarning(L"pads: the engine's pad re-check could not be set; a pad moved onto a slot it counted "
                                 L"empty is read after the next device change");
        }
        g_recheckFailing = true;
        return false;
    }
    g_recheckFailing = false;
    ++g_rechecks;
    return true;
}

/// A pad that answers, or no longer does. The PlayStation pads' changes are said here (XInput's: players/pad_presence).
void NotePresent(int pad, bool present) noexcept
{
    const std::uint32_t bit    = 1u << pad;
    const std::uint32_t before = present ? g_present.fetch_or(bit) : g_present.fetch_and(~bit);
    if (((before & bit) != 0) == present || rules::ReaderOf(pad) != rules::kPlayStation)
    {
        return;
    }
    try
    {
        const int slot = rules::SlotOf(Table(), pad);
        platform::Log(L"pads: {} {} (on player {}'s slot)", rules::PadName(pad), present ? L"connected" : L"is gone", slot + 1);
    }
    catch (...)
    {
    }
}

/// One read of `pad` answering `slot`, on the game thread: its new presses, the opener and B's tap or hold to the
/// listener (an idle pad's too: its window's keys); then the idle, the let-go and the Start gate. `raw` is its buttons
/// as XInput bits, `left` and `right` its triggers.
Shown Rule(int pad, int slot, std::uint16_t raw, std::uint8_t left, std::uint8_t right) noexcept
{
    Pad&               p   = g_pads[static_cast<std::size_t>(pad)];
    const std::int64_t now = NowMs();
    // The listener only notes them (a table change inside the poll would lose its re-check).
    const rules::Gated gated   = rules::Gate(p.hold, raw, now);
    const auto         pressed = static_cast<std::uint16_t>(raw & ~p.last);
    const rules::Long  b       = rules::Hold(p.leave, (raw & rules::kB) != 0, now, rules::kLeaveHoldMs);
    p.last                     = raw;
    for (int bit = 0; bit < 16; ++bit)
    {
        p.downAt[static_cast<std::size_t>(bit)] = (pressed >> bit & 1u) != 0 ? now : p.downAt[static_cast<std::size_t>(bit)];
    }
    if (const Listener listener = g_listener.load(std::memory_order_relaxed);
        listener != nullptr && (pressed != 0 || gated.open || b != rules::Long::None))
    {
        listener(pad, pressed, gated.open, b == rules::Long::Tap, b == rules::Long::Hold);
    }
    if (gated.open)
    {
        ++g_opens;
        try
        {
            g_last = std::format(L"{} held 1 s on {} (player {}'s slot)", rules::OpenerName(pad), rules::PadName(pad), slot + 1);
            platform::Log(L"pads: {}", g_last);
        }
        catch (...)
        {
        }
    }
    if (IsIdle(pad))
    {
        return {true, 0};
    }
    const rules::Wait wait = rules::Poll(p.letGo, raw, left, right, now);
    if (wait != rules::Wait::None)
    {
        // Idle on the poll that ends the wait too: a held-back View released on it would otherwise reach the new slot
        // as a tap.
        try
        {
            if (wait == rules::Wait::LetGoNow)
            {
                platform::LogDebug(L"pads: {} let go: it reaches the game again (player {}'s slot)", rules::PadName(pad), slot + 1);
            }
            else if (wait == rules::Wait::TriggerRests)
            {
                platform::Log(L"pads: {}'s triggers rest at {} and {} (of 255) 2 s after its buttons let go: it reaches the "
                              L"game again (player {}'s slot)",
                              rules::PadName(pad), left, right, slot + 1);
            }
            else if (wait == rules::Wait::ButtonStuck)
            {
                platform::Log(L"pads: {} still holds buttons {:#06x} 10 s after the change: it reaches the game again (player "
                              L"{}'s slot)",
                              rules::PadName(pad), raw, slot + 1);
            }
        }
        catch (...)
        {
        }
        return {true, 0};
    }
    // A pad that plays no one never pauses the game: its Start, heard by the listener, joins (players/player_window).
    // View reaches the game only as a tap at its let-go (the opener holds it back).
    const std::uint16_t shown = rules::StartGate(gated.buttons, slot, g_playing.load(std::memory_order_relaxed));
    if ((pressed & rules::kStart) != 0 && (shown & rules::kStart) == 0)
    {
        ++g_startsHeld;
        platform::LogDebug(L"pads: {}'s Start held back from the game (player {}'s slot, no player there): a join",
                           rules::PadName(pad), slot + 1);
    }
    return {false, shown};
}

/// A pad that did not answer: a View or B it held starts over at its return.
void Dropped(int pad) noexcept
{
    Pad& p  = g_pads[static_cast<std::size_t>(pad)];
    p.hold  = {};
    p.leave = {};
    p.last  = 0;
}

/// The game's poll of `slot`: the assigned pad's state, gated (see the header).
DWORD WINAPI GetStateFor(DWORD slot, XINPUT_STATE* state) noexcept
{
    if (slot >= static_cast<DWORD>(kSlots) || state == nullptr)
    {
        return g_get(slot, state);
    }
    const int   user   = g_lanes[slot].load(std::memory_order_relaxed);
    const DWORD result = g_get(static_cast<DWORD>(user), state);
    if (result != ERROR_SUCCESS)
    {
        *state = XINPUT_STATE{};  // the engine reads the buffer as left after a failure (RE-pad-slots.md)
        NotePresent(user, false);
        Dropped(user);
        return result;
    }
    NotePresent(user, true);
    const Shown shown = Rule(user, static_cast<int>(slot), state->Gamepad.wButtons, state->Gamepad.bLeftTrigger,
                             state->Gamepad.bRightTrigger);
    if (shown.idle)
    {
        state->Gamepad = XINPUT_GAMEPAD{};
    }
    else
    {
        state->Gamepad.wButtons = shown.buttons;
    }
    return result;
}

/// The game's rumble for `slot`: to the assigned pad; off while that pad is idle.
DWORD WINAPI SetStateFor(DWORD slot, XINPUT_VIBRATION* vibration) noexcept
{
    if (slot >= static_cast<DWORD>(kSlots))
    {
        return g_set(slot, vibration);
    }
    const auto user = static_cast<DWORD>(g_lanes[slot].load(std::memory_order_relaxed));
    if (IsIdle(static_cast<int>(user)))
    {
        XINPUT_VIBRATION off{};
        return g_set(user, &off);
    }
    return g_set(user, vibration);
}

/// The pad of scePad handle `handle` (pads 4..7 for users 1..4), or -1.
int PlayStationPad(std::int32_t handle) noexcept
{
    const int user = handle & 0xFF;
    return user >= 1 && user <= kSlots ? rules::Lane(rules::kPlayStation, user - 1) : -1;
}

/// Before a PlayStation record's read: the handle its lane asks for, written into the record when it differs. The
/// cleared connected byte has the game run its own connect path for the pad on this read (controller information, the
/// connection broadcast), the forgotten rumble has it send the slot's rumble to that pad, and the pad that left is
/// stopped. Returns the handle the record holds now.
std::int32_t Place(std::uint8_t* record, int slot, std::int32_t handle) noexcept
{
    g_records[static_cast<std::size_t>(slot)] = record;
    if (const int seen = PlayStationPad(handle); seen >= 0)
    {
        g_handles[static_cast<std::size_t>(seen - kSlots)] = handle;
    }
    const float never = -1.0f;  // no motor value the game computes: its next compare sees a change
    if ((g_forgetSony >> slot & 1u) != 0)
    {
        g_forgetSony &= ~(1u << slot);
        platform::WriteGuarded(record + facts::kSonyRecordLastLarge, &never, sizeof(never));
        platform::WriteGuarded(record + facts::kSonyRecordLastSmall, &never, sizeof(never));
    }
    const int          pad  = g_lanes[static_cast<std::size_t>(rules::Lane(rules::kPlayStation, slot))].load(std::memory_order_relaxed);
    const std::int32_t want = g_handles[static_cast<std::size_t>(pad - kSlots)];
    if (want == handle)
    {
        return handle;
    }
    if (want == 0)
    {
        if ((g_unplacedSaid >> slot & 1u) == 0)
        {
            g_unplacedSaid |= 1u << slot;
            try
            {
                platform::Log(L"pads: {} is to answer player {}'s slot on the PlayStation reader, but its handle is not known "
                              L"yet (no record has read it); the record keeps its own pad until it is",
                              rules::PadName(pad), slot + 1);
            }
            catch (...)
            {
            }
        }
        return handle;
    }
    if (!platform::WriteGuarded(record + facts::kSonyRecordHandle, &want, sizeof(want)))
    {
        return handle;
    }
    // A pad that answers: the connected byte cleared, so this read runs the game's connect path for it (its keys then
    // compare against what the record last sent, releasing what the old pad held). A pad that does not: the byte kept,
    // so this read is the game's own disconnect fall, whose release pass lets go of everything the old pad held.
    if ((g_present.load(std::memory_order_relaxed) >> pad & 1u) != 0)
    {
        const std::uint8_t off = 0;
        platform::WriteGuarded(record + facts::kSonyRecordConnected, &off, sizeof(off));
    }
    platform::WriteGuarded(record + facts::kSonyRecordLastLarge, &never, sizeof(never));
    platform::WriteGuarded(record + facts::kSonyRecordLastSmall, &never, sizeof(never));
    const std::uint8_t stop[2] = {0, 0};
    reinterpret_cast<SetVibrationFn>(g_vibration.Original())(handle, stop);
    ++g_sonyMoves;
    g_unplacedSaid &= ~(1u << slot);
    try
    {
        platform::LogDebug(L"pads: {} now answers player {}'s slot on the PlayStation reader (was {})", rules::PadName(pad),
                           slot + 1, rules::PadName(PlayStationPad(handle)));
    }
    catch (...)
    {
    }
    return want;
}

/// A PlayStation pad's read made idle: nothing pressed, the sticks centred, the triggers and touch at rest; still
/// connected (a disconnect would run the game's disconnect path and could leave it with no pad at all).
void IdlePlayStation(std::uint8_t* data) noexcept
{
    const std::uint32_t none      = 0;
    const std::uint8_t  sticks[4] = {kStickCentre, kStickCentre, kStickCentre, kStickCentre};
    const std::uint8_t  rest[2]   = {0, 0};
    const std::uint8_t  touches   = 0;
    platform::WriteGuarded(data + facts::kPadDataButtons, &none, sizeof(none));
    platform::WriteGuarded(data + facts::kPadDataSticks, sticks, sizeof(sticks));
    platform::WriteGuarded(data + facts::kPadDataTriggers, rest, sizeof(rest));
    platform::WriteGuarded(data + facts::kPadDataTouchCount, &touches, sizeof(touches));
}

/// After a PlayStation read: its buttons as read, system buttons included (empty if unreadable), and the game's data
/// without them, as the library hands it to the game with its switch off.
std::optional<std::uint32_t> TakeSystemButtons(std::uint8_t* data) noexcept
{
    std::uint32_t buttons = 0;
    if (!platform::Read(data + facts::kPadDataButtons, buttons))
    {
        return std::nullopt;
    }
    if ((buttons & rules::kSystemButtons) != 0)
    {
        const std::uint32_t game = buttons & ~rules::kSystemButtons;
        platform::WriteGuarded(data + facts::kPadDataButtons, &game, sizeof(game));
    }
    return buttons;
}

/// The PlayStation reader's read of one record (its one caller, the game thread's device poll): the record's lane's
/// pad, read, then the same rules as XInput's.
int ReadStateFor(std::int32_t handle, std::uint8_t* data) noexcept
{
    const auto          original = reinterpret_cast<ReadStateFn>(g_read.Original());
    std::uint8_t* const record   = data != nullptr ? data - facts::kSonyRecordPadData : nullptr;
    std::int32_t        slot     = -1;
    if (record == nullptr || !platform::Read(record + facts::kSonyRecordId, slot) || slot < 0 || slot >= kSlots)
    {
        if (!g_badRecordSaid)
        {
            g_badRecordSaid = true;
            platform::LogWarning(L"pads: a PlayStation read went by without the pad table's rules: its record's controller id "
                                 L"was unreadable or no slot");
        }
        const int result = original(handle, data);
        if (data != nullptr)
        {
            TakeSystemButtons(data);
        }
        return result;
    }
    handle            = Place(record, slot, handle);
    const int  result = original(handle, data);
    const auto taken  = TakeSystemButtons(data);
    const int  pad    = PlayStationPad(handle);
    if (!g_sonyLive)
    {
        g_sonyLive = true;
        platform::Log(L"pads: the PlayStation reader's first read (player {}'s slot, scePad user {}): its pads go through the "
                      L"pad table",
                      slot + 1, handle & 0xFF);
    }
    std::uint8_t        connected = 0;
    const std::uint32_t buttons   = taken.value_or(0);
    if (pad < 0 || result != 0 || !platform::Read(data + facts::kPadDataConnected, connected) || connected == 0 || !taken)
    {
        if (pad >= 0)
        {
            NotePresent(pad, false);
            Dropped(pad);
        }
        return result;
    }
    NotePresent(pad, true);
    if ((buttons & kSkipFrameBit) != 0)
    {
        return result;  // the game skips this frame
    }
    std::uint8_t triggers[2] = {0, 0};
    platform::Read(data + facts::kPadDataTriggers, triggers);
    // Ruled on the record's slot: the controller id its presses reach the game with, also while a move waits for a
    // handle not yet seen (the record then keeps its own pad).
    const std::uint16_t raw   = rules::FromPlayStation(buttons);
    const Shown         shown = Rule(pad, slot, raw, triggers[0], triggers[1]);
    if (shown.idle)
    {
        IdlePlayStation(data);
    }
    else if (shown.buttons != raw)
    {
        const std::uint32_t out = rules::PlayStationShown(buttons, raw, shown.buttons);
        platform::WriteGuarded(data + facts::kPadDataButtons, &out, sizeof(out));
    }
    return result;
}

/// The PlayStation reader's rumble send (both of its callers): off to an idle pad.
int SetVibrationFor(std::int32_t handle, const std::uint8_t* motors) noexcept
{
    const auto original = reinterpret_cast<SetVibrationFn>(g_vibration.Original());
    if (const int pad = PlayStationPad(handle); pad >= 0 && IsIdle(pad))
    {
        const std::uint8_t off[2] = {0, 0};
        return original(handle, off);
    }
    return original(handle, motors);
}

/// Every pad still holding what it holds now stays idle to the game until it lets go of that.
void AwaitReleases() noexcept
{
    for (Pad& pad : g_pads)
    {
        rules::AwaitLetGo(pad.letGo);
    }
}

/// The held-idle and the muted pads set; a pad leaving both waits for its let-go, and the PlayStation records' rumble
/// is sent again (an idle pad's was sent as off; XInput's is sent every frame).
void ApplyIdle(std::uint32_t windows, std::uint32_t muted, const wchar_t* why)
{
    const std::uint32_t windowsBefore = g_windowIdle.exchange(windows);
    const std::uint32_t mutedBefore   = g_muted.exchange(muted);
    const std::uint32_t before        = windowsBefore | mutedBefore;
    const std::uint32_t after         = windows | muted;
    if (!g_installed || after == before)
    {
        return;
    }
    const std::uint32_t back = before & ~after;
    for (int pad = 0; pad < kPads; ++pad)
    {
        if ((back >> pad & 1u) != 0)
        {
            rules::AwaitLetGo(g_pads[static_cast<std::size_t>(pad)].letGo);  // the press that ended it stays out
            ++g_waited;
        }
    }
    g_forgetSony = (1u << kSlots) - 1u;
    if (muted != mutedBefore)
    {
        g_last = std::format(L"muted: {} (a pad beside the one that plays its slot's player: it reads idle, and its Start "
                             L"joins)",
                             PadsText(muted));
        platform::Log(L"pads: {}", g_last);
    }
    if (windows != windowsBefore)
    {
        g_last = std::format(L"idle to the game: {}: {}", PadsText(windows), why);
        platform::Log(L"pads: {}", g_last);
    }
}

/// The module an address lies in, for the install line.
std::wstring ModuleOf(std::uintptr_t address)
{
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) ||
        GetModuleFileNameW(module, path, MAX_PATH) == 0)
    {
        return L"no module";
    }
    const std::wstring full(path);
    const auto         slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? full : full.substr(slash + 1);
}

/// Logs the engine's other input devices. At install.
void LogDevices() noexcept
{
    void* const  app   = platform::ReadPointer(reinterpret_cast<const void*>(g_appGlobal), 0);
    void* const  data  = app != nullptr ? platform::ReadPointer(app, facts::kAppInputDevices) : nullptr;
    std::int32_t count = 0;
    if (app == nullptr || !platform::Read(static_cast<const std::uint8_t*>(app) + facts::kAppInputDevices + 8, count) ||
        count < 0 || count > 64)
    {
        platform::LogDebug(L"pads: the engine's other input devices are not readable");
        return;
    }
    if (count == 0)
    {
        platform::Log(L"pads: the engine's other input devices are not made yet (its first pad poll makes them)");
        return;
    }
    int sony = 0;
    for (std::int32_t i = 0; i < count && data != nullptr; ++i)
    {
        void* const device = platform::ReadPointer(data, static_cast<std::uint32_t>(i) * facts::kAppInputDeviceBytes);
        void* const vtable = device != nullptr ? platform::ReadPointer(device, 0) : nullptr;
        sony += reinterpret_cast<std::uintptr_t>(vtable) == g_sonyVTable ? 1 : 0;
    }
    platform::Log(L"pads: the engine lists {} other input device(s), {} the PlayStation pad reader", count,
                  sony > 0 ? L"one" : L"none");
}

std::wstring Reading()
{
    const std::uint32_t idle = g_windowIdle.load();
    return std::format(L"{}{}; {}; muted: {}; answering: {}; slots with a player {:#x}; opener held {}, Starts held back {}, "
                       L"waits for a let-go {}, re-checks {} ({} for a pad the engine had not seen; such slots now {:#x}), "
                       L"changes {}, PlayStation moves {}; last: {}",
                       g_installed ? (g_playStation ? L"on (XInput and PlayStation)" : L"on (XInput only)") : L"not installed",
                       idle != 0 ? std::format(L" (idle to the game: {})", PadsText(idle)) : std::wstring(), TableText(Table()),
                       PadsText(g_muted.load()), PadsText(g_present.load()), g_playing.load(), g_opens, g_startsHeld, g_waited,
                       g_rechecks, g_unseenRechecks, g_unseenSlots, g_changes, g_sonyMoves, g_last);
}

/// `pad-table`'s words: four pads for players 1, 2, 3 and the spare slot, each once; 1-4 an XInput pad, p1-p4 a
/// PlayStation pad. Empty if the words are not that.
std::optional<rules::PadTable> ParseTable(const std::wstring& text)
{
    rules::PadTable table{};
    std::uint32_t   seen  = 0;
    int             count = 0;
    bool            ps    = false;
    bool            bad   = false;
    for (const wchar_t c : text)
    {
        if (c == L'p' || c == L'P')
        {
            bad = bad || ps;
            ps  = true;
        }
        else if (c >= L'1' && c <= L'4' && count < kSlots)
        {
            const int pad = rules::Lane(ps ? rules::kPlayStation : rules::kXInput, c - L'1');
            bad           = bad || (seen >> pad & 1u) != 0;
            seen |= 1u << pad;
            table = rules::WithPadOn(table, pad, count++);
            ps    = false;
        }
        else if (c != L' ' && c != L',')
        {
            bad = true;
        }
    }
    if (bad || ps || count != kSlots || !rules::IsValid(table))
    {
        return std::nullopt;
    }
    return table;
}

void RegisterCommands()
{
    control::Register({.name  = L"pad-table",
                       .shape = control::Shape::Text,
                       .help  = L"which pad each player has: four pads for players 1, 2, 3 and the spare slot, each once; 1-4 "
                                L"an XInput pad, p1-p4 a PlayStation pad (`pad-table 2 1 3 4`: pad 2 plays player 1, pad 1 "
                                L"player 2; `pad-table p1 1 3 4`: the first PlayStation pad plays player 1)",
                       .set   = [](const control::Value& value) {
                           const auto table = ParseTable(std::get<std::wstring>(value));
                           if (!table)
                           {
                               return control::Outcome{false, L"four pads, each once: 1-4 an XInput pad, p1-p4 a PlayStation pad"};
                           }
                           Set(*table, L"the console");
                           return control::Outcome{true, TableText(Table())};
                       },
                       .get = [] { return control::Value{TableText(Table())}; }});
    control::Register({.name  = L"pads-idle",
                       .shape = control::Shape::OnOff,
                       .help  = L"every connected pad reads idle to the game, rumble off (an open window's state, for every "
                                L"pad; off: each pad holding a button stays idle until it lets go)",
                       .set   = [](const control::Value& value) {
                           SetIdlePads(std::get<bool>(value) ? kAllPads : 0u, L"the console");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_windowIdle.load() != 0}; }});
    control::Register({.name  = L"players.pad-table",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the pad table, the muted and answering pads, the slots with a player, the "
                                L"Starts held back, the waits for a let-go and the re-checks",
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
}

}  // namespace

rules::PadTable Table() noexcept
{
    rules::PadTable table{};
    for (int lane = 0; lane < kPads; ++lane)
    {
        table.lanes[static_cast<std::size_t>(lane)] = g_lanes[static_cast<std::size_t>(lane)].load(std::memory_order_relaxed);
    }
    for (int slot = 0; slot < kSlots; ++slot)
    {
        table.live[static_cast<std::size_t>(slot)] = g_live[static_cast<std::size_t>(slot)].load(std::memory_order_relaxed);
    }
    return table;
}

bool Set(const rules::PadTable& table, const wchar_t* why) noexcept
try
{
    if (!g_installed || !rules::IsValid(table))
    {
        return false;
    }
    const rules::PadTable before = Table();
    if (before == table)
    {
        return true;
    }
    for (int lane = 0; lane < kPads; ++lane)
    {
        g_lanes[static_cast<std::size_t>(lane)].store(table.lanes[static_cast<std::size_t>(lane)], std::memory_order_relaxed);
    }
    for (int slot = 0; slot < kSlots; ++slot)
    {
        g_live[static_cast<std::size_t>(slot)].store(table.live[static_cast<std::size_t>(slot)], std::memory_order_relaxed);
    }
    AwaitReleases();
    ++g_waited;
    ++g_changes;
    FlagRecheck();
    // Every XInput pad's motors off: a pad keeps the last rumble it was sent, and its slot may no longer be the one the
    // game sends it for; the game's next send per slot sets the new owner's. A PlayStation pad that leaves a slot is
    // stopped as its record takes the new one.
    for (int user = 0; user < kSlots; ++user)
    {
        XINPUT_VIBRATION off{};
        g_set(static_cast<DWORD>(user), &off);
    }
    g_forgetSony = (1u << kSlots) - 1u;
    g_last       = std::format(L"{} (was {}): {}", TableText(table), TableText(before), why);
    platform::Log(L"pads: {}", g_last);
    // The muted pads follow the new table at once (a moved pad's next read must not see the old ones).
    ApplyIdle(g_windowIdle.load(), rules::MutedPads(table, g_playing.load(), g_present.load()), why);
    return true;
}
catch (...)
{
    return false;
}

void SetIdlePads(std::uint32_t pads, const wchar_t* why) noexcept
try
{
    ApplyIdle(pads & kAllPads, g_muted.load(), why);
}
catch (...)
{
}

std::uint32_t PresentPads() noexcept
{
    return g_present.load(std::memory_order_relaxed);
}

int TwinOf(int pad, std::uint16_t button) noexcept
{
    const int bit = std::countr_zero(button);
    if (pad < 0 || pad >= kPads || button == 0 || bit >= 16)
    {
        return -1;
    }
    const std::int64_t at = g_pads[static_cast<std::size_t>(pad)].downAt[static_cast<std::size_t>(bit)];
    if (at == 0)
    {
        return -1;
    }
    const int other = rules::kReaders - 1 - rules::ReaderOf(pad);
    for (int number = 0; number < kSlots; ++number)
    {
        const int twin = rules::Lane(other, number);
        if (rules::Twins(at, g_pads[static_cast<std::size_t>(twin)].downAt[static_cast<std::size_t>(bit)]))
        {
            return twin;
        }
    }
    return -1;
}

void Listen(Listener listener) noexcept
{
    g_listener.store(listener, std::memory_order_relaxed);
}

void SetPlaying(std::uint32_t slots) noexcept
try
{
    g_playing.store(slots | 1u, std::memory_order_relaxed);
    ApplyIdle(g_windowIdle.load(), rules::MutedPads(Table(), slots | 1u, g_present.load()), L"the players");
}
catch (...)
{
}

void RecheckAnswering(std::uint32_t answeringUsers) noexcept
try
{
    const std::uint8_t* const xi = XInputDevice();
    if (xi == nullptr)
    {
        return;
    }
    std::uint32_t unseen = 0;
    for (int slot = 0; slot < kSlots; ++slot)
    {
        const int    user      = g_lanes[static_cast<std::size_t>(slot)].load(std::memory_order_relaxed);
        std::uint8_t connected = 0;
        if ((answeringUsers >> user & 1u) == 0)
        {
            continue;
        }
        if (!platform::Read(xi + facts::kXInputControllerStates + static_cast<std::uint32_t>(slot) * facts::kXInputControllerStateBytes +
                                facts::kXInputStateConnected,
                            connected))
        {
            return;  // unreadable: nothing decided on it
        }
        unseen |= connected == 0 ? 1u << slot : 0u;
    }
    if (unseen != 0 && FlagRecheck())
    {
        ++g_unseenRechecks;
    }
    if (unseen == g_unseenSlots)
    {
        return;
    }
    const std::uint32_t before = g_unseenSlots;
    g_unseenSlots              = unseen;
    for (int slot = 0; slot < kSlots; ++slot)
    {
        const bool now = (unseen >> slot & 1u) != 0;
        if (now == ((before >> slot & 1u) != 0))
        {
            continue;
        }
        const int user = g_lanes[static_cast<std::size_t>(slot)].load(std::memory_order_relaxed);
        if (now)
        {
            platform::Log(L"pads: {} answers on player {}'s slot but the engine counts that slot empty (no device change it "
                          L"heard since the pad came or moved): the engine's re-check set, so its next poll reads it",
                          rules::PadName(user), slot + 1);
        }
        else
        {
            platform::LogDebug(L"pads: the engine reads player {}'s slot again ({})", slot + 1, rules::PadName(user));
        }
    }
}
catch (...)
{
}

bool Install(const platform::GameImage& image) noexcept
{
    const auto getSlot = image.Rebase(facts::kXInputGetStateImport, sizeof(void*));
    const auto setSlot = image.Rebase(facts::kXInputSetStateImport, sizeof(void*));
    const auto app     = image.Rebase(facts::kWindowsApplicationGlobal, sizeof(void*));
    const auto sony    = image.Rebase(facts::kSonyControllerDeviceVTable, sizeof(void*));
    std::uintptr_t get = 0, set = 0;
    if (!getSlot || !setSlot || !app || !sony || !platform::Read(reinterpret_cast<const void*>(*getSlot), get) ||
        !platform::Read(reinterpret_cast<const void*>(*setSlot), set) || get == 0 || set == 0)
    {
        platform::Log(L"pads: an engine check failed; each pad stays the player with its XInput number");
        return false;
    }
    g_appGlobal  = *app;
    g_sonyVTable = *sony;
    g_get        = reinterpret_cast<GetStateFn>(get);
    g_set        = reinterpret_cast<SetStateFn>(set);
    if (!g_getEntry.Replace(*getSlot, get, reinterpret_cast<std::uintptr_t>(&GetStateFor)))
    {
        platform::Log(L"pads: the game's pad read could not be taken; each pad stays the player with its XInput number");
        return false;
    }
    if (!g_setEntry.Replace(*setSlot, set, reinterpret_cast<std::uintptr_t>(&SetStateFor)))
    {
        g_getEntry.Restore();
        platform::Log(L"pads: the game's rumble call could not be taken; each pad stays the player with its XInput number");
        return false;
    }
    g_installed = true;
    // The PlayStation reader: its read and its rumble together, or neither (then its pads play as the game numbers
    // them, beside the table's XInput pads). The rumble first: the read's moves stop a leaving pad through it.
    const auto readEntry      = image.Rebase(facts::kSonyReadState, facts::kSonyLibraryFirst.size());
    const auto vibrationEntry = image.Rebase(facts::kSonySetVibration, facts::kSonyLibraryFirst.size());
    const bool rumble = vibrationEntry && g_vibration.Install(*vibrationEntry, facts::kSonyLibraryFirst,
                                                              reinterpret_cast<std::uintptr_t>(&SetVibrationFor));
    const bool read = rumble && readEntry &&
                      g_read.Install(*readEntry, facts::kSonyLibraryFirst, reinterpret_cast<std::uintptr_t>(&ReadStateFor));
    if (rumble && !read)
    {
        g_vibration.Restore();
    }
    g_playStation = read;
    // Create, the PlayStation pads' window opener, comes through the library's read only with its system-button switch
    // on; ReadStateFor takes the system buttons out again before the game sees the data.
    const auto         systemButtons = g_playStation ? image.Rebase(facts::kSonySystemButtons, 1) : std::nullopt;
    const std::uint8_t on            = 1;
    g_systemButtons = systemButtons.has_value() && platform::WriteGuarded(reinterpret_cast<void*>(*systemButtons), &on, sizeof(on))
                          ? reinterpret_cast<std::uint8_t*>(*systemButtons)
                          : nullptr;
    try
    {
        platform::Log(L"pads: the game's pad read and rumble go through the pad table ({}; read through {}, rumble through {}); "
                      L"the PlayStation reader's {}",
                      TableText(Table()), ModuleOf(get), ModuleOf(set),
                      g_playStation ? L"too" : L"could not be taken: its pads play as the game numbers them");
        if (g_playStation && g_systemButtons != nullptr)
        {
            platform::Log(L"pads: a PlayStation pad's Create button reaches the mod (the library's system-button switch on), "
                          L"never the game: Create held opens its player's window");
        }
        else if (g_playStation)
        {
            platform::LogWarning(L"pads: the library's system-button switch could not be set: a PlayStation pad's Create never "
                                 L"arrives, and its player's window does not open from it");
        }
        RegisterCommands();
    }
    catch (...)
    {
    }
    LogDevices();
    return true;
}

void Remove() noexcept
{
    g_listener.store(nullptr);
    g_windowIdle.store(0);
    g_muted.store(0);
    g_playing.store(~0u);  // every Start through, should a slot stay ours (a replace not put back)
    // The lanes first: a read still in our redirect then places nothing but each record's own pad.
    for (int lane = 0; lane < kPads; ++lane)
    {
        g_lanes[static_cast<std::size_t>(lane)].store(lane, std::memory_order_relaxed);
    }
    for (auto& live : g_live)
    {
        live.store(rules::kXInput, std::memory_order_relaxed);
    }
    if (g_playStation)
    {
        // The switch off before the read is the game's again: no system button ever reaches it.
        if (g_systemButtons != nullptr)
        {
            const std::uint8_t off = 0;
            platform::WriteGuarded(g_systemButtons, &off, sizeof(off));
            g_systemButtons = nullptr;
        }
        g_read.Restore();
        g_vibration.Restore();
        g_playStation = false;
        // A record still on another pad goes back to its own (the game opened scePad user N+1 on record N), as Place moves
        // one: its connect path runs again for a pad that answers, its disconnect fall for one that does not.
        for (int slot = 0; slot < kSlots; ++slot)
        {
            std::uint8_t* const record = g_records[static_cast<std::size_t>(slot)];
            const std::int32_t  own    = g_handles[static_cast<std::size_t>(slot)];
            std::int32_t        now    = 0;
            std::int32_t        id     = -1;
            // Still the record its reads showed (its controller id unchanged) and on a pad of the table's.
            if (record == nullptr || own == 0 || !platform::Read(record + facts::kSonyRecordId, id) || id != slot ||
                !platform::Read(record + facts::kSonyRecordHandle, now) || now == own || PlayStationPad(now) < 0 ||
                !platform::WriteGuarded(record + facts::kSonyRecordHandle, &own, sizeof(own)))
            {
                continue;
            }
            if ((g_present.load() >> rules::Lane(rules::kPlayStation, slot) & 1u) != 0)
            {
                const std::uint8_t off = 0;
                platform::WriteGuarded(record + facts::kSonyRecordConnected, &off, sizeof(off));
            }
        }
    }
    g_setEntry.Restore();
    g_getEntry.Restore();
    if (g_installed)
    {
        FlagRecheck();
    }
    g_installed = false;
}

}  // namespace lss::players::pad_table
