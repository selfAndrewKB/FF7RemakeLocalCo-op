#include "menu/game_ui.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/verified_code.h"
#include "players/weak_object.h"

#include <string>

namespace lss::menu::game_ui {

namespace {

namespace weak = players::weak_object;

// Set at install, then only read.
AssignFn       g_assign    = nullptr;
std::uintptr_t g_ui        = 0;  // EndUISystem's array of managers
std::uintptr_t g_topVtable = 0;

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto assign    = platform::FindFunction(image, facts::kFStringAssign);
    const auto ui        = image.Rebase(facts::kEndUISystemSlots, 8 * 64);
    const auto topVtable = image.Rebase(facts::kMainTopMenuVtable, 8);
    if (!assign || !ui || !topVtable || !weak::Installed())
    {
        return false;
    }
    g_assign    = reinterpret_cast<AssignFn>(*assign);
    g_ui        = *ui;
    g_topVtable = *topVtable;
    return true;
}

AssignFn Assign() noexcept
{
    return g_assign;
}

void* System() noexcept
{
    return reinterpret_cast<void*>(g_ui);
}

void* Manager(std::uint32_t slot) noexcept
{
    return g_ui != 0 ? platform::ReadPointer(reinterpret_cast<const void*>(g_ui), slot * 8) : nullptr;
}

bool IsTopWindow(const void* window) noexcept
{
    return window != nullptr && g_topVtable != 0 && platform::ReadPointer(window, 0) == reinterpret_cast<void*>(g_topVtable);
}

void* WeakAt(const void* object, std::uint32_t offset) noexcept
{
    std::int32_t pair[2]{};
    return object != nullptr && platform::Read(static_cast<const std::uint8_t*>(object) + offset, pair) ? weak::ObjectOf(pair) : nullptr;
}

std::wstring ReadString(const void* at) noexcept
try
{
    platform::GameArray s;
    if (!platform::Read(at, s) || s.data == nullptr || s.num <= 1 || s.num > 4096)
    {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(s.num - 1), L'\0');
    return platform::CopyGuarded(text.data(), s.data, text.size() * sizeof(wchar_t)) ? text : std::wstring{};
}
catch (...)
{
    return {};
}

bool AssignString(void* to, const std::wstring& text) noexcept
{
    const platform::GameArray source = platform::SourceString(text);
    void*                     out    = nullptr;
    return platform::CallGuarded(g_assign, out, to, static_cast<const void*>(&source));
}

}  // namespace lss::menu::game_ui
