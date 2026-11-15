#pragma once

// The text of an engine name (FName), for log lines and readings: the lookup goes through UE4SS and is guarded, since a name
// read from memory the engine has let go can be anything.

#include <cstdint>
#include <string>
#include <vector>

namespace lss::platform {

/// The text of the name whose 8 bytes are `bits`, as the engine keeps an FName (its index, then its number). Empty when the
/// engine cannot say: the lookup faulted or threw. Callers put their own word in its place.
std::wstring NameText(std::uint64_t bits) noexcept;

/// A name for a log line or a reading: "None" for no name, its text, or its bits ("name#...") when the engine cannot say.
std::wstring NameOrBits(std::uint64_t bits);

/// The same for a name kept as a signed 8-byte value (the data rows' arrays).
inline std::wstring NameOrBits(std::int64_t bits)
{
    return NameOrBits(static_cast<std::uint64_t>(bits));
}

/// Names for a log line, comma-separated (NameOrBits each); "none" for an empty list. May throw (allocation).
std::wstring NameList(const std::vector<std::int64_t>& names);

/// The 8 bytes of the engine name whose text is `text`, if the engine already has that name; 0 when it has none (never
/// adds one) or the lookup faulted or threw.
std::uint64_t FindName(const wchar_t* text) noexcept;

}  // namespace lss::platform
