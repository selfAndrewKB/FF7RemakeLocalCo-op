#pragma once

// Summons for players 2 and 3 (research/RE-summon-seats.md). The party has one summon gauge; a command menu offers Summon
// for its own character when the gauge is full, that character has summon materia equipped and the battle allows it. The
// one per-menu part of that test reads the controller's own summon gauge widget, which the game makes and sets on player
// 1's controller only, so the Summon row never appeared in a seat's menu. The two readers (the gauge shown; a summon on
// the field, for its commands) are redirected at their entry: asked by a seat's controller, they answer with player 1's,
// the game's one copy of the party gauge's state. The rest is the game's own: the seat's menu, its character's materia
// and ATB, the cast, one summon at a time. Each seat's answer is logged at Debug when it changes. Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::summon_seats {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::battle::summon_seats
