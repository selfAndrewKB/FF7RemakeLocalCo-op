#include "players/pad_table.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/vtable_entry.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Xinput.h>

namespace lss::players::pad_table {

namespace {

namespace rules = pad_claim_rules;

using GetStateFn = DWORD(WINAPI*)(DWORD user, XINPUT_STATE* state);
using SetStateFn = DWORD(WINAPI*)(DWORD user, XINPUT_VIBRATION* vibration);

constexpr int kSlots = rules::kSlots;

/// One physical pad, as the game's poll last read it. Game thread (the poll and the console both run there).
struct Pad
{
    rules::OpenHold  hold;
    rules::Held      leave;  // B held: the claim's leave
    rules::LetGo     letGo;  // idle to the game until it lets go of what it held at a change
    std::uint16_t    last = 0;  // the buttons on its last poll (the listener's presses)
};

platform::VTableEntry g_getEntry;
platform::VTableEntry g_setEntry;
GetStateFn            g_get       = nullptr;  // what the import slots held: XInput's own, or whatever was there first
SetStateFn            g_set       = nullptr;
std::uintptr_t        g_appGlobal  = 0;
std::uintptr_t        g_sonyVTable = 0;
bool                  g_installed  = false;

std::array<std::atomic<int>, kSlots> g_table{0, 1, 2, 3};
std::atomic<Listener>                g_listener{nullptr};
std::atomic<bool>                    g_idle{false};
std::atomic<std::uint32_t>           g_playing{1};  // the slots a player holds (SetPlaying)

// Game thread.
std::array<Pad, kSlots> g_pads{};
std::uint32_t           g_opens = 0, g_waited = 0, g_rechecks = 0, g_changes = 0, g_startsHeld = 0;
std::wstring            g_last   = L"none yet";

std::int64_t NowMs() noexcept
{
    return static_cast<std::int64_t>(GetTickCount64());
}

/// "pad 2" for XInput user 1: the numbering the `pads` reading uses.
std::wstring PadName(int user)
{
    return std::format(L"pad {}", user + 1);
}

std::wstring TableText(const rules::PadTable& table)
{
    return std::format(L"player 1 {}, player 2 {}, player 3 {}, spare {}", PadName(table[0]), PadName(table[1]),
                       PadName(table[2]), PadName(table[3]));
}

/// The engine's own "poll every slot again" byte, as its WM_DEVICECHANGE handling sets it.
bool FlagRecheck() noexcept
{
    void* const app = platform::ReadPointer(reinterpret_cast<const void*>(g_appGlobal), 0);
    void* const xi  = app != nullptr ? platform::ReadPointer(app, facts::kAppXInputInterface) : nullptr;
    const std::uint8_t on = 1;
    if (xi == nullptr || !platform::WriteGuarded(static_cast<std::uint8_t*>(xi) + facts::kXInputNeedsStateUpdate, &on, 1))
    {
        platform::LogWarning(L"pads: the engine's pad re-check could not be set; a pad moved onto a slot it counted empty "
                             L"is read after the next device change");
        return false;
    }
    ++g_rechecks;
    return true;
}

void Idle(XINPUT_STATE& state) noexcept
{
    state.Gamepad = XINPUT_GAMEPAD{};
}

/// The game's poll of `slot`: the assigned pad's state, gated (see the header).
DWORD WINAPI GetStateFor(DWORD slot, XINPUT_STATE* state) noexcept
{
    if (slot >= static_cast<DWORD>(kSlots) || state == nullptr)
    {
        return g_get(slot, state);
    }
    const int   user   = g_table[slot].load(std::memory_order_relaxed);
    const DWORD result = g_get(static_cast<DWORD>(user), state);
    if (result != ERROR_SUCCESS)
    {
        *state = XINPUT_STATE{};  // the engine reads the buffer as left after a failure (RE-pad-slots.md §3)
        g_pads[static_cast<std::size_t>(user)].hold  = {};  // a View held back as it dropped out is not a tap on its return
        g_pads[static_cast<std::size_t>(user)].leave = {};
        g_pads[static_cast<std::size_t>(user)].last  = 0;
        return result;
    }
    Pad&                pad   = g_pads[static_cast<std::size_t>(user)];
    const std::uint16_t raw   = state->Gamepad.wButtons;
    const std::int64_t  now   = NowMs();
    const rules::Gated  gated = rules::Gate(pad.hold, raw, now);
    const auto          pressed = static_cast<std::uint16_t>(raw & ~pad.last);
    const bool          leave   = rules::Hold(pad.leave, (raw & rules::kB) != 0, now, rules::kLeaveHoldMs);
    pad.last                    = raw;
    if (const Listener listener = g_listener.load(std::memory_order_relaxed);
        listener != nullptr && (pressed != 0 || gated.open || leave))
    {
        listener(user, pressed, gated.open, leave);
    }
    if (gated.open)
    {
        ++g_opens;
        try
        {
            g_last = std::format(L"View held 1 s on {} (player {}'s slot)", PadName(user), slot + 1);
            platform::Log(L"pads: {}", g_last);
        }
        catch (...)
        {
        }
    }
    if (g_idle.load(std::memory_order_relaxed))
    {
        Idle(*state);
        return result;
    }
    const std::uint8_t left  = state->Gamepad.bLeftTrigger;
    const std::uint8_t right = state->Gamepad.bRightTrigger;
    const rules::Wait  wait  = rules::Poll(pad.letGo, raw, left, right, now);
    if (wait != rules::Wait::None)
    {
        // Idle on the poll that ends the wait too: a held-back View released on it would otherwise reach the
        // new slot as a tap.
        Idle(*state);
        try
        {
            if (wait == rules::Wait::LetGoNow)
            {
                platform::LogDebug(L"pads: {} let go: it reaches the game again (player {}'s slot)", PadName(user), slot + 1);
            }
            else if (wait == rules::Wait::TriggerRests)
            {
                platform::Log(L"pads: {}'s triggers rest at {} and {} (of 255) 2 s after its buttons let go: it reaches the "
                              L"game again (player {}'s slot)",
                              PadName(user), left, right, slot + 1);
            }
            else if (wait == rules::Wait::ButtonStuck)
            {
                platform::Log(L"pads: {} still holds buttons {:#06x} 10 s after the change: it reaches the game again (player "
                              L"{}'s slot)",
                              PadName(user), raw, slot + 1);
            }
        }
        catch (...)
        {
        }
        return result;
    }
    // A pad that plays no one never pauses the game: its Start is the claim's join (heard above).
    const std::uint16_t shown = rules::StartGate(gated.buttons, static_cast<int>(slot), g_playing.load(std::memory_order_relaxed));
    if ((pressed & rules::kStart) != 0 && (shown & rules::kStart) == 0)
    {
        ++g_startsHeld;
        platform::LogDebug(L"pads: {}'s Start held back from the game (player {}'s slot, no player there): a join",
                           PadName(user), slot + 1);
    }
    state->Gamepad.wButtons = shown;
    return result;
}

/// The game's rumble for `slot`: to the assigned pad; off while idle.
DWORD WINAPI SetStateFor(DWORD slot, XINPUT_VIBRATION* vibration) noexcept
{
    if (slot >= static_cast<DWORD>(kSlots))
    {
        return g_set(slot, vibration);
    }
    const auto user = static_cast<DWORD>(g_table[slot].load(std::memory_order_relaxed));
    if (g_idle.load(std::memory_order_relaxed))
    {
        XINPUT_VIBRATION off{};
        return g_set(user, &off);
    }
    return g_set(user, vibration);
}

/// Every pad still holding what it holds now stays idle to the game until it lets go of that.
void AwaitReleases() noexcept
{
    for (Pad& pad : g_pads)
    {
        rules::AwaitLetGo(pad.letGo);
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

}  // namespace

void LogDevices() noexcept
{
    void* const   app   = platform::ReadPointer(reinterpret_cast<const void*>(g_appGlobal), 0);
    void*         data  = app != nullptr ? platform::ReadPointer(app, facts::kAppInputDevices) : nullptr;
    std::int32_t  count = 0;
    if (app == nullptr || !platform::Read(static_cast<const std::uint8_t*>(app) + facts::kAppInputDevices + 8, count) ||
        count < 0 || count > 64)
    {
        platform::LogDebug(L"pads: the engine's other input devices are not readable");
        return;
    }
    int sony = 0;
    for (std::int32_t i = 0; i < count && data != nullptr; ++i)
    {
        void* const device = platform::ReadPointer(data, static_cast<std::uint32_t>(i * sizeof(void*)));
        void* const vtable = device != nullptr ? platform::ReadPointer(device, 0) : nullptr;
        sony += reinterpret_cast<std::uintptr_t>(vtable) == g_sonyVTable ? 1 : 0;
    }
    platform::Log(L"pads: the engine lists {} other input device(s){}", count,
                  sony > 0 ? L", one the PlayStation pad reader: a PlayStation pad it reads is not moved by the pad table"
                           : L", none the PlayStation pad reader");
}

namespace {

std::wstring Reading()
{
    return std::format(L"{}{}; {}; slots with a player {:#x}; View held {}, Starts held back {}, waits for a let-go {}, "
                       L"re-checks {}, changes {}; last: {}",
                       g_installed ? L"on" : L"not installed", g_idle.load() ? L" (every pad idle)" : L"",
                       TableText(Table()), g_playing.load(), g_opens, g_startsHeld, g_waited, g_rechecks, g_changes, g_last);
}

}  // namespace

rules::PadTable Table() noexcept
{
    rules::PadTable table{};
    for (int slot = 0; slot < kSlots; ++slot)
    {
        table[static_cast<std::size_t>(slot)] = g_table[static_cast<std::size_t>(slot)].load(std::memory_order_relaxed);
    }
    return table;
}

bool Set(const rules::PadTable& table, const wchar_t* why) noexcept
try
{
    if (!g_installed || !rules::IsPermutation(table))
    {
        return false;
    }
    const rules::PadTable before = Table();
    if (before == table)
    {
        return true;
    }
    for (int slot = 0; slot < kSlots; ++slot)
    {
        g_table[static_cast<std::size_t>(slot)].store(table[static_cast<std::size_t>(slot)], std::memory_order_relaxed);
    }
    AwaitReleases();
    ++g_waited;
    ++g_changes;
    FlagRecheck();
    // Every pad's motors off: a pad keeps the last rumble it was sent, and its slot may no longer be the one the game
    // sends it for. The game's next send per slot sets the new owner's.
    for (int user = 0; user < kSlots; ++user)
    {
        XINPUT_VIBRATION off{};
        g_set(static_cast<DWORD>(user), &off);
    }
    g_last = std::format(L"{} (was {}): {}", TableText(table), TableText(before), why);
    platform::Log(L"pads: {}", g_last);
    return true;
}
catch (...)
{
    return false;
}

void SetIdle(bool on, const wchar_t* why) noexcept
try
{
    if (!g_installed || g_idle.exchange(on) == on)
    {
        return;
    }
    if (!on)
    {
        AwaitReleases();
        ++g_waited;
    }
    g_last = std::format(L"every pad {} to the game: {}", on ? L"idle" : L"back", why);
    platform::Log(L"pads: {}", g_last);
}
catch (...)
{
}

void SetPlaying(std::uint32_t slots) noexcept
{
    g_playing.store(slots | 1u, std::memory_order_relaxed);
}

void Listen(Listener listener) noexcept
{
    g_listener.store(listener, std::memory_order_relaxed);
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
    platform::Log(L"pads: the game's pad read and rumble go through the pad table ({}; read through {}, rumble through {})",
                  TableText(Table()), ModuleOf(get), ModuleOf(set));
    LogDevices();

    control::Register({.name  = L"pad-table",
                       .shape = control::Shape::Text,
                       .help  = L"which pad each player has: four pad numbers for players 1, 2, 3 and the spare slot, "
                                L"each 1-4 once (`pad-table 2 1 3 4`: pad 2 plays player 1, pad 1 player 2)",
                       .set   = [](const control::Value& value) {
                           const auto& text = std::get<std::wstring>(value);
                           rules::PadTable table{};
                           int             count = 0;
                           for (const wchar_t c : text)
                           {
                               if (c >= L'1' && c <= L'4' && count < kSlots)
                               {
                                   table[static_cast<std::size_t>(count++)] = c - L'1';
                               }
                               else if (c != L' ' && c != L',')
                               {
                                   count = kSlots + 1;
                               }
                           }
                           if (count != kSlots || !rules::IsPermutation(table))
                           {
                               return control::Outcome{false, L"four pad numbers 1-4, each once"};
                           }
                           Set(table, L"the console");
                           return control::Outcome{true, TableText(Table())};
                       },
                       .get = [] { return control::Value{TableText(Table())}; }});
    control::Register({.name  = L"pads-idle",
                       .shape = control::Shape::OnOff,
                       .help  = L"every connected pad reads idle to the game, rumble off (the claim screen's state; "
                                L"off: each pad holding a button stays idle until it lets go)",
                       .set   = [](const control::Value& value) {
                           SetIdle(std::get<bool>(value), L"the console");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_idle.load()}; }});
    control::Register({.name  = L"players.pad-table",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the pad table, the View holds and the waits for a let-go",
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
    return true;
}

void Remove() noexcept
{
    g_listener.store(nullptr);
    g_idle.store(false);
    g_setEntry.Restore();
    g_getEntry.Restore();
    if (g_installed)
    {
        for (int slot = 0; slot < kSlots; ++slot)
        {
            g_table[static_cast<std::size_t>(slot)].store(slot, std::memory_order_relaxed);
        }
        FlagRecheck();
    }
    g_installed = false;
}

}  // namespace lss::players::pad_table
