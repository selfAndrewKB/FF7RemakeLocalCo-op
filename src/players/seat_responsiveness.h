#pragma once

// Players 2 and 3's camera responsiveness (research/RE-camera-sensitivity-per-player.md). The game's Responsiveness
// option (1 to 5) is one byte of its option block, which the controller's input step reads every frame through the
// game's own getter, and the camera turns by that level times the controller's rate. A seat's input step (the
// ProcessPlayerInput replacement in players/seat_camera.cpp) runs inside Scope(seat), which holds the seat's own level in
// that byte for the call and puts player 1's back after (platform/byte_scope.h, Hold: a game write inside is undone
// and counted). Player 1's option, its row and every other reader stay the game's. The levels are the settings file's
// responsiveness options (platform/settings.h), set by their rows in the game's Options screen (menu/coop_page) and the
// console; a change applies from the seat's next input step. Logged at Info when a level is set, a warning once if
// the game wrote the byte inside a seat's step. Game thread.

#include <string>

namespace lss::players::seat_responsiveness {

/// Reads the levels from the settings file and registers their console commands. Always true.
bool Install() noexcept;

/// The seat's level in the game's Responsiveness byte for the scope's life, player 1's put back after. Holds nothing
/// for a seat without an option of its own or while the game has no option block yet.
class Scope
{
public:
    explicit Scope(int seat) noexcept;
    ~Scope();
    Scope(const Scope&)            = delete;
    Scope& operator=(const Scope&) = delete;

private:
    int  seat_ = 0;
    bool held_ = false;
};

/// For the console: player 1's byte, players 2 and 3's levels, the steps held and the game writes undone.
std::wstring Reading();

}  // namespace lss::players::seat_responsiveness
