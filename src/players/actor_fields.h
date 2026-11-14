#pragma once

// The fields of the game's actors that several modules read, one reader each: a character's components, mesh, player
// slot and tags, an actor's root component, a component's owner, a controller's camera manager and leader. Each is one
// guarded read (null for a null object or a faulting read), as the modules made before; any thread may call them, and
// what they return belongs to the thread that owns the object.

#include "engine_facts.h"
#include "platform/guarded.h"

#include <cstdint>

namespace lss::players::actor_fields {

/// A character's battle component, or null.
inline std::uint8_t* BattleComponent(const void* character) noexcept
{
    return static_cast<std::uint8_t*>(platform::ReadPointer(character, facts::kCharacterBattleComponent));
}

/// A character's field action component, or null.
inline std::uint8_t* FieldAction(const void* character) noexcept
{
    return static_cast<std::uint8_t*>(platform::ReadPointer(character, facts::kCharacterFieldAction));
}

/// A character's battle AI controller, or null.
inline void* BattleAi(const void* character) noexcept
{
    return platform::ReadPointer(character, facts::kCharacterBattleAI);
}

/// A character's skeletal mesh component, or null.
inline std::uint8_t* Mesh(const void* character) noexcept
{
    return static_cast<std::uint8_t*>(platform::ReadPointer(character, facts::kCharacterMesh));
}

/// The player slot a character is held by, or null while its AI runs it.
inline void* PlayerSlot(const void* character) noexcept
{
    return platform::ReadPointer(character, facts::kCharacterPlayerSlot);
}

/// An actor's tag array's data (FName entries), or null.
inline std::uint8_t* Tags(const void* actor) noexcept
{
    return static_cast<std::uint8_t*>(platform::ReadPointer(actor, facts::kActorTags));
}

/// An actor's root component, or null.
inline std::uint8_t* RootComponent(const void* actor) noexcept
{
    return static_cast<std::uint8_t*>(platform::ReadPointer(actor, facts::kActorRootComponent));
}

/// The actor that owns a component, or null.
inline void* ComponentOwner(const void* component) noexcept
{
    return platform::ReadPointer(component, facts::kComponentOwner);
}

/// The field action component a field action state belongs to, or null.
inline void* ActionStateComponent(const void* state) noexcept
{
    return platform::ReadPointer(state, facts::kStateComponent);
}

/// A player controller's camera manager, or null.
inline void* CameraManager(const void* controller) noexcept
{
    return platform::ReadPointer(controller, facts::kControllerCameraManager);
}

/// The character a controller leads with, or null.
inline void* LeaderCharacter(const void* controller) noexcept
{
    return platform::ReadPointer(controller, facts::kControllerLeaderCharacter);
}

/// A user widget's widget tree (its named children are found in it), or null.
inline void* WidgetTree(const void* userWidget) noexcept
{
    return platform::ReadPointer(userWidget, facts::kUserWidgetTree);
}

}  // namespace lss::players::actor_fields
