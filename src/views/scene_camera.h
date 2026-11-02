#pragma once

// The scene camera on every further player's view: a cutscene, a talk scene or an event camera plays on each further
// player's view as it does on player 1's (research/RE-cutscene-camera-two.md, pass S-RMK-CUTCAM;
// RE-cutscene-scene-state.md; RE-camera.md §2.2-2.4). Rebirth collapsed to player 1's single view for scenes (its
// whole_screen module); here the scene camera is given to each further player's camera manager instead, the user's
// call of 2026-10-21 (in one window the views now collapse for scenes too, views/whole_screen; on a monitor row this
// is what each further column shows).
//
// Each camera manager shows the lowest active of its 25 pose slots and blends between its own slots. A scene writes
// the primary manager's slots only (every writer resolves the primary controller's manager): the cutscene pose into
// slot 8 each frame, a talk camera into 14, an event camera into 18, the start and end blends as on-switches with a
// 60-byte record in slots 2-7, 9-13, 16, 17 (seeded with the source pose: the field camera's slot 24 for 6, 12, 16).
// A further player's manager, fed by that player's own field camera operator, keeps its slot 24 through a scene.
// Each seat has its own copy record.
//
// The lever, the game's own setters on each further player's manager, applied right before its UpdateCamera each
// frame while a scene holds the players (the cutscene manager playing, or the game's field control lock):
//   - the pose slots (8, 14, 18 and the other non-blend slots) are copied from player 1's manager every frame, the
//     active byte included (the same pose for every view: the writers read no player);
//   - a blend slot is set once, when it turns on for player 1: its record, and its seed pose; a blend from the
//     field camera is seeded with the seat's own slot 24, so the blend starts from that player's camera; the
//     manager's own UpdateCamera then runs the blend with its own length and ends it (the end blend lands on the
//     player's own field camera for free: the "to field" target is always slot 24 of the manager that blends);
//   - a camera cut (player 1's manager's cut bit, set by the cut instance on the first controller's manager only) is
//     raised on the seat's manager through the game's primary-scoped cut, so the anti-aliasing history resets there
//     too;
//   - the cutscene's end reset (EV/MV: every slot off, 24 on, the view target the own main camera) runs on the
//     seat's manager as well, by a redirect of the one call the cut instance makes;
//   - when the scene releases the players, the copied pose slots are switched off on the seat's manager, and a blend
//     still running there finishes by itself.
// Not covered (documented): the field operator's own return (flavor 6 seeded with the cut's end pose, the re-seat
// behind the leader) runs for player 1's operator only; a further player's camera returns through the end blend to
// wherever its operator stands. Sequencer camera shakes, rumble: player 1's.
// A mirror player's manager (views/mirror_player, v0.42.0; up to two mirrors since the seats plan's step 5) takes the
// same copy for EVERY slot, the field camera's 24 included, every frame, so its view is player 1's: a blend seeded as
// player 1's was (player 1's slot at the switch-on), the cut raised, the end reset run on it too. One source for
// every copy; a mirror needs no scene to run. Each mirror has its own copy record, by the mirror module's slot. A seat
// whose column shows player 1's picture (views/column_source.h Source::PlayerOne, SetSource) takes that every-slot
// copy too, scene or not, whatever the `scene-camera` switch (that switch is the scene on further views); the two
// copies are one loop (CopySlots), the slot set and the blend seed its parameters.
// Readings `scene.*`, `camera-slots.*`, `scene-camera.state`; the switch `scene-camera on|off` (on). Game thread.

#include "players/field_hold.h"
#include "players/seats.h"

#include <array>
#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::views::scene_camera {

/// Rebases the globals, finds the game's slot setters, the reset and the cutscene manager's playing test, redirects
/// the cutscene end's reset call, and registers the readings and the switch. False (logged) if a check fails: the
/// readings may still work, the mirror is stood aside.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each seat's camera manager while its player is in (null when out), by seat; [0] unused.
using SeatManagers = std::array<void*, players::scopes::kMaxPlayers>;

/// Once per engine tick with player 1's camera manager and each seat's: reads and logs what changed; a seat that
/// left has its copy forgotten.
void Tick(void* managerOne, const SeatManagers& seatManagers) noexcept;

/// Right before a camera manager's UpdateCamera. Player 1's (`seat` 0): its cut bit is noted for this frame. A
/// seat's (`manager` its own): the scene slots are mirrored onto it (or every slot, while its column shows player
/// 1's picture); the caller holds the seat's primary scope (the game's primary-scoped setters then reach the seat's
/// manager).
void BeforeUpdateCamera(void* managerOne, int seat, void* manager) noexcept;

