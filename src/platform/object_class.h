#pragma once

// An engine class by its path and an object's class test, through UE4SS: the lookup (StaticFindObject) runs until the
// class is found and is then kept per path; the test (UObject::IsA) is guarded, since an object read from game memory may
// already be gone. Game thread.

namespace lss::platform {

/// The class at `path` (e.g. L"/Script/EndGame.EndFieldActionActorDoor"), or null while the engine has none by that name.
/// `path` must be a string that lives as long as the mod (a facts constant): it keys the kept lookups.
void* FindClass(const wchar_t* path) noexcept;

/// `object` is of class `cls` or a child of it; false for a null object or class, or when the test faults.
bool IsA(void* object, void* cls) noexcept;

}  // namespace lss::platform
