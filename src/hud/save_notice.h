#pragma once

// A line of the mod's in the game's save-data notice, the box at the top right ("Checking saved data...";
// research/RE-title-notice.md): the one notice the game draws on the title screen. The notice is the game's own: its
// show with the "Checking" look, the mod's text set on the widget's one text block with the game's setter, and the
// game's own hide (EndMenuAPI's reflected one) when the line's time is up. It has no text of its own to take and no
// duration, and the game's save check at the title's press uses the same widget: the line waits while the game's
// notice is up or its save system is busy, and is drawn again when the game's check hid the box early. One line at a
// time; a new one replaces the old. Game thread.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::hud::save_notice {

/// Finds the game's show and text setter (checked; logged). False (nothing shows) if either differs.
bool Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// `text` in the notice for `seconds` from its first draw: when the game's notice is down and its save system idle, or
/// at once over the mod's own line; dropped if the game's holds the box past `seconds`.
void Show(std::wstring text, int seconds) noexcept;

/// Once per engine tick: the line drawn when due, drawn again when the game's check hid it early, hidden when its
/// time is up.
void Tick() noexcept;

/// The line ended now, hidden if it is up (the title screen left: no line of its over a loading screen).
void End() noexcept;

}  // namespace lss::hud::save_notice
