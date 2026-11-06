#pragma once

// The live UUserWidgets, kept current without walking every UObject.
//
// A walk of GUObjectArray (UE4SS FindAllOf) costs ~20-25 ms in FF7R, so doing it every few frames
// stalls the game. Instead, UE4SS's
// StaticConstructObject post-callback records each UUserWidget as the engine constructs it; one
// walk at install (while the game loads) finds the widgets made before. Construction may run on loading threads,
// so new widgets are queued under a lock and taken on the game thread. Collected widgets are
// dropped when the list is read (pointer, GUObjectArray index and class checked, as elsewhere).

#include <cstdint>
#include <vector>

namespace RC::Unreal {
class UObject;
}

namespace lss::hud::user_widgets {

struct Widget
{
    RC::Unreal::UObject* object = nullptr;
    std::int32_t         index  = -1;
    const void*          cls    = nullptr;
};

/// Registers the construction callback. False (logged) if UMG's UserWidget class is not found.
bool Install() noexcept;
void Remove() noexcept;

/// Game thread: the live widgets (sorted by GUObjectArray index).
const std::vector<Widget>& Live() noexcept;

/// Whether `widget` is still the object it was when recorded.
bool Alive(const Widget& widget) noexcept;

}  // namespace lss::hud::user_widgets
