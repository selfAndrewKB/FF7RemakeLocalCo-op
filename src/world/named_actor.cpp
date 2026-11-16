#include "world/named_actor.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/weak_object.h"

namespace lss::world::named_actor {

namespace {

using FindActorFn = void* (*)(std::uint64_t level, std::uint64_t name);

FindActorFn          g_findActor = nullptr;
const std::uint64_t* g_level     = nullptr;  // the level FName the game's own name lookups pass

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto findActor = platform::FindFunction(image, facts::kFindActorByName);
    const auto level     = image.Rebase(facts::kNamedActorLevel, 8);
    if (!findActor || !level)
    {
        platform::Log(L"named actor: the character registry's lookup by name is not what the research read; nothing finds a "
                      L"character or object by its name");
        return false;
    }
    g_findActor = reinterpret_cast<FindActorFn>(*findActor);
    g_level     = reinterpret_cast<const std::uint64_t*>(*level);
    return true;
}

bool Installed() noexcept
{
    return g_findActor != nullptr;
}

void* InLevel(std::uint64_t level, std::uint64_t name) noexcept
{
    void* actor = nullptr;
    return g_findActor != nullptr && platform::CallGuarded(g_findActor, actor, level, name) ? actor : nullptr;
}

void* Find(std::uint64_t name) noexcept
{
    std::uint64_t level = 0;
    if (g_level == nullptr || !platform::Read(g_level, level))
    {
        return nullptr;
    }
    void* const actor = InLevel(level, name);
    return actor != nullptr && players::weak_object::Live(actor) ? actor : nullptr;
}

}  // namespace lss::world::named_actor
