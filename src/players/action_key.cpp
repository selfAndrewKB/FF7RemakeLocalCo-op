#include "players/action_key.h"

#include "platform/guarded.h"

#include <array>
#include <cstring>

namespace lss::players::action_key {

bool Test(Getter getter, std::uintptr_t key, bool& out) noexcept
{
    alignas(16) std::array<std::uint8_t, 24> copy{};
    if (getter == nullptr || key == 0 || !platform::CopyGuarded(copy.data(), reinterpret_cast<const void*>(key), copy.size()))
    {
        return false;
    }
    void* controller = nullptr;
    std::memcpy(&controller, copy.data() + 16, sizeof(controller));
    if (controller != nullptr)
    {
        // The copy's reference, which the getter releases (the game's own ++ on the shared reference count).
        std::int32_t count = 0;
        if (!platform::Read(static_cast<std::uint8_t*>(controller) + 8, count))
        {
            return false;
        }
        ++count;
        if (!platform::WriteGuarded(static_cast<std::uint8_t*>(controller) + 8, &count, sizeof(count)))
        {
            return false;  // without the reference the getter would release one it never got
        }
    }
    bool result = false;
    if (!platform::CallGuarded(getter, result, static_cast<void*>(copy.data())))
    {
        return false;
    }
    out = result;
    return true;
}

}  // namespace lss::players::action_key
