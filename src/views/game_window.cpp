#include "views/game_window.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/game_globals.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <format>

namespace lss::views::game_window {

namespace {

using platform::Read;

// FGenericWindow's virtuals the module calls, by their stock 4.18 slot (the research confirmed the shipped table).
using ReshapeWindowFn     = void (*)(void* self, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height);
using GetWindowModeFn     = std::int32_t (*)(void* self);
using GetOSWindowHandleFn = void* (*)(void* self);
/// FSystemResolution::RequestResolutionChange(width, height, EWindowMode): sets r.SetRes at console priority.
using RequestResolutionFn = std::uint64_t (*)(std::uint32_t width, std::uint32_t height, std::int32_t mode);

std::uintptr_t      g_vtable    = 0;        // FWindowsWindow's vtable, live
RequestResolutionFn g_request   = nullptr;  // live
const std::int32_t* g_systemRes = nullptr;  // GSystemResolution, live (read only)

template <class Fn>
Fn Slot(std::size_t index) noexcept
{
    std::uintptr_t entry = 0;
    return Read(reinterpret_cast<const void*>(g_vtable + index * sizeof(void*)), entry) ? reinterpret_cast<Fn>(entry) : nullptr;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto engineGlobal = players::game_globals::Address(players::game_globals::Global::Engine);
    const auto vtable       = image.Rebase(facts::kWindowsWindowVTable, 30 * sizeof(std::uintptr_t));
    const auto systemRes    = players::game_globals::Address(players::game_globals::Global::SystemResolution);
    const auto reshape      = platform::FindFunction(image, facts::kWindowsWindowReshapeWindow);
    const auto mode         = platform::FindFunction(image, facts::kWindowsWindowGetWindowMode);
    const auto handle       = platform::FindFunction(image, facts::kWindowsWindowGetOSWindowHandle);
    const auto request      = platform::FindFunction(image, facts::kRequestResolutionChange);
    const auto slotReshape  = image.ReadPointer(facts::kWindowsWindowVTable + facts::kWindowsWindowSlotReshapeWindow * 8);
    const auto slotMode     = image.ReadPointer(facts::kWindowsWindowVTable + facts::kWindowsWindowSlotGetWindowMode * 8);
    const auto slotHandle   = image.ReadPointer(facts::kWindowsWindowVTable + facts::kWindowsWindowSlotGetOSWindowHandle * 8);
    const bool slotsHold    = reshape && mode && handle && slotReshape && slotMode && slotHandle && *slotReshape == *reshape &&
                           *slotMode == *mode && *slotHandle == *handle;
    const bool witness = image.BytesMatch(facts::kSetWindowTitleExec.address, facts::kSetWindowTitleExec.entryBytes);
    if (!engineGlobal || !vtable || !systemRes || !request || !slotsHold || !witness)
    {
        platform::LogWarning(L"window span: the game window's code is not what the research read (GEngine {}, vtable {}, "
                             L"slots {}, resolution request {}, witness {}); the span is unavailable",
                             engineGlobal ? L"found" : L"missing", vtable ? L"found" : L"missing", slotsHold ? L"hold" : L"differ",
                             request ? L"found" : L"missing", witness ? L"matches" : L"differs");
        return false;
    }
    g_vtable    = *vtable;
    g_systemRes = reinterpret_cast<const std::int32_t*>(systemRes);
    g_request   = reinterpret_cast<RequestResolutionFn>(*request);
    return true;
}

/// GEngine -> GameViewportWindow (TWeakPtr<SWindow>: the object pointer first) -> SWindow::NativeWindow (TSharedPtr: the
/// object pointer first).
std::optional<Window> Find(const wchar_t** why) noexcept
{
    std::uintptr_t engine = 0, swindow = 0, native = 0, vtable = 0;
    engine = reinterpret_cast<std::uintptr_t>(players::game_globals::Object(players::game_globals::Global::Engine));
    if (engine == 0)
    {
        *why = L"GEngine is not set";
        return std::nullopt;
    }
    if (!Read(reinterpret_cast<const void*>(engine + facts::kGameEngineViewportWindow), swindow) || swindow == 0)
    {
        *why = L"UGameEngine::GameViewportWindow is empty";
        return std::nullopt;
    }
    if (!Read(reinterpret_cast<const void*>(swindow + facts::kSWindowNativeWindow), native) || native == 0)
    {
        *why = L"SWindow::NativeWindow is empty";
        return std::nullopt;
    }
    if (!Read(reinterpret_cast<const void*>(native), vtable) || vtable != g_vtable)
    {
        *why = L"the native window's vtable is not FWindowsWindow's";
        return std::nullopt;
    }
    Window window;
    window.object = reinterpret_cast<void*>(native);
    void* handle  = nullptr;
    if (!platform::CallGuarded(Slot<GetOSWindowHandleFn>(facts::kWindowsWindowSlotGetOSWindowHandle), handle, window.object))
    {
        *why = L"GetOSWindowHandle faulted";
        return std::nullopt;
    }
    window.hwnd = static_cast<HWND>(handle);
    DWORD owner = 0;
    GetWindowThreadProcessId(window.hwnd, &owner);
    if (!IsWindow(window.hwnd) || owner != GetCurrentProcessId() || GetParent(window.hwnd) != nullptr)
    {
        *why = L"GetOSWindowHandle is not a top-level window of this process";
        return std::nullopt;
    }
    if (!platform::CallGuarded(Slot<GetWindowModeFn>(facts::kWindowsWindowSlotGetWindowMode), window.mode, window.object))
    {
        *why = L"GetWindowMode faulted";
        return std::nullopt;
    }
    return window;
}

Rect ClientRect(HWND hwnd) noexcept
{
    RECT  client{};
    POINT origin{};
    GetClientRect(hwnd, &client);
    ClientToScreen(hwnd, &origin);
    return {origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
}

bool Reshape(void* window, const Rect& rect) noexcept
{
    return platform::CallGuarded(Slot<ReshapeWindowFn>(facts::kWindowsWindowSlotReshapeWindow), window, rect.left, rect.top,
                                 rect.Width(), rect.Height());
}

bool Request(const Resolution& resolution) noexcept
{
    std::uint64_t result = 0;
    return platform::CallGuarded(g_request, result, static_cast<std::uint32_t>(resolution.x), static_cast<std::uint32_t>(resolution.y),
                                 resolution.mode);
}

Resolution ReadResolution() noexcept
{
    Resolution   resolution;
    std::int32_t values[3]{};
    if (platform::CopyGuarded(values, g_systemRes, sizeof(values)))
    {
        resolution = {values[0], values[1], values[2]};
    }
    return resolution;
}

std::wstring RectText(const Rect& r)
{
    return std::format(L"({}, {}, {}, {}) {}x{}", r.left, r.top, r.right, r.bottom, r.Width(), r.Height());
}

const wchar_t* ModeName(std::int32_t mode) noexcept
{
    switch (mode)
    {
        case kModeFullscreen: return L"Fullscreen (exclusive)";
        case kModeWindowedFullscreen: return L"Borderless";
        case kModeWindowed: return L"Windowed";
        default: return L"unknown";
    }
}

std::wstring ResolutionText(const Resolution& resolution)
{
    return std::format(L"{}x{} {}", resolution.x, resolution.y, ModeName(resolution.mode));
}

std::wstring ViewportText() noexcept
{
    std::uintptr_t client = 0, viewport = 0;
    std::int32_t   size[2]{};
    client = reinterpret_cast<std::uintptr_t>(players::game_globals::GameViewportClient());
    if (client != 0 &&
        Read(reinterpret_cast<const void*>(client + facts::kViewportClientViewport), viewport) && viewport != 0 &&
        platform::CopyGuarded(size, reinterpret_cast<const void*>(viewport + facts::kViewportSizeX), sizeof(size)))
    {
        return std::format(L"{}x{}", size[0], size[1]);
    }
    return L"unread";
}

}  // namespace lss::views::game_window
