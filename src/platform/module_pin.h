#pragma once

// Keeps this DLL loaded until the process exits.
//
// Every change that leaves the game calling into our compiled code (a vtable entry, a redirected
// call) pins the DLL first. Unloading it would leave the engine calling freed memory the next
// frame, and a restore can fail (another mod may have hooked the same place on top of ours and now
// calls through to us), so "restore, then unload" is not safe; staying loaded is.

namespace lss::platform {

/// Pins the module this code lives in. Returns false if Windows refuses.
bool PinThisModule() noexcept;

}  // namespace lss::platform
