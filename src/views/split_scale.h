#pragma once

// Split views fill their part of the window whatever the game's resolution option, which can be changed while the
// game runs (plan/next-phase.md §4; research/RE-split-upscale.md).
//
// Square Enix sizes the scene buffer from the resolution option, not the window. In single player its Borderless mode
// draws the one view at the option's size and its final pass scales it to the window; in split the mod switches that
// fit off (views/letterbox_patch) and each view keeps its rect in window pixels. An option below the window then leaves
// the part of each view beyond the buffer undrawn (streaks: seen with 1920x1080 on a 2560x1440
// desktop). The game's own dynamic resolution scales every view by one fraction without touching the buffer, and its
// final pass then scales each view into its rect, as single player does with its one view. So while further views are
// shown the dynamic-resolution cap (r.DynamicRes.MaxScreenPercentage) is held at resolution / window
// (views/split_scale_rules.h): each view renders at its share of the option's pixels, the option acting as the render
// scale it is in single player; at an option at or above the window nothing changes, and a lower cap the player set
// is kept. A pause would hand the views to the legacy driver (the streaks again), so r.DynamicRes.PausingDenied is held
// at 1 with it. The game sizes the bloom area (bloom, its glare passes, the narrow filter) from the views' full size
// without that fraction, into targets as large as the buffer: past the buffer's width they repeat their last column
// (light streaks, seen on a 4096x2160 TV). While the views are held, its two products in
// PrepareViewRectsForRendering take the views' own scale (one operand each, research/RE-split-bloom-streaks.md), and are
// put back with the hold (also at an unload: a code swap is safe from any thread).
// Only while the letterbox patch is applied (the views in window pixels); the sizes are sampled twice a
// second and acted on once two samples agree (a resolution change and its window resize land on different frames).
// Both values go through the engine's console (the game's own setting path; they are left at console priority after),
// each held by Rebirth's players/held_setting rules, by hand here (two values in one
// module): the value read before every write (one the mod did not write is the one to put back), the mod's counted as
// written once its command ran, and put back only while the mod's is still in place. Logged at Info: each hold (with
// the resolution that would show the views at full sharpness), each release, the game's dynamic resolution enabled; a
// Warning when it is not (the cap then cannot act), at each change. `split-scale on|off` (on), reading
// `split-scale.state`. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::views::split_scale {

/// Runs one engine console command (players::Session::Console, as typed in the console); false if it could not.
using Console = bool (*)(const wchar_t* command) noexcept;

/// Finds GEngine, the resolution, the two console variables' values and the dynamic-resolution status; registers the
/// commands. False (logged) if anything differs from the research: split keeps the streaks at a low option.
bool Install(const platform::GameImage& image, Console console) noexcept;

/// Each engine tick, after the layout: `split` while the views are laid out in window pixels (the letterbox patch
/// applied: further players, a mirror, or the span's hold with one view).
void Tick(bool split) noexcept;

/// Puts the two values back if the mod's are in place. Runs only from UE4SS's mod reload (the destructor never runs at
/// process exit), on UE4SS's thread: there the values are left as the mod set them, and said so, since the engine's
/// console is game-thread code.
void Remove() noexcept;

}  // namespace lss::views::split_scale
