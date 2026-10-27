#pragma once

// The engine's single view: the whole screen player 1's view alone while further players are in (their controllers,
// characters and pads untouched). The one-window outcome for the moments views/column_source.h decides (a full-screen
// menu, a shop, a cutscene, a minigame: views/whole_screen.h; the user's ruling of 2026-10-26 and of 2026-10-27);
// this class only holds the engine's switch to what it is told. Rebirth's players/single_view, taken as
// it is (its rules were played there since v0.34); only the switch's access differs (players/split_switch.h: 4.18
// has no reflected setter, the viewport's byte is written directly).
//
// The engine lays the local players out from one stock switch, UGameViewportClient::bDisableSplitScreenOverride:
// while it is set the split type is None whatever the player count (UpdateActiveSplitscreenType,
// research/RE-layout-fov.md §2.2), player 1's view fills the screen and every further local player gets a zero-size
// view: no scene view, no render, no listener update, no HUD pass. The engine lays the players out again on every
// redraw, so a write during the engine tick shows in that frame's draw.
//
// The engine is reached through SplitSwitchAccess, so this policy has no engine dependency and is tested on the host
// (tests/single_view_test.cpp): a value that cannot be read is never overwritten, a value someone else set is left
// alone, and a failure is reported once per request rather than retried every tick.

#include <optional>
#include <string>

namespace lss::players {

struct SplitSwitchAccess
{
    std::optional<bool> (*read)() noexcept;  // true while the split is force-disabled
    bool (*write)(bool disabled) noexcept;   // false if the switch could not be written
};

/// What an Update did, for the caller to log.
enum class SingleViewChange
{
    None,           // nothing to do
    Single,         // the mod set the switch: player 1's view alone
    AlreadySingle,  // the switch was already set by someone else; it will not be cleared by the mod
    SingleFailed,   // it could not be read, set, or confirmed; the split stays (until the request changes)
    Split,          // the mod cleared its switch: the split is back
    LeftAlone,      // the split was asked back, but the switch is not the mod's (or someone cleared it)
    SplitFailed,    // it could not be read or cleared; the screen stays single (until the request changes)
};

/// The change for the log (empty for None).
std::wstring Describe(SingleViewChange change);

class SingleView
{
public:
    explicit SingleView(SplitSwitchAccess access) noexcept : access_(access) {}

    /// Call on the game thread every tick with whether the single view is wanted (views/column_source.h decides;
    /// false once no further player is in). Acts only when the request changes (or a failed request is asked for
    /// again after a change).
    SingleViewChange Update(bool wanted) noexcept;

    /// At the mod's end: the mod's own switch cleared if it is still set, a failed last request notwithstanding (a
    /// switch found set stays). None when there is nothing to clear.
    SingleViewChange Release() noexcept;

    /// True while the screen shows player 1's view alone through the switch (the mod's or one found set). The
    /// split's per-player presentation (the further players' HUD images, the halves' layout) is off.
    bool Single() const noexcept { return single_; }

private:
    SingleViewChange SetSingle() noexcept;
    SingleViewChange SetSplit() noexcept;

    SplitSwitchAccess access_;
    bool              single_ = false;  // the switch is set (by the mod when owned_)
    bool              owned_  = false;  // the mod set it, so the mod clears it
    bool              wanted_ = false;  // the last request
    bool              failed_ = false;  // the last request failed; not retried until it changes
};

}  // namespace lss::players
