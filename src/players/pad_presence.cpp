#include "players/pad_presence.h"

#include "control/registry.h"
#include "platform/log.h"
#include "players/pad_table.h"

#include <array>
#include <cstdint>
#include <format>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Xinput.h>

namespace lss::players::pad_presence {

namespace {

using GetStateFn = DWORD(WINAPI*)(DWORD user, XINPUT_STATE* state);

constexpr unsigned  kUsers       = 4;     // XInput users 0..3
constexpr ULONGLONG kPollEveryMs = 1000;  // the game polls every frame; a disconnected slot's query is the slow one

struct Pad
{
    bool          connected = false;
    DWORD         packet    = 0;  // XInput's packet number: changes when the pad reports a change
    ULONGLONG     changedAt = 0;  // when it last changed (0: never seen)
};

// Game thread only.
GetStateFn              g_getState   = nullptr;
bool                    g_loadTried  = false;
bool                    g_polled     = false;  // the first poll logs the set
ULONGLONG               g_nextPollAt = 0;
std::array<Pad, kUsers> g_pads{};

GetStateFn Load() noexcept
{
    if (!g_loadTried)
    {
        g_loadTried = true;
        // The game's own XInput first (its only import, RE-pad-slots.md §2.5): the poll sees what the game's reader sees.
        for (const wchar_t* name : {L"xinput1_3.dll", L"xinput1_4.dll", L"xinput9_1_0.dll"})
        {
            if (HMODULE module = LoadLibraryW(name); module != nullptr)
            {
                g_getState = reinterpret_cast<GetStateFn>(GetProcAddress(module, "XInputGetState"));
                if (g_getState != nullptr)
                {
                    break;
                }
            }
        }
        if (g_getState == nullptr)
        {
            platform::LogWarning(L"pads: no XInput library found; the pads reading stays unknown");
        }
    }
    return g_getState;
}

std::wstring ConnectedList(const std::array<Pad, kUsers>& pads)
{
    std::wstring list;
    for (unsigned user = 0; user < kUsers; ++user)
    {
        if (pads[user].connected)
        {
            list += list.empty() ? std::format(L"{}", user) : std::format(L", {}", user);
        }
    }
    return list.empty() ? L"none" : list;
}

void Poll(ULONGLONG now) noexcept
{
    GetStateFn getState = Load();
    if (getState == nullptr)
    {
        return;
    }
    const std::array<Pad, kUsers> before = g_pads;
    for (unsigned user = 0; user < kUsers; ++user)
    {
        XINPUT_STATE state{};
        Pad&         pad = g_pads[user];
        pad.connected    = getState(user, &state) == ERROR_SUCCESS;
        if (pad.connected && (state.dwPacketNumber != pad.packet || pad.changedAt == 0))
        {
            pad.packet    = state.dwPacketNumber;
            pad.changedAt = now;
        }
    }
    bool          changed   = !g_polled;
    std::uint32_t answering = 0;
    for (unsigned user = 0; user < kUsers; ++user)
    {
        changed = changed || before[user].connected != g_pads[user].connected;
        answering |= g_pads[user].connected ? 1u << user : 0u;
    }
    pad_table::RecheckAnswering(answering);  // a pad the engine counts absent is read again (pad_table.h)
    if (changed)
    {
        if (g_polled)
        {
            platform::LogDebug(L"pads: XInput users connected {} -> {}", ConnectedList(before), ConnectedList(g_pads));
        }
        else
        {
            platform::Log(L"pads: XInput users connected: {}", ConnectedList(g_pads));
        }
    }
    g_polled = true;
}

std::wstring Reading(ULONGLONG now)
{
    if (!g_polled)
    {
        return L"unknown (not polled yet)";
    }
    std::wstring text = std::format(L"XInput users connected: {}", ConnectedList(g_pads));
    for (unsigned user = 0; user < kUsers; ++user)
    {
        const Pad& pad = g_pads[user];
        if (!pad.connected)
        {
            continue;
        }
        text += pad.changedAt == 0 ? std::format(L"; pad {} (user {}): no report yet", user + 1, user)
                                   : std::format(L"; pad {} (user {}): packet {} changed {:.1f} s ago", user + 1, user,
                                                 pad.packet, static_cast<double>(now - pad.changedAt) / 1000.0);
    }
    return text;
}

}  // namespace

void Register()
{
    control::Register({.name  = L"pads",
                       .shape = control::Shape::Text,
                       .help  = L"the XInput users with a pad, polled once a second, and when each pad last reported "
                                L"a change (players/pad_presence.h)",
                       .get   = [] { return control::Value{Reading(GetTickCount64())}; }});
}

void Tick() noexcept
{
    const ULONGLONG now = GetTickCount64();
    if (now < g_nextPollAt)
    {
        return;
    }
    g_nextPollAt = now + kPollEveryMs;
    Poll(now);
}

}  // namespace lss::players::pad_presence
