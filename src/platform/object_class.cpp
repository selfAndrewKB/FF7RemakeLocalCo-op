#include "platform/object_class.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "platform/guarded.h"

#include <array>
#include <utility>

namespace lss::platform {

namespace {

using RC::Unreal::UClass;
using RC::Unreal::UObject;

/// The classes found so far, by their path's address (the paths are facts constants); a handful are asked for.
std::array<std::pair<const wchar_t*, void*>, 16> g_found{};

bool IsARaw(void* object, void* cls)
{
    return static_cast<UObject*>(object)->IsA(static_cast<UClass*>(cls));
}

}  // namespace

void* FindClass(const wchar_t* path) noexcept
{
    for (const auto& [known, cls] : g_found)
    {
        if (known == path)
        {
            return cls;
        }
    }
    void* cls = nullptr;
    try
    {
        cls = RC::Unreal::UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, path);
    }
    catch (...)
    {
        cls = nullptr;
    }
    if (cls != nullptr)
    {
        for (auto& entry : g_found)
        {
            if (entry.first == nullptr)
            {
                entry = {path, cls};
                break;
            }
        }
    }
    return cls;
}

bool IsA(void* object, void* cls) noexcept
{
    bool is = false;
    return object != nullptr && cls != nullptr && CallGuarded(&IsARaw, is, object, cls) && is;
}

}  // namespace lss::platform
