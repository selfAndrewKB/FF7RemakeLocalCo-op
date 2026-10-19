#include "players/unreal_call.h"

#include "platform/guarded.h"

#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>

namespace lss::players {

bool GuardedProcessEvent(RC::Unreal::UObject* target, RC::Unreal::UFunction* function, void* params) noexcept
{
    __try
    {
        target->ProcessEvent(function, params);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

}  // namespace lss::players
