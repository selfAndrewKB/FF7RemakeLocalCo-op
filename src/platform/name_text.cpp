#include "platform/name_text.h"

#include "platform/guarded.h"

#include <Unreal/NameTypes.hpp>

#include <format>

namespace lss::platform {

namespace {

/// The lookup itself, apart from its guard: a guarded frame may hold no object that needs unwinding.
void Lookup(std::int64_t bits, std::wstring* out) noexcept
try
{
    *out = RC::Unreal::FName(bits).ToString();
}
catch (...)
{
    out->clear();
}

/// The reverse lookup, apart from its guard.
void FindBits(const wchar_t* text, std::int64_t* out) noexcept
try
{
    const RC::Unreal::FName name(text, RC::Unreal::FNAME_Find);
    *out = (static_cast<std::int64_t>(name.GetNumber()) << 32) | static_cast<std::int64_t>(name.GetComparisonIndex().ToUnstableInt());
}
catch (...)
{
    *out = 0;
}

}  // namespace

std::wstring NameText(std::uint64_t bits) noexcept
{
    std::wstring text;
    if (!CallGuarded(&Lookup, static_cast<std::int64_t>(bits), &text))
    {
        return {};
    }
    return text;
}

std::wstring NameOrBits(std::uint64_t bits)
{
    if (bits == 0)
    {
        return L"None";
    }
    std::wstring text = NameText(bits);
    return text.empty() ? std::format(L"name#{:x}", bits) : text;
}

std::wstring NameList(const std::vector<std::int64_t>& names)
{
    std::wstring out;
    for (const std::int64_t name : names)
    {
        out += (out.empty() ? L"" : L", ") + NameOrBits(name);
    }
    return out.empty() ? L"none" : out;
}

std::uint64_t FindName(const wchar_t* text) noexcept
{
    std::int64_t bits = 0;
    if (text == nullptr || *text == L'\0' || !CallGuarded(&FindBits, text, &bits))
    {
        return 0;
    }
    return static_cast<std::uint64_t>(bits);
}

}  // namespace lss::platform
