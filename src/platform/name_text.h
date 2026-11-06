#pragma once

// The text of an engine name (FName), for log lines and readings: the lookup goes through UE4SS and is guarded, since a name
// read from memory the engine has let go can be anything.

#include <cstdint>
#include <string>

namespace lss::platform {

/// The text of the name whose 8 bytes are `bits`, as the engine keeps an FName (its index, then its number). Empty when the
/// engine cannot say: the lookup faulted or threw. Callers put their own word in its place.
std::wstring NameText(std::uint64_t bits) noexcept;

}  // namespace lss::platform
