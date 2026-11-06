#pragma once

// The game's own window and resolution, reached through the engine: the levers the span (views/window_span) pulls.
//
//   - The native window: GEngine -> UGameEngine::GameViewportWindow -> SWindow::NativeWindow, an FWindowsWindow. Its
//     ReshapeWindow is the call at the bottom of every path the engine moves its window by; GetWindowMode and
//     GetOSWindowHandle say what it is now.
//   - The resolution: FSystemResolution::RequestResolutionChange, the options menu's own path, and the GSystemResolution
//     global it ends in, read only.
// Every read is guarded and the window's vtable must be the one the research found (research/RE-multi-monitor-views.md).
// Game thread only. Install logs what is missing; the calls log nothing, their callers say what happened.

#include "views/monitor_row.h"

#include <cstdint>
#include <optional>
#include <string>

struct HWND__;

namespace lss::platform {
class GameImage;
}

namespace lss::views::game_window {

using monitor_row::Rect;

// EWindowMode, as GetWindowMode answers and a resolution request takes.
inline constexpr std::int32_t kModeFullscreen         = 0;  // exclusive
inline constexpr std::int32_t kModeWindowedFullscreen = 1;  // Borderless
inline constexpr std::int32_t kModeWindowed           = 2;

struct Resolution
{
    std::int32_t x = 0, y = 0, mode = 0;
    bool         operator==(const Resolution&) const noexcept = default;
};

/// Verifies the window chain's code, the resolution request and the resolution global in the running build. False
/// (logged) if they are not as researched; nothing else here may be called then.
bool Install(const platform::GameImage& image) noexcept;

struct Window
{
    void*        object = nullptr;  // the FWindowsWindow
    HWND__*      hwnd   = nullptr;
    std::int32_t mode   = -1;
};

/// The game's native window, or nothing with the reason in `why`: its handle must name a top-level window of this process.
std::optional<Window> Find(const wchar_t** why) noexcept;

/// The window's client area in screen coordinates: what ReshapeWindow takes (it adds the OS border itself).
Rect ClientRect(HWND__* hwnd) noexcept;

/// The engine's own reshape of its window to `rect`. False if the call faulted.
bool Reshape(void* window, const Rect& rect) noexcept;

/// The game's own resolution request; the engine applies it on its next frame. False if the call faulted.
bool Request(const Resolution& resolution) noexcept;

/// GSystemResolution now; zeros if unread.
Resolution ReadResolution() noexcept;

// For the log.
std::wstring   RectText(const Rect& rect);
const wchar_t* ModeName(std::int32_t mode) noexcept;
std::wstring   ResolutionText(const Resolution& resolution);
/// The game viewport's size (FViewport SizeX/SizeY), or "unread".
std::wstring   ViewportText() noexcept;

}  // namespace lss::views::game_window
