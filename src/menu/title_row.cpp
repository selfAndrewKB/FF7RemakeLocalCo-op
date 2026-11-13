#include "menu/title_row.h"

#include "engine_facts.h"
#include "menu/coop_menu_rules.h"
#include "menu/coop_page_open.h"
#include "menu/game_ui.h"
#include "menu/title_row_rules.h"
#include "platform/entry_redirect.h"
#include "platform/game_array.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>

namespace lss::menu::title_row {

namespace {

using game_ui::At;
using platform::GameArray;

using ConstructFn = void (*)(void* start);
using GrowFn      = void (*)(void* array);
using FindFn      = void* (*)(void* tree, const void* name);
using SetCountFn  = void (*)(void* box, std::int32_t count);
using RefreshFn   = void (*)(void* box);
using IsAFn       = bool (*)(void* object);
using SetSizeFn   = void (*)(void* slot, std::uint64_t size);

constexpr int kMostRows = 16;  // far more than the game builds: a longer list is not the one the research read

/// Where the row's pieces are, found at install.
struct Game
{
    std::uintptr_t pressAt = 0, textSlot = 0, textWas = 0, listName = 0;
    GrowFn         grow     = nullptr;
    FindFn         find     = nullptr;
    SetCountFn     setCount = nullptr;
    RefreshFn      refresh  = nullptr;
    IsAFn          isBox    = nullptr;
    IsAFn          isCanvasSlot = nullptr;
    SetSizeFn      setSize  = nullptr;
};

// Set at install and on the first tick, then only read.
Game                    g;
platform::VTableEntry   g_construct, g_text;
platform::EntryRedirect g_press;
bool                    g_placed = false;

// Game thread.
std::uint64_t g_inserted = 0;
std::uint64_t g_failed   = 0;

/// The kind's place taken: the array grown the way the construct grows it, the kinds below moved down. Raw: inside a guard.
void InsertRaw(GameArray* kinds, std::size_t place)
{
    const std::int32_t was = kinds->num;
    kinds->num             = was + 1;
    if (kinds->num > kinds->max)
    {
        g.grow(kinds);
    }
    auto* const data = static_cast<std::int32_t*>(kinds->data);
    std::memmove(data + place + 1, data + place, (static_cast<std::size_t>(was) - place) * sizeof(std::int32_t));
    data[place] = facts::kStartMenuKindCoop;
}

bool InsertGuarded(GameArray* kinds, std::size_t place) noexcept
{
    __try
    {
        InsertRaw(kinds, place);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// The list told of its new length, as the construct's last steps do. Each step guarded: a failure leaves the row in the
/// list, whose press and text go by kind.
const wchar_t* ShowInList(void* start, std::int32_t rows) noexcept
{
    void* const tree = platform::ReadPointer(start, facts::kUserWidgetTree);
    void*       list = nullptr;
    bool        isBox = false;
    if (tree == nullptr || !platform::CallGuarded(g.find, list, tree, reinterpret_cast<const void*>(g.listName)) || list == nullptr ||
        !platform::CallGuarded(g.isBox, isBox, list) || !isBox)
    {
        return L"its list could not be found";
    }
    if (!platform::CallGuarded(g.setCount, list, rows) || !platform::CallGuarded(g.refresh, list))
    {
        return L"its list could not be refreshed";
    }
    float       width = 0.0f, base = 0.0f;
    void* const slot  = game_ui::SlotOf(list);
    bool        isCanvasSlot = false;
    if (slot == nullptr || !platform::Read(At(start, facts::kStartMenuListWidth), width) ||
        !platform::Read(At(start, facts::kStartMenuListBase), base) || !platform::CallGuarded(g.isCanvasSlot, isCanvasSlot, slot) ||
        !isCanvasSlot)
    {
        return L"its list's height could not be read";
    }
    const float height = static_cast<float>(rows - facts::kStartMenuBaseRows) * facts::kStartMenuRowHeight + base;
    const auto  packed = static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(width)) |
                         (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(height)) << 32);
    return platform::CallGuarded(g.setSize, slot, packed) ? nullptr : L"its list's height could not be set";
}

/// The Co-op row below Options in a list the construct just built.
void AddRow(void* start) noexcept
{
    GameArray kinds;
    if (!platform::Read(At(start, facts::kStartMenuKinds), kinds) || kinds.data == nullptr || kinds.num <= 0 || kinds.num >= kMostRows)
    {
        ++g_failed;
        platform::LogWarning(L"Co-op menu: the title menu's rows could not be read ({}); no Co-op row there", kinds.num);
        return;
    }
    std::array<std::int32_t, kMostRows> read{};
    if (!platform::CopyGuarded(read.data(), kinds.data, static_cast<std::size_t>(kinds.num) * sizeof(std::int32_t)))
    {
        ++g_failed;
        platform::LogWarning(L"Co-op menu: the title menu's rows could not be copied; no Co-op row there");
        return;
    }
    const auto place = title_row_rules::PlaceBelow(std::span(read.data(), static_cast<std::size_t>(kinds.num)), facts::kStartMenuKindOptions,
                                                   facts::kStartMenuKindCoop);
    if (!place)
    {
        platform::LogDebug(L"Co-op menu: the title menu's {} rows need no Co-op row added", kinds.num);
        return;
    }
    auto* const live = reinterpret_cast<GameArray*>(At(start, facts::kStartMenuKinds));
    if (!InsertGuarded(live, *place))
    {
        ++g_failed;
        platform::LogWarning(L"Co-op menu: inserting the title menu's Co-op row faulted");
        return;
    }
    if (const wchar_t* failed = ShowInList(start, kinds.num + 1))
    {
        ++g_failed;
        platform::LogWarning(L"Co-op menu: the title menu's Co-op row is in its rows, but {}", failed);
        return;
    }
    ++g_inserted;
    platform::LogDebug(L"Co-op menu: a Co-op row in the title menu, place {} of {}", *place, kinds.num + 1);
}

/// The start menu's construct (vtable slot): the game's, then the row.
void Construct(void* start)
{
    reinterpret_cast<ConstructFn>(g_construct.Original())(start);
    AddRow(start);
}

/// The kind's press (its one caller is the row press's switch): the page, when the start menu has no flow of its own
/// running, the test the game's handler for the kind makes.
void Press(void* start)
{
    std::int32_t action = -1;
    if (platform::Read(At(start, facts::kStartMenuAction), action) && action == 0)
    {
        coop_page_open::OpenAtTitle(start);
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
try
{
    namespace f = facts;
    const auto press    = platform::FindFunction(image, f::kStartMenuKindCoopPress);
    const auto grow     = platform::FindFunction(image, f::kIntArrayGrow);
    const auto find     = platform::FindFunction(image, f::kWidgetTreeFind);
    const auto setCount = platform::FindFunction(image, f::kScrollBoxSetCount);
    const auto refresh  = platform::FindFunction(image, f::kScrollBoxRefresh);
    const auto castBox  = platform::FindFunction(image, f::kCastScrollBox);
    const auto castSlot = platform::FindFunction(image, f::kCastCanvasSlot);
    const auto setSize  = platform::FindFunction(image, f::kCanvasSlotSetSize);
    const auto textSlot = image.Rebase(f::kStartMenuRowTextSlot, 8);
    const auto textWas  = image.Rebase(f::kStartMenuRowTextWas);
    const auto listName = image.Rebase(f::kNameListMenu, 8);
    if (!press || !grow || !find || !setCount || !refresh || !castBox || !castSlot || !setSize || !textSlot || !textWas || !listName ||
        !coop_page_open::InstallTitle(image))
    {
        platform::LogWarning(L"Co-op menu: a check of the title menu failed; no Co-op row there (the main menu's stays)");
        return false;
    }
    g.pressAt  = *press;
    g.grow     = reinterpret_cast<GrowFn>(*grow);
    g.find     = reinterpret_cast<FindFn>(*find);
    g.setCount = reinterpret_cast<SetCountFn>(*setCount);
    g.refresh  = reinterpret_cast<RefreshFn>(*refresh);
    g.isBox    = reinterpret_cast<IsAFn>(*castBox);
    g.isCanvasSlot = reinterpret_cast<IsAFn>(*castSlot);
    g.setSize  = reinterpret_cast<SetSizeFn>(*setSize);
    g.textSlot = *textSlot;
    g.textWas  = *textWas;
    g.listName = *listName;
    return true;
}
catch (...)
{
    return false;
}

bool Place(const platform::GameImage& image) noexcept
{
    g_placed = g_press.InstallWithoutOriginal(g.pressAt, std::span(facts::kStartMenuKindCoopEntry.data(), facts::kStartMenuKindCoopFirstBytes),
                                              reinterpret_cast<std::uintptr_t>(&Press)) &&
               g_text.Replace(g.textSlot, g.textWas, reinterpret_cast<std::uintptr_t>(coop_menu_rules::kRowLabel)) &&
               platform::ReplaceSlot(image, facts::kStartMenuConstruct, reinterpret_cast<std::uintptr_t>(&Construct), g_construct);
    if (g_placed)
    {
        platform::Log(L"Co-op menu: a Co-op row in the title menu below Options (the game's unused Download Save Data row)");
    }
    else
    {
        Unplace();
        platform::LogWarning(L"Co-op menu: the title menu's row could not be placed; no Co-op row there (the main menu's stays)");
    }
    return g_placed;
}

void Unplace() noexcept
{
    g_construct.Restore();
    g_text.Restore();
    g_press.Restore();
    g_placed = false;
}

std::wstring Reading()
{
    return std::format(L"; title row {}, added {}, failed {}", g_placed ? L"placed" : L"not placed", g_inserted, g_failed);
}

}  // namespace lss::menu::title_row
