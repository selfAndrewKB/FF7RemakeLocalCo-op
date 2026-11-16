#pragma once

// The game's single slots answered for one further player (platform/byte_scope.h holds the mechanism).
//
// Square Enix's code keeps one copy of what is really per player. Each further player (2, and a later 3) gets
// a set of contexts, bound as their objects appear and cleared as they go:
//   Primary   PartyManager+0, the primary controller: the camera basis, the input step, the rumble channel.
//   Operator  the field root's camera operator weak pair: "the operator" the camera and menu code looks up.
//   Leader    PartyManager's leader weak pair and type: every attack, guard, dodge, ordinal and switch resolves it.
//   UiSlot    EndUISystem[19]: the command controller the UI code reads.
//   Focus     the input object's focus path, layers, cause and capture map (the player's own, kept between
//             calls), and the menu-core delegate muted: the UI route reaches this player's widgets only.
//   Latches   the UI filters' wait-tint and Dying latch bytes, the player's own (the game's counts do the sharing).
// A Scope opens the chosen contexts, in that order, and puts the game's values back in reverse; it is held only
// if every chosen context was bound and opened, so a caller that needs the leader never runs with player 1's.
// Game thread only.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::players::scopes {

/// Rebases the slots' addresses. False (logged) if one is outside the image.
bool Install(const platform::GameImage& image) noexcept;

inline constexpr int kMaxPlayers = 4;  // seats 1..3 are the further players

enum Which : unsigned
{
    Primary  = 1u << 0,
    Operator = 1u << 1,
    Leader   = 1u << 2,
    UiSlot   = 1u << 3,
    Focus    = 1u << 4,
    Latches  = 1u << 5,
    All      = 0x3F,
};

/// A player's controller (their primary), or null to unbind.
void BindController(int player, void* controller) noexcept;
/// Their field camera operator (its weak pair), or {-1, 0} to unbind.
void BindOperator(int player, const std::int32_t (&weak)[2]) noexcept;
/// The member they hold and its player type, or null to unbind.
void BindMember(int player, void* member, std::uint8_t type) noexcept;
/// Their command controller, or null to unbind (their focus copy and latches start empty again).
void BindCommandController(int player, void* controller) noexcept;
/// Everything of that player unbound.
void Unbind(int player) noexcept;

void*        Controller(int player) noexcept;
void*        Member(int player) noexcept;
std::uint8_t MemberType(int player) noexcept;  // 9 (NONE) when unbound
std::uintptr_t PartyManagerAddress() noexcept;  // the live party manager, 0 when unset
void*        CommandController(int player) noexcept;
/// The leader type the game held before this player's Leader scope opened (player 1's), while it is open.
std::uint8_t OtherLeaderType(int player) noexcept;
/// Player 1's command controller, inside a player's UiSlot scope as outside: there the slot holds that player's, so the
/// one it held before the scope opened. Null when none.
void* PlayerOneCommandController() noexcept;
/// This player's focus path holds at least one widget (their menu is up in their own context).
bool FocusPathSet(int player) noexcept;
/// The player one of whose `which` contexts is open now (a Scope of theirs is running the game's code), else 0.
int OpenPlayer(unsigned which) noexcept;

class Scope
{
public:
    Scope(int player, unsigned which) noexcept;
    ~Scope();
    Scope(const Scope&)            = delete;
    Scope& operator=(const Scope&) = delete;

    bool Held() const noexcept { return held_; }

private:
    int      player_ = 0;
    unsigned opened_ = 0;
    bool     held_   = false;
};

/// The game's own leader (player 1's) back in the slot for its lifetime, inside an open Scope of `player`'s that holds
/// Leader; nothing otherwise. For one game call that must not take the player's member as the party leader (the
/// action start's global locks: world/traversal_action.h). Game thread; the Scope's leader is written in again after.
class LeaderPause
{
public:
    explicit LeaderPause(int player) noexcept;
    ~LeaderPause();
    LeaderPause(const LeaderPause&)            = delete;
    LeaderPause& operator=(const LeaderPause&) = delete;

    bool Paused() const noexcept { return paused_; }

private:
    int  player_ = 0;
    bool paused_ = false;
};

}  // namespace lss::players::scopes
