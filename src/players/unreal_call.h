#pragma once

// A reflected engine call (UObject::ProcessEvent) under a structured-exception guard, shared by the modules that drive
// the game's reflected setters (players/session, hud/root_placement). Game thread only.

namespace RC::Unreal {
class UObject;
class UFunction;
}  // namespace RC::Unreal

namespace lss::players {

/// Calls `function` on `target` with `params` (its parameter frame). False if the engine raised an exception.
bool GuardedProcessEvent(RC::Unreal::UObject* target, RC::Unreal::UFunction* function, void* params) noexcept;

}  // namespace lss::players
