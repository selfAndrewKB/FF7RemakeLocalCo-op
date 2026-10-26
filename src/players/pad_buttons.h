#pragma once

// Each pad's buttons for its own player (RE-input.md §1, §8; RE-input-p2.md §2: the design, a written exception).
//
// The game keeps one merged action state for every pad: a second pad presses and releases player 1's actions,
// and opens player 1's menus. The pad is known in exactly one place, the Slate key event's UserIndex in the
// input object's key handlers, so that is where a pad's events are told apart. For each pad beyond the first
// (XInput users 1.., one per further player) the mod keeps a copy of the action state (players/action_tables.h)
// and, around that pad's key events, exchanges it with the live bytes: the game's own key mapping, press rules
// and release write the pad's copy. The copy is exchanged in again around that player's own engine calls (their
// controller's input step and movement, beside their stick scope), so the game's own pollers read it there, and
// the live tables hold exactly what a one-pad game would: pad 1, the keyboard and the game's synthetic keys.
// The only rule restated is the game's per-frame advance (prev = held unless latched; latch cleared), made on
// each copy after the game's input tick. In that tick the stick aggregation sees pad 1's record alone, so the
// aggregate and the menus' stick cursor are pad 1's natively; each further pad's record is read by its own
// stick scope.
//
// Where a further pad's press would reach the UI: the game's dispatcher is told the press was already handled
// (its own argument, which skips the route, the menu-core delegate and the UI table), and the release's route
// is skipped, unless a UI context for that pad's player is open (players/focus_context.h, registered here):
// then the press is dispatched inside it, into that player's own widgets.
//
// The game's menus are shared, so there the game's own rule for a second pad comes back (players/menu_pads.h,
// RE-pause-menu-any-pad.md, switch `pad-menus`): while a menu owns input (game_state::MenuOwnsInput, a shop too), a further
// pad's new presses are the game's own pad events and the stick aggregate takes every pad, so every pad drives the
// menu; outside it, a press that pressed only menu openers in the pad's copy (Start's PauseMenu, MapMenu,
// EnemyIntel) is given to the game as well, so the game's own poll opens the menu. Each key's repeats and release
// go where its press went. Game thread only.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::players::pad_buttons {

/// Replaces the input object's Tick and key handler slots and redirects the dispatcher's entry and the one
/// release-route call. False (logged) if a check fails; nothing is then changed.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// The pads with a player of their own beyond player 1: bit u = XInput user u (a seat's controller id) has its own
/// copy (0: the game's own state everywhere). Bit 0 is ignored. A pad newly in starts from nothing held; a pad that
/// stays keeps its copy.
void SetPads(std::uint32_t pads) noexcept;

/// Each further pad's copy for the length of one engine call of its player. Game thread; does not nest.
class ButtonScope
{
public:
    explicit ButtonScope(unsigned user) noexcept;
    ~ButtonScope();
    ButtonScope(const ButtonScope&)            = delete;
    ButtonScope& operator=(const ButtonScope&) = delete;

    bool Held() const noexcept { return user_ != 0; }

private:
    unsigned user_ = 0;
};

/// A player's own UI context, opened around their pad's key events so the game's route reaches their widgets:
/// `enter` returns true when it opened (the press is then routed), `leave` closes it. Null: no context.
struct UiContext
{
    bool (*enter)(unsigned user) noexcept = nullptr;
    void (*leave)(unsigned user) noexcept = nullptr;
};
void SetUiContext(UiContext context) noexcept;

}  // namespace lss::players::pad_buttons
