#include "world/referenced_actor.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/entry_redirect.h"
#include "platform/game_thread.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/weak_object.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::world::referenced_actor {

namespace {

using ReadFn      = std::int64_t (*)(const std::int32_t* reference);
using InterfaceFn = void* (*)(void* object, void* interfaceClass);

// Set at install.
platform::EntryRedirect g_read;
InterfaceFn             g_interfaceAddress = nullptr;
std::uintptr_t          g_classAt          = 0;  // the game's cached class pointer (0 until its first read)

// Game thread (the game's own callers of the read run there; the log's records are kept only from it).
platform::GameThreadMark       g_thread;
std::uint32_t                  g_guarded = 0;  // reads answered "no" instead of the game's dereference
std::array<std::wstring, 8>    g_classes;       // the classes met, each logged once
std::size_t                    g_classCount = 0;
bool                           g_classesFull = false;  // more classes met than kept: said once

void ClassText(void* object, std::wstring* out) noexcept
try
{
    *out = static_cast<RC::Unreal::UObject*>(object)->GetClassPrivate()->GetName();
}
catch (...)
{
    out->clear();
}

/// The object's class name, or a placeholder (the object may be anything: read under a guard).
std::wstring ClassName(void* object) noexcept
{
    std::wstring text;
    if (object == nullptr || !platform::CallGuarded(&ClassText, object, &text) || text.empty())
    {
        return L"(unnamed)";
    }
    return text;
}

/// Says what was met, by the object (null: `what` says why there is none): Info once per class, Trace after (the read
/// runs every sweep tick while a reference stays bad). Off the game thread only the count moves.
void Guarded(void* object, const wchar_t* what) noexcept
try
{
    ++g_guarded;
    if (!g_thread.Here())
    {
        return;
    }
    if (object == nullptr)
    {
        platform::LogDebug(L"referenced actor: a character's reference {}; the game's read of it answered no instead of its crash "
                           L"({} so far)",
                           what, g_guarded);
        return;
    }
    const std::wstring name = ClassName(object);
    for (std::size_t i = 0; i < g_classCount; ++i)
    {
        if (g_classes[i] == name)
        {
            platform::LogTrace(L"referenced actor: a character's reference names {} again; answered no ({} so far)", name, g_guarded);
            return;
        }
    }
    if (g_classCount < g_classes.size())
    {
        g_classes[g_classCount++] = name;
    }
    else if (g_classesFull)
    {
        platform::LogTrace(L"referenced actor: a character's reference names {} (beyond the classes kept); answered no", name);
        return;
    }
    else
    {
        g_classesFull = true;
        platform::Log(L"referenced actor: more classes met than the reading keeps; further ones go to the Trace log");
    }
    platform::Log(L"referenced actor: a character's reference names {}, which is not an End actor; the game's read of it "
                  L"answered no instead of its crash",
                  name);
}
catch (...)
{
}

/// The game's read, entered with the character's reference block: the game's own tests first, its read only when
/// they pass.
std::int64_t Read(const std::int32_t* reference) noexcept
{
    const auto original = reinterpret_cast<ReadFn>(g_read.Original());
    std::int32_t pair[2]{};
    if (!platform::CopyGuarded(pair, reference, sizeof(pair)))
    {
        Guarded(nullptr, L"has no readable reference block");
        return 0;
    }
    void* object = players::weak_object::ObjectOf(pair);
    if (object == nullptr)
    {
        Guarded(nullptr, pair[1] == 0 ? L"names nothing" : L"names an object no longer there");
        return 0;
    }
    void* interfaceClass = nullptr;
    if (!platform::Read(reinterpret_cast<const void*>(g_classAt), interfaceClass) || interfaceClass == nullptr)
    {
        platform::LogDebug(L"referenced actor: the game has not made its interface class yet; its read runs unguarded this once");
        return original(reference);  // the game's first read makes its class; it runs as the game's
    }
    void* address = nullptr;
    if (!platform::CallGuarded(g_interfaceAddress, address, object, interfaceClass) || address == nullptr)
    {
        Guarded(object, nullptr);
        return 0;
    }
    return original(reference);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (g_read.IsInstalled())
    {
        return false;
    }
    if (!players::weak_object::Installed())
    {
        platform::Log(L"referenced actor: the weak object reader is not installed; the game's read runs unguarded");
        return false;
    }
    const auto read    = platform::FindFunction(image, facts::kReferencedActorRead);
    const auto cast    = platform::FindFunction(image, facts::kGetInterfaceAddress);
    const auto classAt = image.Rebase(facts::kEndActorInterfaceClass, sizeof(void*));
    if (!read || !cast || !classAt)
    {
        platform::Log(L"referenced actor: an engine check failed; the game's read runs unguarded");
        return false;
    }
    g_interfaceAddress = reinterpret_cast<InterfaceFn>(*cast);
    g_classAt          = *classAt;
    g_thread.Set();  // installed on the game thread, where the game's own callers of the read run
    if (!g_read.InstallAtSecondInstruction(*read, facts::kReferencedActorReadFirst, facts::kReferencedActorReadSecond,
                                           reinterpret_cast<std::uintptr_t>(&Read)))
    {
        platform::Log(L"referenced actor: the read's entry could not be redirected; it runs unguarded");
        return false;
    }
    control::Register({.name  = L"referenced-actor",
                       .shape = control::Shape::Text,
                       .help  = L"the game's reads through a character's referenced actor answered no instead of a crash, and the classes met",
                       .get   = [] {
                           std::wstring classes;
                           for (std::size_t i = 0; i < g_classCount; ++i)
                           {
                               classes += (i == 0 ? L"" : L", ") + g_classes[i];
                           }
                           return control::Value{std::format(L"{} guarded; classes: {}", g_guarded, classes.empty() ? L"none" : classes)};
                       }});
    platform::Log(L"referenced actor: the game's read through a character's referenced actor answers no where the actor is "
                  L"missing or not an End actor (the game would dereference null there)");
    return true;
}

void Remove() noexcept
{
    g_read.Restore();
}

}  // namespace lss::world::referenced_actor
