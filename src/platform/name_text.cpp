#include "platform/name_text.h"

#include "platform/guarded.h"

#include <Unreal/NameTypes.hpp>

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

}  // namespace lss::platform
