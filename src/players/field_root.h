#pragma once

// The field root and what the modules read off it, in one place (research/RE-lifecycle-world.md,
// RE-script-release.md): the game's field root object (facts::kEndGameRootGlobal), its state manager (the field
// control lock's mask and counter live there, as do the on/off table and the owner of each id), the root's object
// that holds the character registry and the field battle state, the battle system global (null outside battle), the
// UI system's slot for the field command controller, the party manager and the game's own party leader getter. One
// header for them, so no module rebases the same globals and walks the same pointers itself. Rebased and verified
// once at install, which runs right after weak_object's and before every module that reads them; a module that needs them checks Installed() at its own install. Every read is
// guarded and answers null or 0 while the game has no root (the boot, the title's first frames) or no battle; a
// module keeps gating its own readers and ticks on its own install, as before (this module says nothing about
// those). The globals are the game's own fixed addresses, so any thread may call these; what they point at belongs
// to the game thread, as before.

#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::players::field_root {

/// Rebases the globals and finds the leader getter. False (logged) when one is outside the image or not as the
/// research read it; the readers then answer null and the modules that need them at install refuse theirs.
bool Install(const platform::GameImage& image) noexcept;
bool Installed() noexcept;

/// The field root object, or null.
std::uint8_t* Root() noexcept;

/// The root's state manager (facts::kRootStateManager), or null.
std::uint8_t* StateManager() noexcept;

/// The field control lock's mask and counter on the state manager; false (outputs untouched) when unread.
bool ControlLock(std::uint32_t& mask, std::uint32_t& counter) noexcept;
/// The mask alone (one read): 0 when free or unread.
std::uint32_t ControlLockMask() noexcept;

/// The on/off table on the state manager (facts::kOnOffTable: Square Enix's EndFieldOnOffTable, one reference count
/// per id): whether `id` is on; false for an id past the table or while unread.
bool OnOff(std::uint32_t id) noexcept;
/// Who turned `id` on (facts::kOnOffOwners: the last non-zero reason a SetOn passed, an act's name for a data-set id);
/// 0 while off, unread or past the table.
std::uint64_t OnOffOwner(std::uint32_t id) noexcept;
/// An owner's text for the log ("none" for 0).
std::wstring OnOffOwnerText(std::uint64_t owner) noexcept;

/// The root's object holding the character registry and the field battle state (facts::kEndGameRootCharaOwner),
/// or null.
std::uint8_t* CharaOwner() noexcept;

/// The field battle state on that object says a battle is on.
bool InBattle() noexcept;

/// The battle system (`sys`), null outside battle or unread.
void* BattleSystem() noexcept;

/// The field command controller in the UI system's slot (facts::kCommandCtrlSlotGlobal): player 1's outside every scope,
/// a seat's own inside that seat's scopes (players/scopes.h puts it there); null when unread or empty.
std::uint8_t* CommandController() noexcept;
/// The slot's live address, for the scopes that hold a seat's controller in it; 0 before install.
std::uintptr_t CommandControllerSlot() noexcept;

/// PartyManager_GetPartyLeader on the party manager with the game's selector (facts::kLeaderCurrent, kLeaderDefault
/// or a Blueprint act's own), guarded; null without a manager, when the call raises, or when the game answers none.
/// (The modules that redirect the game's own calls of the getter keep their own FindFunction: the redirect needs
/// the expected target. The party manager global itself is still rebased by the modules that read other things
/// off it, scopes::PartyManagerAddress among them: a later dedup, not B1's.)
void* PartyLeader(std::uint8_t selector) noexcept;

}  // namespace lss::players::field_root
