#pragma once

// A shop a further player opens is that player's: its camera zooms on the opener's view, the shopkeeper and the
// opener's character face each other, and the opener's pad drives it (research/RE-shop-two.md).
// Shopkeepers serve every player, and a shop pauses the
// whole game.
//
// A shopkeeper is an NPC (world/field_targets admits NPCs for further players). The press only fires the NPC's talk
// trigger; the NPC's next state then queues, on later frames and outside any player's run, a greeting and a show-menu
// act (type 43, kind 1), which opens the shop on its first frame and, through its own Begin, Update, OnFinish and end,
// resolves "the leader" (LEADER_) for the main character, its camera preset and its facing holds, and sets the
// shop camera on the primary camera manager. Run as the game's, all of that is player 1's.
//   - A vending machine is the same shop: a further player's press runs the member's own vending action and the level
//     then queues the act on its own script channel, so a kind-1 act no NPC press names is the vending press's
//     (field_targets::ClaimVendingPress, matched by time).
//   - The act's creation (one call, which runs its Begin) is redirected: when its channel's object is an NPC a further
//     player pressed (field_targets::ClaimTalkPress), or a vending press waits, and the act's row is kind 1 (a shop), it runs in that player's
//     Leader scope, and on a monitor row in its Primary scope too, and the act is noted as the player's with those
//     scopes; its Update, OnFinish and destructor (vtable entries of this act alone) run in the same scopes. So "the
//     leader" is the opener's character through the game's own code, and the camera: on a row the opener's manager
//     (the opener's view zooms on the shop, the others keep theirs); in one window the game's own primary manager,
//     player 1's, which the engine's single view shows (views/whole_screen.h: the shop takes the whole screen),
//     framing the opener's character with the shopkeeper as single player does
//     [ASSUMPTION until played]. The
//     act's tick (which takes and releases the field lock) is not scoped: the lock and its hand stay the game's, so
//     the whole game pauses as for player 1's shop.
//   - While a player's shop act lives with its camera on the player's own manager (a row), views/scene_camera leaves
//     that manager alone (OwnsScene), so the opener's view keeps its own shop zoom; player 1's view stays as it is.
//   - The shop menu is one, global: while it is up (EndUISystem[44]'s "shop up" byte), players/game_state counts it
//     as menu time, so every pad drives it as in a pause (players/menu_pads.h), the opener's included.
// A shop whose scope cannot open at its start runs as the game's throughout; one whose player leaves or changes
// character before its act ends runs the rest as the game's (a warning): the opener's hold may then stay on its
// character until the next field action. A note whose act was freed without its destructor is forgotten (the act's
// vtable is checked). Logged: install, each further player's shop opened and ended (Info); each show-menu act with
// its kind and press (Debug); scopes refused (Warning); reading `shop-act`. Game thread.

#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::shop_act {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Where the next shop a further player opens puts its camera: one window (true) the game's primary manager, player
/// 1's, under the engine's single view; else (a monitor row, columns held) the opener's own manager. From the entry
/// point each tick (views/column_source.h's columns held); a shop already open keeps the scopes it started with.
void SetOneWindow(bool oneWindow) noexcept;

/// A shop act of this seat's player is alive with its camera on the seat's own manager (a row's shop).
bool OwnsScene(int seat) noexcept;

}  // namespace lss::world::shop_act
