#include "platform/guarded.h"

#include <cstring>

namespace lss::platform {

bool CopyGuarded(void* out, const void* from, std::size_t length) noexcept
{
    __try
    {
        std::memcpy(out, from, length);
        return true;
    }
    __except (GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool WriteGuarded(void* to, const void* from, std::size_t length) noexcept
{
    __try
    {
        std::memcpy(to, from, length);
        return true;
    }
    __except (GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

}  // namespace lss::platform