/// Where the seat's column's view comes from this tick (views/column_source.h, from mod.cpp): `playerOne` true gives
/// the seat's manager the every-slot copy of player 1's, as a mirror's [ASSUMPTION, not played: a seat's own operator,
/// lock camera and sequence cameras write its slots too, and a slot active on the seat alone is not switched off by
/// the copy, so the picture may differ from a mirror's until the play check]; false its own view (the scene slots
/// while a scene holds). A change back to its own releases the copied slots on `manager`, the seat's camera manager
/// now (null when the seat is out: nothing is touched; its own field camera slot stays on either way).
void SetSource(int seat, bool playerOne, void* manager) noexcept;

/// Whether the seat's view shows player 1's camera now: a scene's slots copied onto its manager and on there, or its
/// column on player 1's picture. That view then shows player 1's world as well (world/env_sets.h). A seat's own shop
/// camera, its own field camera under a lock, or the `scene-camera` switch off: false. Game thread; false for a seat
/// outside the built ones.
bool ShowsPlayerOne(int seat) noexcept;

/// The game's scene signals, as read.
struct Scene
{
    bool          valid     = false;  // the state manager was readable
    bool          manager   = false;  // the cutscene manager exists
    bool          playing   = false;  // its playing test (any scene)
    bool          skip      = false;  // its skip byte
    std::uint32_t lockMask  = 0;      // the field control lock mask
    std::uint32_t lockCount = 0;      // and counter (never raised in this build: kept as the game reads it)
    bool          skippable = false;  // on/off 12 ShowSkipPauseMenu
    bool          black     = false;  // on/off 39, 40 or 53
    bool          loading   = false;  // on/off 41
    std::uint32_t mover     = 0;      // the lock bits that are player 1's own crawl, squeeze or grappling hook

    bool operator==(const Scene&) const = default;

    /// Any scene or lock beyond player 1's own move: the whole screen's gate on the event camera (views/whole_screen.h,
    /// v0.111.0: slot 18 alone is no reason; the lock alone neither). Player 1's own crawl, squeeze or grappling hook
    /// locks only player 1 (as the session keeps the seats' members theirs through it): its bits hold no one else. A
    /// treasure or push-button lock still counts here: an event that cuts to its own shot collapses as before.
    bool HoldsAnyone() const noexcept { return playing || (lockMask & ~mover) != 0 || lockCount != 0; }
    /// The scene holds the further players: the copy's reason to run. A treasure or push-button lock freezes only the
    /// interacting character (players/field_hold.h InteractionLock; the members stay their players'), so its bits hold
    /// no one else either.
    bool Holds() const noexcept
    {
        return playing || (lockMask & ~mover & ~players::field_hold::kInteractionLocks) != 0 || lockCount != 0;
    }
};

/// The scene signals read now (a few guarded reads and the game's playing test; all false before Install): the
/// whole-screen read takes them before the session's tick, Tick reads them again after it.
Scene ReadSceneNow() noexcept;

/// The event camera's slot (18) active on player 1's manager at the last Tick: an event that cut to its own shot.
/// The whole screen then shows that shot alone (views/whole_screen.h), so a treasure or push-button lock takes the
/// further players' members with the game's leader while it stands (the user, 2026-10-27: an event that brings the
/// focus to something happening live may take them). False before Install.
bool EventShot() noexcept;

/// Whether a pose slot is active on a camera manager now (one guarded read; false for a null manager, a slot past
/// the table or an unreadable byte): the whole-screen read asks for the event camera's slot on player 1's manager.
bool SlotActive(const void* manager, std::uint32_t slot) noexcept;

/// The mirror players' copies (views/mirror_player), one per mirror slot 0..kMirrorSlots-1.
inline constexpr int kMirrorSlots = 2;
/// Mirror `slot`'s camera manager: every slot of player 1's manager `managerOne` is set on `manager` right before its
/// update, so its view is player 1's. All null: no mirror in that slot.
void SetMirror(int slot, void* controller, void* manager, void* managerOne) noexcept;
/// The mirror slot whose camera manager this is, or -1.
int MirrorSlotOf(const void* manager) noexcept;
void* MirrorController(int slot) noexcept;
/// Player 1's manager as the mirrors' source (null without a mirror): its update notes the cut for the mirrors.
void* MirrorSource() noexcept;
/// Right before mirror `slot`'s manager's UpdateCamera; the caller holds that mirror's primary scope.
void BeforeMirrorUpdate(int slot) noexcept;
/// The mirror copies' state and counts, for the `mirror-player.state` reading.
std::wstring MirrorPlayerText();

}  // namespace lss::views::scene_camera
