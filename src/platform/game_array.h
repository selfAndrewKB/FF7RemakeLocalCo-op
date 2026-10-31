#pragma once

// An FString or TArray as the engine lays them out (Data, Num, Max), and a source FString over the mod's own text for the
// game's string copies to read. Pure.
//
// The game's copies (FString::operator=, EndText_ResolveTextIdOrLiteral) read only a source's Data and Num, so a source
// over the mod's text needs no allocation of the game's; Num counts the terminating zero, and an empty text is the
// game's empty string (no Data). The text must outlive the copy.

#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <string>

namespace lss::platform {

struct GameArray
{
    void*        data = nullptr;
    std::int32_t num  = 0;
    std::int32_t max  = 0;
};
static_assert(sizeof(GameArray) == 16);

/// A source FString over the `length` characters at `text` (zero-terminated there).
inline GameArray SourceString(const wchar_t* text, std::size_t length) noexcept
{
    return length == 0 ? GameArray{} : GameArray{const_cast<wchar_t*>(text), static_cast<std::int32_t>(length + 1), 0};
}
inline GameArray SourceString(const wchar_t* text) noexcept
{
    return SourceString(text, std::wcslen(text));
}
inline GameArray SourceString(const std::wstring& text) noexcept
{
    return SourceString(text.c_str(), text.size());
}
GameArray SourceString(std::wstring&&) = delete;  // a temporary's text is gone before the game copies it

}  // namespace lss::platform
