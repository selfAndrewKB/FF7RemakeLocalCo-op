#include "menu/coop_menu.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "control/registry.h"
#include "engine_facts.h"
#include "menu/coop_menu_rules.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/weak_object.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lss::menu::coop_menu {

namespace {

namespace rules    = coop_menu_rules;
namespace settings = platform::settings;
namespace weak     = players::weak_object;

using OpenWindowFn = void (*)(void* mgr, std::uint8_t kind);
using ShowTopFn    = void* (*)(void* mgr, bool restoreCursor);
using GetValueFn   = std::int32_t (*)(void* page, std::uint8_t key, std::int32_t row);
using SetValueFn   = void* (*)(void* page, std::uint8_t category, std::int32_t value);
using KeyDownFn    = std::uintptr_t (*)(void* page, void* a, void* b, void* c);
using CreateFn     = void* (*)(void* world, void* cls, std::uint8_t inGame);
using OpenFn       = void (*)(void* page, std::uint8_t startCategory);
using AddToRootFn  = bool (*)(void* widget, std::int32_t zOrder);
using InRootFn     = bool (*)(void* widget);
using RemoveFn     = void* (*)(void* widget);
using TakeWidgetFn = void* (*)(void* widget, void* out);
using PopLayerFn   = void (*)(void* ui, void* taken);
using AssignFn     = void* (*)(void* to, const void* from);
using FindOrAddFn  = void* (*)(void* map, const std::uint8_t* key);
using InfoCtorFn   = void* (*)(void* info);
using InfoDtorFn   = void (*)(void* info);
using InfoMoveFn   = void* (*)(void* to, void* from);
using GrowFn       = void (*)(void* array);
using RemoveAtFn   = void (*)(void* array, std::int32_t index, std::int32_t count);
using SetCountFn   = void (*)(void* box, std::int32_t count);
using RefreshFn    = void (*)(void* box);
using ObjectFn     = void* (*)(void* object);  // a window's Hide, an object's GetWorld
using SetVisibilityFn = void (*)(void* widget, std::uint8_t visibility);

/// An FString or TArray as the game lays them out.
struct GameArray
{
    void*        data = nullptr;
    std::int32_t num  = 0;
    std::int32_t max  = 0;
};
static_assert(sizeof(GameArray) == 16);

/// A source FString over the mod's own text (the game's copy reads only its Data and Num).
GameArray View(const wchar_t* text, std::size_t length) noexcept
{
    return length == 0 ? GameArray{} : GameArray{const_cast<wchar_t*>(text), static_cast<std::int32_t>(length + 1), 0};
}
GameArray View(const wchar_t* text) noexcept
{
    return View(text, std::wcslen(text));
}

// The guide while the page is open: the page's own keys (the Top window's guide shows whatever has focus).
constexpr const wchar_t* kGuide = L"<button=decide> Choose  <button=cancel> Back";

/// The game's functions, found at install.
struct Game
{
    OpenWindowFn  openWindow = nullptr;  // the redirected originals' entries (calls go through the redirects' Original)
    ShowTopFn     showTop    = nullptr;
    CreateFn      create     = nullptr;
    OpenFn        open       = nullptr;
    AddToRootFn   addToRoot  = nullptr;
    InRootFn      inRoot     = nullptr;
    RemoveFn      remove     = nullptr;
    TakeWidgetFn  takeWidget = nullptr;
    PopLayerFn    popLayer   = nullptr;
    AssignFn      assign     = nullptr;
    FindOrAddFn   findOrAdd  = nullptr;
    InfoCtorFn    infoCtor   = nullptr;
    InfoDtorFn    infoDtor   = nullptr;
    InfoMoveFn    infoMove   = nullptr;
    GrowFn        growInfos  = nullptr;
    GrowFn        growTexts  = nullptr;
    RemoveAtFn    removeAt   = nullptr;
    SetCountFn    setCount   = nullptr;
    RefreshFn     refresh    = nullptr;
    SetVisibilityFn setVisibility = nullptr;  // the dev switch menu.coop-page-hidden
    std::uintptr_t ui        = 0;  // EndUISystem's array of managers
    std::uintptr_t topVtable = 0;
    // Where the redirects and the table words go (live), placed on the first tick.
    std::uintptr_t openWindowAt = 0, getValueAt = 0, setValueAt = 0;
    std::uintptr_t textSlot = 0, textWas = 0, helpSlot = 0, helpWas = 0, unlockSlot = 0, orderWord = 0;
    std::uintptr_t headings = 0;  // g_OptionsMenu_CategoryHeadingIds
    std::array<std::uintptr_t, facts::kOptionsHeadingsWere.size()> headingsWere{};
};

// Set at install and on the first tick, then only read.
Game                       g;
platform::EntryRedirect    g_openWindow, g_getValue, g_setValue;
platform::VTableEntry      g_keyDown, g_text, g_help, g_unlock, g_order;
/// The page's categories' entries of the game's heading table, swapped for their labels while the page is open (the
/// game's own Options page reads the same table).
std::array<platform::VTableEntry, rules::kCategoryKeys.size()> g_headings;
std::vector<rules::Category> g_page;  // the page's categories and rows
bool                       g_installed = false;
bool                       g_pending   = false;  // the first tick places everything
bool                       g_placed    = false;  // the row shows and opens the page

// Game thread.
void*        g_class = nullptr;   // MainMenu_OptionsTop2_C, found at the first open
weak::Weak   g_pageWeak;          // the mod's page, kept while alive (after a close it takes itself out of the overlay, so
                                  // each open adds it again and its construct runs again)
bool         g_filled = false;    // the page holds the mod's data (false across a re-add, whose construct puts the game's back,
                                  // and once the page is gone: its address may become System's own page)
bool         g_open   = false;    // the page is open over the hidden Top window
void*        g_mgr    = nullptr;  // the main menu manager while open
weak::Weak   g_topWeak;
std::wstring g_savedTitle, g_savedGuide;
bool         g_failed = false;  // a fault while writing the page: the row opens nothing until the next start
std::uint64_t g_opens = 0, g_commits = 0;
int           g_pageVisibility = -1;  // the page's own visibility while the dev switch hides it, else -1
/// A value the page committed, run by the next tick (an option's command may resize the window: never inside the
/// page's key event). The last commit per option wins.
std::array<std::optional<int>, settings::kOptions.size()> g_queued{};
/// The value the page last showed per option (a commit equal to it changes nothing when the current word is unknown).
std::array<int, settings::kOptions.size()> g_shown{};

void* Ui(std::uint32_t slot) noexcept
{
    return g.ui != 0 ? platform::ReadPointer(reinterpret_cast<const void*>(g.ui), slot * 8) : nullptr;
}

void* WeakAt(const void* object, std::uint32_t offset) noexcept
{
    std::int32_t pair[2]{};
    return object != nullptr && platform::Read(static_cast<const std::uint8_t*>(object) + offset, pair) ? weak::ObjectOf(pair) : nullptr;
}

std::uint8_t* At(void* object, std::uint32_t offset) noexcept
{
    return static_cast<std::uint8_t*>(object) + offset;
}

/// An FString of the game's, read into ours (empty if unreadable).
std::wstring ReadString(const void* at) noexcept
try
{
    GameArray s;
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

/// The game's copy of `text` into the FString at `to`. False if it faulted.
bool AssignString(void* to, const std::wstring& text) noexcept
{
    const GameArray view = View(text.c_str(), text.size());
    void*           out  = nullptr;
    return platform::CallGuarded(g.assign, out, to, static_cast<const void*>(&view));
}

// --- Values: the settings table's options through their console commands ------------------------------------------

/// The mod's page, by its weak pointer (the serial too: a dead page's address may hold another object).
bool IsModPage(const void* page) noexcept
{
    return page != nullptr && page == weak::Resolve(g_pageWeak);
}

const control::Entry* EntryOf(int option) noexcept
{
    return control::Global().Find(settings::kOptions[static_cast<std::size_t>(option)]->key);
}

std::optional<std::wstring> CurrentWord(int option)
{
    const control::Entry* entry = EntryOf(option);
    return entry != nullptr && entry->get ? rules::WordOfReading(entry->get()) : std::nullopt;
}

/// The option a row of the page shows, by the screen's category key and row index.
std::optional<int> OptionAt(std::uint8_t key, std::int32_t row) noexcept
{
    for (const rules::Category& category : g_page)
    {
        if (category.key == key && row >= 0 && row < static_cast<std::int32_t>(category.options.size()))
        {
            return category.options[static_cast<std::size_t>(row)];
        }
    }
    return std::nullopt;
}

/// The game's value read: the mod's rows answer their option's word's place (a commit not yet run: its value).
std::int32_t GetValue(void* page, std::uint8_t key, std::int32_t row)
{
    if (g_filled && IsModPage(page))
    {
        try
        {
            if (const auto option = OptionAt(key, row))
            {
                const auto              i     = static_cast<std::size_t>(*option);
                const settings::Option& shown = *settings::kOptions[i];
                const auto              word  = CurrentWord(*option);
                const int value = g_queued[i] ? *g_queued[i] : (word ? rules::ValueOf(shown, *word).value_or(0) : 0);
                g_shown[i]      = value;
                platform::LogTrace(L"Co-op page: {} shows {} (the option reads {})", shown.key, value,
                                   word ? *word : std::wstring(L"nothing readable"));
                return value;
            }
        }
        catch (...)
        {
            return 0;
        }
    }
    return reinterpret_cast<GetValueFn>(g_getValue.Original())(page, key, row);
}

/// The game's value commit: a mod row's value is queued for its option's command, run by the next tick (an unchanged
/// value does nothing: the game re-commits current values at its construct and its Reset All).
void* SetValue(void* page, std::uint8_t category, std::int32_t value)
{
    const auto option = rules::OptionOf(category);
    if (!option)
    {
        return reinterpret_cast<SetValueFn>(g_setValue.Original())(page, category, value);
    }
    try
    {
        const auto              i   = static_cast<std::size_t>(*option);
        const settings::Option& row = *settings::kOptions[i];
        if (!g_open || !IsModPage(page))
        {
            platform::LogDebug(L"Co-op page: a commit of {} to {} outside the open page; nothing changed", row.key, value);
            return page;
        }
        const auto word = rules::WordOf(row, value);
        if (!word)
        {
            platform::LogWarning(L"Co-op page: {} has no value {}; nothing changed", row.key, value);
            return page;
        }
        const auto current = g_queued[i] ? rules::WordOf(row, *g_queued[i]) : CurrentWord(*option);
        if ((current && *current == *word) || (!current && value == g_shown[i]))
        {
            platform::LogDebug(L"Co-op page: {} stays {}", row.key, *word);
            return page;
        }
        g_queued[i] = value;
        platform::LogDebug(L"Co-op page: {} to {}, on the next tick", row.key, *word);
    }
    catch (...)
    {
        platform::LogWarning(L"Co-op page: a commit raised an exception; nothing changed");
    }
    return page;
}

/// The queued commits, each through its option's console command (as the console and the keys run them).
void RunQueued() noexcept
try
{
    for (std::size_t i = 0; i < g_queued.size(); ++i)
    {
        if (!g_queued[i])
        {
            continue;
        }
        const int value = *g_queued[i];
        g_queued[i].reset();
        const settings::Option& row   = *settings::kOptions[i];
        const auto              word  = rules::WordOf(row, value);
        const control::Entry*   entry = EntryOf(static_cast<int>(i));
        if (!word || entry == nullptr || !entry->set)
        {
            platform::LogWarning(L"Co-op page: {} has no command to take {}; nothing changed", row.key, value);
            continue;
        }
        const control::Outcome outcome = entry->set(rules::CommandValue(entry->shape, *word));
        ++g_commits;
        platform::Log(L"Co-op page: {} set to {} ({} {}){}{}", row.label, row.wordLabels[static_cast<std::size_t>(value)], row.key,
                      *word, outcome.ok ? L"" : L"; refused: ", outcome.ok ? std::wstring() : outcome.detail);
    }
}
catch (...)
{
    platform::LogWarning(L"Co-op page: running a change raised an exception; the rest waits for the next tick");
}

// --- The Top window while the page is open ----------------------------------------------------------------------

/// The heading table's entries for the page's categories: their labels while the page is open, the game's after.
void SwapHeadings(bool labels) noexcept
{
    for (std::size_t i = 0; i < g_page.size() && i < g_headings.size(); ++i)
    {
        const std::uint8_t key = g_page[i].key;
        if (key >= g.headingsWere.size())
        {
            continue;
        }
        if (!labels)
        {
            g_headings[i].Restore();
        }
        else if (!g_headings[i].Active() &&
                 !g_headings[i].Replace(g.headings + 8 * key, g.headingsWere[key], reinterpret_cast<std::uintptr_t>(g_page[i].text.label)))
        {
            platform::LogWarning(L"Co-op page: the heading of {} could not be set; it shows the game's", g_page[i].text.label);
        }
    }
}

/// The Top window's title and guide back as they were.
void RestoreTop() noexcept
{
    void* const top = weak::Resolve(g_topWeak);
    if (top == nullptr)
    {
        return;
    }
    if (!AssignString(At(top, facts::kWindowTitle), g_savedTitle) || !AssignString(At(top, facts::kTopGuide), g_savedGuide))
    {
        platform::LogWarning(L"Co-op page: the main menu's title or guide could not be put back");
    }
}

/// The dev switch's hide undone at a close (the page is the same object at the next open), unless the game has set the
/// page's visibility since.
void ShowPageAgain(void* page) noexcept
{
    std::uint8_t now = 0xFF;
    if (g_pageVisibility >= 0 && page != nullptr && platform::Read(At(page, facts::kWidgetVisibility), now) &&
        now == facts::kVisibilityHidden)
    {
        platform::CallGuarded(g.setVisibility, page, static_cast<std::uint8_t>(g_pageVisibility));
    }
    g_pageVisibility = -1;
}

/// The page closed (by its own Back, or gone): the Top window back, as after any of the game's windows.
void Closed(const wchar_t* how) noexcept
{
    g_open = false;
    ShowPageAgain(weak::Resolve(g_pageWeak));
    SwapHeadings(false);
    RestoreTop();
    void* shown = nullptr;
    const bool ok = g_mgr != nullptr && platform::CallGuarded(g.showTop, shown, g_mgr, true) && shown != nullptr;
    platform::Log(L"Co-op page: closed ({}); {}", how, ok ? L"the main menu is back" : L"the main menu could not be shown again (it closed)");
    g_mgr = nullptr;
}

/// The page taken down the way its own no-animation Back does (its focus layer popped, out of the overlay), for a
/// main menu closed under it.
void ForceClose(void* page, const wchar_t* why) noexcept
{
    g_open = false;
    ShowPageAgain(page);
    SwapHeadings(false);
    if (page != nullptr)
    {
        alignas(16) std::uint8_t taken[16]{};
        void* ref = nullptr;
        if (platform::CallGuarded(g.takeWidget, ref, page, static_cast<void*>(taken)) && ref != nullptr)
        {
            platform::CallGuarded(g.popLayer, reinterpret_cast<void*>(g.ui), ref);
        }
        void* removed = nullptr;
        platform::CallGuarded(g.remove, removed, page);
        const std::uint8_t none = 0;
        platform::WriteGuarded(At(page, facts::kOptionsState), &none, sizeof(none));
    }
    RestoreTop();
    g_mgr = nullptr;
    platform::Log(L"Co-op page: closed with the main menu ({}); its layer popped and the page taken out", why);
}

/// The page's key handler (vtable slot): the game's, then the Top window back if Back closed the mod's page.
std::uintptr_t KeyDown(void* page, void* a, void* b, void* c)
{
    const std::uintptr_t reply = reinterpret_cast<KeyDownFn>(g_keyDown.Original())(page, a, b, c);
    if (g_open && IsModPage(page))
    {
        std::uint8_t state = 0xFF;
        if (platform::Read(At(page, facts::kOptionsState), state) && (state == facts::kOptionsStateClosing || state == 0))
        {
            Closed(L"Back");
        }
    }
    return reply;
}

// --- The page --------------------------------------------------------------------------------------------------

/// `text` appended to a TArray<FString> of the game's, grown first and counted once built. False if it did not grow.
bool AppendString(GameArray* strings, const wchar_t* text)
{
    if (strings->num + 1 > strings->max)
    {
        g.growTexts(strings);
    }
    if (strings->num >= strings->max || strings->data == nullptr)
    {
        return false;
    }
    std::uint8_t* element = static_cast<std::uint8_t*>(strings->data) + 16 * static_cast<std::size_t>(strings->num);
    std::memset(element, 0, 16);
    const GameArray view = View(text);
    g.assign(element, &view);
    ++strings->num;
    return true;
}

/// One List row appended to `rows` (TArray<FOptionInfo>), built the way the game's construct builds one; each element is
/// built in room grown first and counted only once built, so a fault never leaves an unbuilt element counted (the
/// growths read Num and set Max above it). Raw calls: run inside FillPage's guard. 0 when done, else what failed.
const wchar_t* AppendRow(GameArray* rows, int option)
{
    const settings::Option&      row = *settings::kOptions[static_cast<std::size_t>(option)];
    alignas(16) std::uint8_t     info[facts::kOptionInfoSize];
    g.infoCtor(info);
    info[0]                                  = 0;  // List
    info[facts::kOptionInfoCategory]         = rules::RowByte(option);
    std::memset(info + facts::kOptionInfoMin, 0, 8);  // Min, Max (the constructor leaves them)
    const GameArray text = View(row.label);
    const GameArray help = View(row.menuHelp);
    g.assign(info + facts::kOptionInfoText, &text);
    g.assign(info + facts::kOptionInfoHelp, &help);
    // Each value with the row's help: the value list's cells are reused, and a cell past ValueHelps' end keeps the help
    // it showed last (a game row's).
    auto* values = reinterpret_cast<GameArray*>(info + facts::kOptionInfoValues);
    auto* helps  = reinterpret_cast<GameArray*>(info + facts::kOptionInfoValueHelps);
    for (const wchar_t* name : row.wordLabels)
    {
        if (!AppendString(values, name) || !AppendString(helps, row.menuHelp))
        {
            g.infoDtor(info);
            return L"a value list did not grow";
        }
    }
    if (rows->num + 1 > rows->max)
    {
        g.growInfos(rows);
    }
    if (rows->num >= rows->max || rows->data == nullptr)
    {
        g.infoDtor(info);
        return L"a category's rows did not grow";
    }
    g.infoMove(static_cast<std::uint8_t*>(rows->data) + facts::kOptionInfoSize * static_cast<std::size_t>(rows->num), info);
    ++rows->num;
    g.infoDtor(info);  // frees nothing after the move
    return nullptr;
}

/// The page's categories, rows and left list replaced with the mod's, through the game's own helpers. 0 when done,
/// else the step that failed (a fault included).
const wchar_t* FillPageRaw(void* page)
{
    auto* labels = reinterpret_cast<GameArray*>(At(page, facts::kOptionsLabels));
    auto* order  = reinterpret_cast<GameArray*>(At(page, facts::kOptionsOrder));
    if (order->max < static_cast<std::int32_t>(g_page.size()) || order->data == nullptr)
    {
        return L"the category order has no room";
    }
    for (const rules::Category& category : g_page)
    {
        if (category.key >= labels->num || labels->data == nullptr)
        {
            return L"a category's label is missing";
        }
        const GameArray label = View(category.text.label);
        g.assign(static_cast<std::uint8_t*>(labels->data) + 16 * static_cast<std::size_t>(category.key), &label);
        void* const infos = g.findOrAdd(At(page, facts::kOptionsItems), &category.key);
        if (infos == nullptr)
        {
            return L"a category's rows could not be found";
        }
        const GameArray help = View(category.text.help);
        g.assign(At(infos, facts::kOptionInfosHelp), &help);
        auto* rows = reinterpret_cast<GameArray*>(At(infos, facts::kOptionInfosRows));
        if (rows->num > 0)
        {
            g.removeAt(rows, 0, rows->num);
        }
        // The game's construct only appends rows and finds categories by key (checked in IDA for the reviewer, 2026-10-31),
        // so after a re-add it adds its rows behind the mod's, which this clears again.
        for (const int option : category.options)
        {
            if (const wchar_t* failed = AppendRow(rows, option))
            {
                return failed;
            }
        }
    }
    for (std::size_t i = 0; i < g_page.size(); ++i)
    {
        static_cast<std::uint8_t*>(order->data)[i] = g_page[i].key;
    }
    order->num = static_cast<std::int32_t>(g_page.size());
    *reinterpret_cast<std::int32_t*>(At(page, facts::kOptionsCategoryIndex)) = 0;
    *reinterpret_cast<std::int32_t*>(At(page, facts::kOptionsRowIndex))      = 0;
    void* const box = *reinterpret_cast<void**>(At(page, facts::kOptionsCategoryList));
    if (box == nullptr)
    {
        return L"the category list is missing";
    }
    g.setCount(box, order->num);
    g.refresh(box);
    return nullptr;
}

const wchar_t* FillPage(void* page) noexcept
{
    __try
    {
        return FillPageRaw(page);
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return L"a fault while writing it";
    }
}

/// The mod's page, alive and in the root overlay with the mod's data, or null (logged).
void* ReadyPage(void* top) noexcept
{
    void* page = weak::Resolve(g_pageWeak);
    if (page == nullptr)
    {
        if (g_class == nullptr)
        {
            g_class = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(nullptr, nullptr, facts::kOptionsClassPath);
        }
        void* world = nullptr;
        const auto getWorld = reinterpret_cast<ObjectFn>(platform::ReadPointer(platform::ReadPointer(top, 0), facts::kObjectGetWorldSlot));
        if (g_class == nullptr || getWorld == nullptr || !platform::CallGuarded(getWorld, world, top) || world == nullptr ||
            !platform::CallGuarded(g.create, page, world, g_class, std::uint8_t{1}) || page == nullptr)
        {
            platform::LogWarning(L"Co-op page: the game's Options screen could not be made ({}); the row opens nothing",
                                 g_class == nullptr ? L"its class is not loaded" : L"its creation failed");
            return nullptr;
        }
        g_pageWeak = weak::MakeWeak(page);
        g_filled   = false;
        platform::Log(L"Co-op page: made the mod's own Options screen (not the one System keeps)");
    }
    bool inRoot = false;
    if (!platform::CallGuarded(g.inRoot, inRoot, page))
    {
        return nullptr;
    }
    if (!inRoot)
    {
        std::int32_t z = 0;
        platform::Read(At(top, facts::kWindowZOrder), z);
        platform::WriteGuarded(At(page, facts::kWindowZOrder), &z, sizeof(z));
        g_filled   = false;  // its construct runs now, over the game's data
        bool added = false;
        if (!platform::CallGuarded(g.addToRoot, added, page, z))
        {
            platform::LogWarning(L"Co-op page: adding the page to the screen faulted; the row opens nothing");
            return nullptr;
        }
    }
    if (const wchar_t* failed = FillPage(page))
    {
        g_failed = true;
        platform::LogError(L"Co-op page: writing the page failed ({}); the row opens nothing until the game restarts", failed);
        return nullptr;
    }
    g_filled = true;
    return page;
}

/// The row's press: the page over the hidden Top window, as the game shows a window (its tutorial gate and prelude
/// first), with the manager's current window left on Top so its title, Back and re-open paths stay the game's.
void OpenPage(void* mgr) noexcept
try
{
    if (g_open || g_failed)
    {
        return;
    }
    std::uint8_t tutorial = 0;
    if (void* const t = Ui(facts::kUiMenuTutorialSlot);
        t != nullptr && platform::Read(At(t, facts::kMenuTutorialState), tutorial) &&
        tutorial >= facts::kMenuTutorialBusyFirst && tutorial <= facts::kMenuTutorialBusyLast)
    {
        platform::LogDebug(L"Co-op page: the menu tutorial is busy ({}); not opened, as the game opens no window then", tutorial);
        return;
    }
    platform::CallGuarded(reinterpret_cast<OpenWindowFn>(g_openWindow.Original()), mgr, facts::kOpenWindowPreludeOnly);
    void* const  top        = WeakAt(mgr, facts::kMainMenuMgrCurrent);
    std::uint8_t topCurrent = 0;
    if (top == nullptr || platform::ReadPointer(top, 0) != reinterpret_cast<void*>(g.topVtable) ||
        !platform::Read(At(mgr, facts::kMainMenuMgrTopCurrent), topCurrent) || topCurrent != 1)
    {
        platform::LogWarning(L"Co-op page: the main menu's current window is not its first page; not opened");
        return;
    }
    void* const page = ReadyPage(top);
    if (page == nullptr)
    {
        return;
    }
    const auto hide = reinterpret_cast<ObjectFn>(platform::ReadPointer(platform::ReadPointer(top, 0), facts::kWindowHideSlot));
    void* ignored = nullptr;
    if (hide == nullptr || !platform::CallGuarded(hide, ignored, top))
    {
        platform::LogWarning(L"Co-op page: the main menu's first page could not be hidden; not opened");
        return;
    }
    const std::uint8_t hidden = 0;
    platform::WriteGuarded(At(top, facts::kWindowVisible), &hidden, 1);
    g_topWeak    = weak::MakeWeak(top);
    g_savedTitle = ReadString(At(top, facts::kWindowTitle));
    g_savedGuide = ReadString(At(top, facts::kTopGuide));
    AssignString(At(top, facts::kWindowTitle), rules::kTitle);
    AssignString(At(top, facts::kTopGuide), kGuide);
    SwapHeadings(true);
    g_mgr  = mgr;
    g_open = true;
    if (!platform::CallGuarded(g.open, page, std::uint8_t{0}))
    {
        platform::LogWarning(L"Co-op page: opening the page faulted; the main menu comes back");
        Closed(L"its opening faulted");
        return;
    }
    ++g_opens;
    platform::Log(L"Co-op page: opened ({} categories, {} options; the main menu's first page hidden, its title Co-op)",
                  g_page.size(), settings::kOptions.size());
}
catch (...)
{
}

/// The game's window opener: the Co-op row's kind opens the page; every other kind is the game's.
void OpenWindow(void* mgr, std::uint8_t kind)
{
    if (kind == facts::kTopRowKind && g_placed && mgr != nullptr)
    {
        OpenPage(mgr);
        return;
    }
    reinterpret_cast<OpenWindowFn>(g_openWindow.Original())(mgr, kind);
}

// --- Placing the row --------------------------------------------------------------------------------------------

/// The redirects, the key handler's slot, then the four table words (the row last, so it never shows unopenable).
bool Place(const platform::GameImage& image) noexcept
{
    const bool redirected =
        g_openWindow.Install(g.openWindowAt, std::span(facts::kOpenWindowByKindEntry.data(), facts::kRedirectFirstBytes),
                             reinterpret_cast<std::uintptr_t>(&OpenWindow)) &&
        g_getValue.Install(g.getValueAt, std::span(facts::kOptionsGetValueEntry.data(), facts::kRedirectFirstBytes),
                           reinterpret_cast<std::uintptr_t>(&GetValue)) &&
        g_setValue.Install(g.setValueAt, std::span(facts::kOptionsSetValueEntry.data(), facts::kRedirectFirstBytes),
                           reinterpret_cast<std::uintptr_t>(&SetValue)) &&
        platform::ReplaceSlot(image, facts::kOptionsKeyDown, reinterpret_cast<std::uintptr_t>(&KeyDown), g_keyDown);
    if (!redirected)
    {
        return false;
    }
    std::uintptr_t unlockWas = 0;
    if (!platform::Read(reinterpret_cast<const void*>(g.unlockSlot), unlockWas))
    {
        return false;
    }
    return g_text.Replace(g.textSlot, g.textWas, reinterpret_cast<std::uintptr_t>(rules::kRowLabel)) &&
           g_help.Replace(g.helpSlot, g.helpWas, reinterpret_cast<std::uintptr_t>(rules::kRowHelp)) &&
           g_unlock.Replace(g.unlockSlot, unlockWas, 0) &&
           g_order.Replace(g.orderWord, facts::kTopRowOrderWas, facts::kTopRowOrderCoop);
}

void Unplace() noexcept
{
    // The headings and the row first (their tables back), then the row's opener.
    SwapHeadings(false);
    g_order.Restore();
    g_unlock.Restore();
    g_help.Restore();
    g_text.Restore();
    g_keyDown.Restore();
    g_setValue.Restore();
    g_getValue.Restore();
    g_openWindow.Restore();
}

/// One of the main menu's frame texts as shown: its text and its own visibility.
std::wstring FrameText(const void* menu, std::uint32_t offset)
{
    void* const   block      = platform::ReadPointer(menu, offset);
    std::uint8_t  visibility = 0xFF;
    if (block == nullptr || !platform::Read(At(block, facts::kWidgetVisibility), visibility))
    {
        return L"(none)";
    }
    return std::format(L"'{}' (visibility {})", ReadString(At(block, facts::kTextBlockText)), visibility);
}

std::wstring Reading()
{
    std::wstring text = std::format(L"row {}; page {}{}; opened {}, values set {}", g_placed ? L"placed" : L"not placed",
                                    g_open ? L"open" : L"closed", g_failed ? L" (failed: opens nothing)" : L"", g_opens, g_commits);
    if (g_open && g_mgr != nullptr)
    {
        // The frame's texts while the page is open (a first open after launch shows them blank: research §11.5).
        void* const menu = WeakAt(g_mgr, facts::kMainMenuMgrMenu);
        std::uint8_t pageVisibility = 0xFF;
        if (void* const page = weak::Resolve(g_pageWeak))
        {
            platform::Read(At(page, facts::kWidgetVisibility), pageVisibility);
        }
        text += std::format(L"; main menu title {}, help {}, guide {}; page visibility {}", FrameText(menu, facts::kMainMenuTitleText),
                            FrameText(menu, facts::kMainMenuHelpText), FrameText(menu, facts::kMainMenuGuideText), pageVisibility);
    }
    void* const mgr = Ui(facts::kUiMainMenuSlot);
    void* const top = mgr != nullptr ? WeakAt(mgr, facts::kMainMenuMgrCurrent) : nullptr;
    GameArray   rows;
    if (top != nullptr && platform::ReadPointer(top, 0) == reinterpret_cast<void*>(g.topVtable) &&
        platform::Read(At(top, facts::kTopRows), rows) && rows.data != nullptr && rows.num > 0 && rows.num <= 16)
    {
        std::uint8_t kinds[16]{};
        if (platform::CopyGuarded(kinds, rows.data, static_cast<std::size_t>(rows.num)))
        {
            text += L"; main menu rows (kinds):";
            for (std::int32_t i = 0; i < rows.num; ++i)
            {
                text += std::format(L" {}", kinds[i]);
            }
        }
    }
    return text;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
try
{
    namespace f = facts;
    const auto openWindow = platform::FindFunction(image, f::kOpenWindowByKind);
    const auto getValue   = platform::FindFunction(image, f::kOptionsGetValue);
    const auto setValue   = platform::FindFunction(image, f::kOptionsSetValue);
    const auto showTop    = platform::FindFunction(image, f::kShowTopWindow);
    const auto create     = platform::FindFunction(image, f::kOptionsCreate);
    const auto open       = platform::FindFunction(image, f::kOptionsOpen);
    const auto addToRoot  = platform::FindFunction(image, f::kAddToRootOverlayFn);
    const auto inRoot     = platform::FindFunction(image, f::kIsInRootOverlay);
    const auto remove     = platform::FindFunction(image, f::kRemoveFromRootOverlay);
    const auto takeWidget = platform::FindFunction(image, f::kTakeWidget);
    const auto popLayer   = platform::FindFunction(image, f::kPopFocusLayer);
    const auto assign     = platform::FindFunction(image, f::kFStringAssign);
    const auto findOrAdd  = platform::FindFunction(image, f::kOptionItemsFindOrAdd);
    const auto infoCtor   = platform::FindFunction(image, f::kOptionInfoCtor);
    const auto infoDtor   = platform::FindFunction(image, f::kOptionInfoDtor);
    const auto infoMove   = platform::FindFunction(image, f::kOptionInfoMove);
    const auto growInfos  = platform::FindFunction(image, f::kOptionInfosGrow);
    const auto growTexts  = platform::FindFunction(image, f::kStringArrayGrow);
    const auto removeAt   = platform::FindFunction(image, f::kOptionInfosRemoveAt);
    const auto setCount   = platform::FindFunction(image, f::kScrollBoxSetCount);
    const auto refresh    = platform::FindFunction(image, f::kScrollBoxRefresh);
    const auto setVisible = platform::FindFunction(image, f::kWidgetSetVisibilityFn);
    const auto ui         = image.Rebase(f::kEndUISystemSlots, 8 * 64);
    const auto topVtable  = image.Rebase(f::kMainTopMenuVtable, 8);
    const auto textSlot   = image.Rebase(f::kTopRowTextSlot, 8);
    const auto textWas    = image.Rebase(f::kTopRowTextWas);
    const auto helpSlot   = image.Rebase(f::kTopRowHelpSlot, 8);
    const auto helpWas    = image.Rebase(f::kTopRowHelpWas);
    const auto unlockSlot = image.Rebase(f::kTopRowUnlockSlot, 8);
    const auto orderWord  = image.Rebase(f::kTopRowOrderWord, 8);
    const auto headings   = image.Rebase(f::kOptionsHeadings, 8 * f::kOptionsHeadingsWere.size());
    bool       headingsOk = headings.has_value();
    for (std::size_t key = 0; key < f::kOptionsHeadingsWere.size() && headingsOk; ++key)
    {
        const auto was = image.Rebase(f::kOptionsHeadingsWere[key]);
        headingsOk     = was.has_value();
        g.headingsWere[key] = was.value_or(0);
    }
    g_page                = rules::BuildPage();
    if (!openWindow || !getValue || !setValue || !showTop || !create || !open || !addToRoot || !inRoot || !remove || !takeWidget ||
        !popLayer || !assign || !findOrAdd || !infoCtor || !infoDtor || !infoMove || !growInfos || !growTexts || !removeAt ||
        !setCount || !refresh || !setVisible || !ui || !topVtable || !textSlot || !textWas || !helpSlot || !helpWas || !unlockSlot || !orderWord ||
        !headingsOk || g_page.empty() || !players::weak_object::Installed())
    {
        platform::Log(L"Co-op menu: an engine check failed; no Co-op row in the main menu (the options stay on the console and the "
                      L"settings file)");
        return false;
    }
    g.openWindowAt = *openWindow;
    g.getValueAt   = *getValue;
    g.setValueAt   = *setValue;
    g.showTop      = reinterpret_cast<ShowTopFn>(*showTop);
    g.create       = reinterpret_cast<CreateFn>(*create);
    g.open         = reinterpret_cast<OpenFn>(*open);
    g.addToRoot    = reinterpret_cast<AddToRootFn>(*addToRoot);
    g.inRoot       = reinterpret_cast<InRootFn>(*inRoot);
    g.remove       = reinterpret_cast<RemoveFn>(*remove);
    g.takeWidget   = reinterpret_cast<TakeWidgetFn>(*takeWidget);
    g.popLayer     = reinterpret_cast<PopLayerFn>(*popLayer);
    g.assign       = reinterpret_cast<AssignFn>(*assign);
    g.findOrAdd    = reinterpret_cast<FindOrAddFn>(*findOrAdd);
    g.infoCtor     = reinterpret_cast<InfoCtorFn>(*infoCtor);
    g.infoDtor     = reinterpret_cast<InfoDtorFn>(*infoDtor);
    g.infoMove     = reinterpret_cast<InfoMoveFn>(*infoMove);
    g.growInfos    = reinterpret_cast<GrowFn>(*growInfos);
    g.growTexts    = reinterpret_cast<GrowFn>(*growTexts);
    g.removeAt     = reinterpret_cast<RemoveAtFn>(*removeAt);
    g.setCount     = reinterpret_cast<SetCountFn>(*setCount);
    g.refresh      = reinterpret_cast<RefreshFn>(*refresh);
    g.setVisibility = reinterpret_cast<SetVisibilityFn>(*setVisible);
    g.ui           = *ui;
    g.topVtable    = *topVtable;
    g.textSlot     = *textSlot;
    g.textWas      = *textWas;
    g.helpSlot     = *helpSlot;
    g.helpWas      = *helpWas;
    g.unlockSlot   = *unlockSlot;
    g.orderWord    = *orderWord;
    g.headings     = *headings;
    g_installed    = true;
    g_pending      = true;  // placed by the first tick, on the game thread the redirected functions run on
    control::Register({.name  = L"menu.coop",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the Co-op row of the main menu and its page: placed, open, opened and set counts, and "
                                L"the main menu's rows by kind while its first page is current",
                       .get   = [] {
                           try
                           {
                               return control::Value{Reading()};
                           }
                           catch (...)
                           {
                               return control::Value{std::wstring(L"unreadable (an exception)")};
                           }
                       }});
    control::Register({.name  = L"menu.coop-page-hidden",
                       .shape = control::Shape::OnOff,
                       .help  = L"(dev) the open Co-op page hidden (on) or shown as before (off), to tell what it covers",
                       .set   = [](const control::Value& value) {
                           void* const page = g_open ? weak::Resolve(g_pageWeak) : nullptr;
                           if (page == nullptr)
                           {
                               return control::Outcome{false, L"the Co-op page is not open"};
                           }
                           const bool hide = std::get<bool>(value);
                           if (hide == (g_pageVisibility >= 0))
                           {
                               return control::Outcome{};
                           }
                           std::uint8_t was = 0;
                           if (hide && !platform::Read(At(page, facts::kWidgetVisibility), was))
                           {
                               return control::Outcome{false, L"the page's visibility is unreadable"};
                           }
                           const auto to = hide ? facts::kVisibilityHidden : static_cast<std::uint8_t>(g_pageVisibility);
                           if (!platform::CallGuarded(g.setVisibility, page, to))
                           {
                               return control::Outcome{false, L"the setter faulted"};
                           }
                           g_pageVisibility = hide ? was : -1;
                           platform::Log(L"Co-op page: {} (dev switch)", hide ? L"hidden" : L"shown again");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_pageVisibility >= 0}; }});
    return true;
}
catch (...)
{
    return false;
}

void Remove() noexcept
{
    if (!g_installed)
    {
        return;
    }
    // The unload may run off the game thread: no UI calls here, only the mod's own state and its changes put back. A page
    // open at that moment (the game is closing) is left to the game.
    if (g_open)
    {
        g_open = false;
        platform::Log(L"Co-op page: the mod unloads with the page open; the page is left to the game");
    }
    g_pageVisibility = -1;
    g_placed = false;
    Unplace();
}

void Tick() noexcept
{
    if (g_pending)
    {
        g_pending = false;
        if (const auto image = platform::GameImage::Locate(); image && Place(*image))
        {
            g_placed = true;
            platform::Log(L"Co-op menu: a Co-op row in the main menu before System (the trial's Buy Now row, which the full game never "
                          L"shows), opening the game's Options screen with the mod's {} options in {} categories",
                          settings::kOptions.size(), g_page.size());
        }
        else
        {
            Unplace();
            platform::Log(L"Co-op menu: the row's opener or tables could not be placed; no Co-op row (the options stay on the console "
                          L"and the settings file)");
        }
    }
    if (weak::Resolve(g_pageWeak) == nullptr)
    {
        g_filled = false;  // gone: its address may become another page's
    }
    RunQueued();
    if (!g_open)
    {
        return;
    }
    // A main menu closed under the page (no game path is known to while the page has focus): take it down.
    void* const  page     = weak::Resolve(g_pageWeak);
    void* const  mgr      = Ui(facts::kUiMainMenuSlot);
    std::int32_t menuOpen = 0;
    const bool   menuUp   = mgr == g_mgr && platform::Read(At(mgr, facts::kMainMenuMgrOpen), menuOpen) && menuOpen == 1 &&
                        WeakAt(mgr, facts::kMainMenuMgrMenu) != nullptr;
    if (page == nullptr)
    {
        platform::LogWarning(L"Co-op page: the page is gone while open");
        if (menuUp)
        {
            Closed(L"the page is gone");
        }
        else
        {
            // The Top window is shown again only while the main menu is open, as the game shows it.
            g_open = false;
            ShowPageAgain(nullptr);
            SwapHeadings(false);
            RestoreTop();
            g_mgr = nullptr;
            platform::Log(L"Co-op page: the page is gone with the main menu closing; its title and guide put back");
        }
        return;
    }
    if (!menuUp)
    {
        ForceClose(page, L"the main menu closed");
        return;
    }
    std::uint8_t state = 0xFF;
    if (platform::Read(At(page, facts::kOptionsState), state) && (state == facts::kOptionsStateClosing || state == 0))
    {
        Closed(L"seen by the tick");  // closed without the key handler seeing it
    }
}

}  // namespace lss::menu::coop_menu
