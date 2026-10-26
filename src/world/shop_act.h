#pragma once

// A shop a further player opens is that player's: its camera zooms on the opener's view, the shopkeeper and the
// opener's character face each other, and the opener's pad drives it (research/RE-shop-two.md §8; the user,
// 2026-10-26: shopkeepers for every player, "shops should pause the entire game"; Rebirth's player_two/shop_talk and
// shop camera skip were the model).
//
// A shopkeeper is an NPC (world/field_targets admits NPCs for further players). The press only fires the NPC's talk
// trigger; the NPC's next state then queues, on later frames and outside any player's run, a greeting and a show-menu
// act (type 43, kind 1), which opens the shop on its first frame and, through its own Begin, Update, OnFinish and end,
// resolves "the leader" (LEADER_) for the main character, its camera preset and its facing holds, and sets the
// shop camera on the primary camera manager. Run as the game's, all of that is player 1's.
//   - The act's creation (one call, which runs its Begin) is redirected: when its channel's object is an NPC a further
//     player pressed (field_targets::ClaimTalkPress) and the act's row is kind 1 (a shop), it runs in that player's
//     Leader and Primary scopes, and the act is noted as the player's; its Update, OnFinish and destructor (vtable
//     entries of this act alone) run in the same scopes. So "the leader" is the opener's character and the camera the
//     opener's manager, through the game's own code. The act's tick (which takes and releases the field lock) is not
//     scoped: the lock and its hand stay the game's, so the whole game pauses as for player 1's shop.
//   - While a player's shop act lives, views/scene_camera_two leaves that player's camera manager alone (OwnsScene), so
//     the opener's view keeps its own shop zoom; player 1's view stays as it is.
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

/// A shop act of this seat's player is alive (its camera on the seat's manager).
bool OwnsScene(int seat) noexcept;

}  // namespace lss::world::shop_act
