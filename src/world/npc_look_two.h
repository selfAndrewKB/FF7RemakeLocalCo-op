#pragma once

// NPCs that keep watching the player watch player 2 too, player 1 first (research/RE-npc-reactions.md §10; the user,
// 2026-10-21: "the main guy I have in mind, still only seems to follow Cloud").
//
// Some NPCs (the old man in Sector 5) track the player continuously while near. That is not the Curiosity look
// (world/npc_senses) but the field AI's own look-at setting from the NPC's row: a target name, `Leader` for these,
// resolved once to the party leader (player 1's character), and a reaction distance that starts the look (and a body
// turn) and ends it. The mod, before the game's tick of each such look, picks the target the way the game's range
// test sees it: player 1 when in range, else player 2 when in range, else player 1; on a change it clears the game's
// request (the game's own clear) and writes the new target where the game's own resolve writes it, so the game's tick
// requests the look and turns the NPC. Only looks whose row names `Leader` and whose target is one of the players
// (or player 2's last character) are touched; anything else is the game's alone. Player 2 out: the look goes back
// to player 1. The tick runs on a worker thread: the players' weak pairs are published by the game thread.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::npc_look_two {

/// Checks the look's tick, its range test and the clear against the research and redirects the tick's call (logged).
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, each engine tick: player 1's character and player 2's character while in (null otherwise).
void Tick(void* memberOne, void* memberTwo) noexcept;

/// The switch (`npc-look on|off`, on at start): off turns every look the mod moved back to player 1.
void SetEnabled(bool on) noexcept;

/// The `world.npc-look` reading: the looks turned to player 2 and back.
std::wstring Reading() noexcept;

}  // namespace lss::world::npc_look_two
