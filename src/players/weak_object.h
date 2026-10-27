#pragma once

// UE4 weak object pointers as the game stores them: {the object's index in GUObjectArray, its serial number}. A pair
// names an object across frames and resolves to null once the engine destroyed or replaced it (the engine's own
// validity rule: the same serial, not unreachable or pending kill). Moved out of players/seat_camera (plan/
// seats.md, the close of step 3): every module that keeps a game object between ticks uses these, and the camera
// module's satellites no longer include the camera module for them. Game thread.

#include <cstdint>
#include <type_traits>

namespace lss::platform {
class GameImage;
}

namespace lss::players::weak_object {

/// Finds the object array's globals and the serial allocator (logged). False if a check failed (no pair can then be
/// made; every pair resolves to null).
bool Install(const platform::GameImage& image) noexcept;
/// Install succeeded: a module that keeps game objects between ticks may install.
bool Installed() noexcept;

/// A weak pointer pair {index, serial}.
struct Weak
{
    std::int32_t index  = -1;
    std::int32_t serial = 0;

    bool Set() const noexcept { return index >= 0 && serial != 0; }
    friend bool operator==(const Weak&, const Weak&) = default;
};
static_assert(sizeof(Weak) == 8 && std::is_trivially_copyable_v<Weak>, "written raw into the game's weak fields");

/// A pair for a live object, made the way the game makes one (the serial allocated if the object has none yet).
/// Unset if the object cannot be read.
Weak MakeWeak(void* object) noexcept;

/// The object a pair names, if it is still that object (same serial) and reachable; else nullptr.
void* Resolve(const Weak& weak) noexcept;

/// Whether an object lives at `address` now: the object array's item at the index read there holds exactly this
/// address and is neither unreachable nor pending kill. Writes nothing (MakeWeak may allocate a serial), so it may be
/// asked of an address that could have been freed, before MakeWeak is.
bool Live(const void* address) noexcept;

/// The same two, on the raw {index, serial} array other modules keep. False / nullptr as above.
bool  WeakPairOf(void* object, std::int32_t (&pair)[2]) noexcept;
void* ObjectOf(const std::int32_t (&pair)[2]) noexcept;

}  // namespace lss::players::weak_object
