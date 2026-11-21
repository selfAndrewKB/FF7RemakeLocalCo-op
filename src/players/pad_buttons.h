#pragma once

// Each pad's buttons for its own player (research/RE-input.md, RE-input-p2.md: the design, a written exception).
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
// The game's menus are shared, and so are its minigames and bike chases, played on one picture, so there the game's
// own rule for a second pad comes back (players/menu_pads.h, RE-pause-menu-any-pad.md, RE-minigame-input.md;
// switches `pad-menus`, `pad-minigames`): while a menu owns input (game_state::MenuOwnsInput, a shop too), or a
// minigame or a bike chase holds the whole screen (views/whole_screen.h), a further pad's new presses are the game's
// own pad events and the stick aggregate takes every pad, so every pad drives the menu or the game; outside it, a
// press that pressed only menu openers in the pad's copy (Start's PauseMenu, MapMenu, EnemyIntel) is given to the
// game as well, so the game's own poll opens the menu. Each key's repeats and release go where its press went.
// Logged at Info when the shared moment begins and ends, with the key events each pad gave the game during it.
//
// The keyboard and mouse are one more device (research/RE-kbm-routing.md): their events carry the Slate user 0, a key
// event a Windows key code (a pad button's carries none). A keyboard key goes to players/keyboard_keys first (Tab, the
// open window's keys); then, while the keyboard plays player 2 or 3 alone (KeyboardPlayer), its keys, the
// mouse's buttons (the input object's mouse handlers, taken here too) and the input tick's repeat of a held mouse
// button are that player's exactly as their pad's would be, each key's repeats and release where its press went
// (keyboard_route_rules). The wheel steps UI focus only: in that player's UI context, else nowhere. While the keyboard
// plays player 2 or 3 and views are split, the game's "keyboard used" flag is put back after each of its events, so the
// device class (the button art, the rumble's device) stays player 1's device's.
// Game thread only.

#include <array>
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

/// The player the keyboard and mouse press for: 1 or 2 (a seat's controller id) while they play player 2 or 3 alone and
/// that player has their own buttons, else 0 (player 1's: the game's own state). The one answer every keyboard route
/// uses (players/keyboard_route too). Game thread.
int KeyboardPlayer() noexcept;

/// The keyboard's digital stick (its movement and camera keys) as the stick scope of `user` (a pad slot) sees it, for
/// the player the keyboard plays: `live`, the field the input tick built, while that is player 1 or the game's input is
/// shared; while it is player 2 or 3, the stick built from the keyboard's direction bits, which were kept out of the
/// tick. Still for every other player.
std::array<float, 4> KeyboardStickFor(std::uint32_t user, const std::array<float, 4>& live) noexcept;

}  // namespace lss::players::pad_buttons
