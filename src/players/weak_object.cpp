#include "players/weak_object.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"

#include <cstddef>

namespace lss::players::weak_object {

namespace {

using platform::Read;

using AllocateSerialFn = std::int32_t (*)(void* objectArray, std::int32_t index);

constexpr std::uint32_t kItemBytes       = 0x18;  // FUObjectItem: Object +0, Flags +8, SerialNumber +0x10
constexpr std::uint32_t kItemFlags       = 0x08;
constexpr std::uint32_t kItemSerial      = 0x10;
constexpr std::int32_t  kUnreachableBits = 0x30000000;  // PendingKill | Unreachable: the pointer is not to be used

// Set once at install, then only read.
std::uint8_t**   g_objects        = nullptr;
std::int32_t*    g_objectCount    = nullptr;
void*            g_objectArray    = nullptr;
AllocateSerialFn g_allocateSerial = nullptr;

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto objects  = image.Rebase(facts::kObjectArrayObjects, sizeof(void*));
    const auto count    = image.Rebase(facts::kObjectArrayCount, sizeof(std::int32_t));
    const auto array    = image.Rebase(facts::kObjectArray);
    const auto allocate = platform::FindFunction(image, facts::kAllocateSerialNumber);
    if (!objects || !count || !array || !allocate)
    {
        platform::Log(L"weak objects: the object array's globals are not what the research read; no object can be kept "
                      L"between ticks");
        return false;
    }
    g_objects        = reinterpret_cast<std::uint8_t**>(*objects);
    g_objectCount    = reinterpret_cast<std::int32_t*>(*count);
    g_objectArray    = reinterpret_cast<void*>(*array);
    g_allocateSerial = reinterpret_cast<AllocateSerialFn>(*allocate);
    return true;
}

bool Installed() noexcept
{
    return g_allocateSerial != nullptr;
}

Weak MakeWeak(void* object) noexcept
{
    Weak         weak;
    std::int32_t index  = -1;
    std::int32_t serial = 0;
    if (object != nullptr && g_allocateSerial != nullptr &&
        Read(static_cast<std::uint8_t*>(object) + facts::kObjectIndex, index) && index >= 0 &&
        platform::CallGuarded(g_allocateSerial, serial, g_objectArray, index) && serial != 0)
    {
        weak = {index, serial};
    }
    return weak;
}

void* Resolve(const Weak& weak) noexcept
{
    std::uint8_t* objects = nullptr;
    std::int32_t  count   = 0;
    if (!weak.Set() || g_objects == nullptr || !Read(g_objects, objects) || !Read(g_objectCount, count) ||
        weak.index >= count || objects == nullptr)
    {
        return nullptr;
    }
    const std::uint8_t* item   = objects + static_cast<std::size_t>(weak.index) * kItemBytes;
    void*               object = nullptr;
    std::int32_t        flags = 0, serial = 0;
    if (!Read(item, object) || !Read(item + kItemFlags, flags) || !Read(item + kItemSerial, serial) ||
        serial != weak.serial || (flags & kUnreachableBits) != 0)
    {
        return nullptr;
    }
    return object;
}

bool WeakPairOf(void* object, std::int32_t (&pair)[2]) noexcept
{
    const Weak weak = MakeWeak(object);
    pair[0]         = weak.index;
    pair[1]         = weak.serial;
    return weak.Set();
}

void* ObjectOf(const std::int32_t (&pair)[2]) noexcept
{
    return Resolve(Weak{pair[0], pair[1]});
}

}  // namespace lss::players::weak_object
