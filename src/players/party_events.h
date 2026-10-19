#pragma once

// The game's party funnels, heard by the session (RE-slot-member.md §1.5, §2.5, §3; Rebirth's player_two/party_events).
//   leader   PartyManager_SetPartyLeader and its toad twin: every path that makes a character player 1's (the battle
//            cycle and KO switch, the battle-start default leader, the battle-end and result restores, scripts, toad)
//            passes one of them before any party-manager write. The listener hears it first, so a further player can
//            give back a member the game is about to seat player 1 on.
//   stream   the formation rebuild at the end of every player stream (the one writer of the deployed formation;
//            story party changes, party applies): heard before and after. After it every member is typed PARTY
//            again, so a seated member's PLAYER type is re-asserted by the session.
//   event    the game's hand of the leader to its AI at an event's start (two of three call sites; the third crosses
//            a block) and its return at the event's end (one of two sites): a further player's member gets the same.
// The calls are redirected (platform/entry_redirect.h, platform/call_redirect.h); the game's code always runs. Game
// thread only: a call from another thread passes through unheard (counted).

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::players::party_events {

struct Listener
{
    /// Before the game makes `character` player 1's leader (null: a release).
    void (*leaderChanging)(void* context, void* character) noexcept = nullptr;
    /// Before and after the game rebuilds the deployed formation (the statuses are final before).
    void (*streamFinishing)(void* context) noexcept = nullptr;
    void (*streamFinished)(void* context) noexcept  = nullptr;
    /// After the game handed its leader to its AI (an event begins) and after it took it back.
    void (*leaderHandedToAi)(void* context) noexcept = nullptr;
    void (*leaderReturned)(void* context) noexcept   = nullptr;
    void* context = nullptr;
};

/// Resolves the functions and places the call-site redirects. False (logged) if a check fails; the two entry
/// redirects are placed by Tick (from the game thread, outside the functions).
bool Install(const platform::GameImage& image, const Listener& listener) noexcept;
void Remove() noexcept;

/// Game thread, each engine tick: places the entry redirects once.
void Tick() noexcept;

}  // namespace lss::players::party_events
