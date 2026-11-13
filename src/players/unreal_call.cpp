#include "players/unreal_call.h"

#include "platform/guarded.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

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

RC::Unreal::UObject* ClassDefaultOf(const wchar_t* classPath) noexcept
try
{
    auto* type = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(nullptr, nullptr, classPath);
    return type == nullptr ? nullptr : static_cast<RC::Unreal::UObject*>(type->GetClassDefaultObject());
}
catch (...)
{
    return nullptr;
}

}  // namespace lss::players
