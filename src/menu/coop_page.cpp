#include "menu/coop_page.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "control/registry.h"
#include "engine_facts.h"
#include "menu/coop_slider.h"
#include "menu/game_ui.h"
#include "platform/entry_redirect.h"
#include "platform/game_array.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/verified_code.h"
#include "players/weak_object.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lss::menu::coop_page {

namespace {

namespace rules    = coop_menu_rules;
namespace settings = platform::settings;
namespace weak     = players::weak_object;

using game_ui::At;
using platform::GameArray;
using platform::SourceString;

using GetValueFn  = std::int32_t (*)(void* page, std::uint8_t key, std::int32_t row);
using SetValueFn  = void* (*)(void* page, std::uint8_t category, std::int32_t value);
using CreateFn    = void* (*)(void* world, void* cls, std::uint8_t inGame);
using AddToRootFn = bool (*)(void* widget, std::int32_t zOrder);
using InRootFn    = bool (*)(void* widget);
using FindOrAddFn = void* (*)(void* map, const std::uint8_t* key);
using InfoCtorFn  = void* (*)(void* info);
using InfoDtorFn  = void (*)(void* info);
using InfoMoveFn  = void* (*)(void* to, void* from);
using GrowFn      = void (*)(void* array);
using RemoveAtFn  = void (*)(void* array, std::int32_t index, std::int32_t count);
using SetCountFn  = void (*)(void* box, std::int32_t count);
using RefreshFn   = void (*)(void* box);
using GetWorldFn  = void* (*)(void* object);

/// The game's functions, found at install.
struct Game
{
    std::uintptr_t     getValueAt = 0, setValueAt = 0;  // where the redirects go
    game_ui::AssignFn  assign     = nullptr;
    CreateFn           create     = nullptr;
    AddToRootFn        addToRoot  = nullptr;
    InRootFn           inRoot     = nullptr;
    FindOrAddFn        findOrAdd  = nullptr;
    InfoCtorFn         infoCtor   = nullptr;
    InfoDtorFn         infoDtor   = nullptr;
    InfoMoveFn         infoMove   = nullptr;
    GrowFn             growInfos  = nullptr;
    GrowFn             growTexts  = nullptr;
    RemoveAtFn         removeAt   = nullptr;
    SetCountFn         setCount   = nullptr;
    RefreshFn          refresh    = nullptr;
};

// Set at install and on the first tick, then only read.
Game                         g;
platform::EntryRedirect      g_getValue, g_setValue;
std::vector<rules::Category> g_page;  // the page's categories and rows

// Game thread.
void*         g_class = nullptr;  // MainMenu_OptionsTop2_C, found at the first open
weak::Weak    g_pageWeak;         // the mod's page, kept while alive
bool          g_filled = false;   // the page holds the mod's data (false across a re-add, whose construct puts the game's back,
                                  // and once the page is gone: its address may become System's own page)
bool          g_open   = false;   // the page is open over the hidden Top window
bool          g_failed = false;   // a fault while writing the page: the row opens nothing until the next start
std::uint64_t g_commits = 0;
/// A value the page committed, run by the next tick (an option's command may resize the window: never inside the
/// page's key event). The last commit per option wins.
std::array<std::optional<int>, settings::kOptions.size()> g_queued{};
/// The value the page last showed per option (a commit equal to it changes nothing when the current word is unknown).
std::array<int, settings::kOptions.size()> g_shown{};
/// The option and value the page's value read last answered, for a slider row's text (menu/coop_slider), taken once.
struct Answered
{
    int          option = -1;
    std::int32_t value  = 0;
};
std::optional<Answered> g_answered;

// --- Values: the settings table's options through their console commands ------------------------------------------

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
    g_answered.reset();  // every read, any page: an answer belongs only to the read just made
    if (g_filled && IsPage(page))
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
                g_answered      = Answered{*option, value};
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
        if (!g_open || !IsPage(page))
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
        if (outcome.ok)
        {
            settings::Save(row, *word);  // the next start reads the choice (logged either way)
        }
    }
}
catch (...)
{
    platform::LogWarning(L"Co-op page: running a change raised an exception; the rest waits for the next tick");
}

// --- Writing the page -------------------------------------------------------------------------------------------

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
    const GameArray source = SourceString(text);
    g.assign(element, &source);
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
    // A row longer than the game's longest list is the game's slider over its values' places (menu/coop_slider).
    const bool slider = rules::IsSlider(row) && coop_slider::Ready();
    info[0]           = slider ? facts::kOptionKindRange : facts::kOptionKindList;
    info[facts::kOptionInfoCategory] = rules::RowByte(option);
    std::memset(info + facts::kOptionInfoMin, 0, 8);  // Min, Max (the constructor leaves them)
    if (slider)
    {
        const auto last = static_cast<std::int32_t>(row.wordLabels.size()) - 1;
        std::memcpy(info + facts::kOptionInfoMax, &last, sizeof last);
    }
    const GameArray text = SourceString(row.label);
    const GameArray help = SourceString(row.menuHelp);
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
        const GameArray label = SourceString(category.text.label);
        g.assign(static_cast<std::uint8_t*>(labels->data) + 16 * static_cast<std::size_t>(category.key), &label);
        void* const infos = g.findOrAdd(At(page, facts::kOptionsItems), &category.key);
        if (infos == nullptr)
        {
            return L"a category's rows could not be found";
        }
        const GameArray help = SourceString(category.text.help);
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

}  // namespace

bool Install(const platform::GameImage& image) noexcept
try
{
    namespace f = facts;
    const auto getValue  = platform::FindFunction(image, f::kOptionsGetValue);
    const auto setValue  = platform::FindFunction(image, f::kOptionsSetValue);
    const auto create    = platform::FindFunction(image, f::kOptionsCreate);
    const auto addToRoot = platform::FindFunction(image, f::kAddToRootOverlayFn);
    const auto inRoot    = platform::FindFunction(image, f::kIsInRootOverlay);
    const auto findOrAdd = platform::FindFunction(image, f::kOptionItemsFindOrAdd);
    const auto infoCtor  = platform::FindFunction(image, f::kOptionInfoCtor);
    const auto infoDtor  = platform::FindFunction(image, f::kOptionInfoDtor);
    const auto infoMove  = platform::FindFunction(image, f::kOptionInfoMove);
    const auto growInfos = platform::FindFunction(image, f::kOptionInfosGrow);
    const auto growTexts = platform::FindFunction(image, f::kStringArrayGrow);
    const auto removeAt  = platform::FindFunction(image, f::kOptionInfosRemoveAt);
    const auto setCount  = platform::FindFunction(image, f::kScrollBoxSetCount);
    const auto refresh   = platform::FindFunction(image, f::kScrollBoxRefresh);
    g_page               = rules::BuildPage();
    // game_ui is installed first (menu/coop_menu): the page writes its strings with the game's copy.
    if (!getValue || !setValue || !create || !addToRoot || !inRoot || !findOrAdd || !infoCtor || !infoDtor || !infoMove ||
        !growInfos || !growTexts || !removeAt || !setCount || !refresh || game_ui::Assign() == nullptr || g_page.empty())
    {
        return false;
    }
    g.getValueAt = *getValue;
    g.setValueAt = *setValue;
    g.assign     = game_ui::Assign();
    g.create     = reinterpret_cast<CreateFn>(*create);
    g.addToRoot  = reinterpret_cast<AddToRootFn>(*addToRoot);
    g.inRoot     = reinterpret_cast<InRootFn>(*inRoot);
    g.findOrAdd  = reinterpret_cast<FindOrAddFn>(*findOrAdd);
    g.infoCtor   = reinterpret_cast<InfoCtorFn>(*infoCtor);
    g.infoDtor   = reinterpret_cast<InfoDtorFn>(*infoDtor);
    g.infoMove   = reinterpret_cast<InfoMoveFn>(*infoMove);
    g.growInfos  = reinterpret_cast<GrowFn>(*growInfos);
    g.growTexts  = reinterpret_cast<GrowFn>(*growTexts);
    g.removeAt   = reinterpret_cast<RemoveAtFn>(*removeAt);
    g.setCount   = reinterpret_cast<SetCountFn>(*setCount);
    g.refresh    = reinterpret_cast<RefreshFn>(*refresh);
    return true;
}
catch (...)
{
    return false;
}

bool Place() noexcept
{
    return g_getValue.Install(g.getValueAt, std::span(facts::kOptionsGetValueEntry.data(), facts::kRedirectFirstBytes),
                              reinterpret_cast<std::uintptr_t>(&GetValue)) &&
           g_setValue.Install(g.setValueAt, std::span(facts::kOptionsSetValueEntry.data(), facts::kRedirectFirstBytes),
                              reinterpret_cast<std::uintptr_t>(&SetValue));
}

void Unplace() noexcept
{
    g_setValue.Restore();
    g_getValue.Restore();
}

const std::vector<coop_menu_rules::Category>& Categories() noexcept
{
    return g_page;
}

void* Ready(void* top) noexcept
{
    void* page = weak::Resolve(g_pageWeak);
    if (page == nullptr)
    {
        if (g_class == nullptr)
        {
            g_class = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(nullptr, nullptr, facts::kOptionsClassPath);
        }
        void* world = nullptr;
        const auto getWorld =
            reinterpret_cast<GetWorldFn>(platform::ReadPointer(platform::ReadPointer(top, 0), facts::kObjectGetWorldSlot));
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

void* Page() noexcept
{
    return weak::Resolve(g_pageWeak);
}

std::optional<int> TakeAnswered(std::int32_t value) noexcept
{
    const auto answered = g_answered;
    g_answered.reset();
    return answered && answered->value == value ? std::optional<int>(answered->option) : std::nullopt;
}

bool IsPage(const void* object) noexcept
{
    return object != nullptr && object == weak::Resolve(g_pageWeak);
}

bool IsOpen() noexcept
{
    return g_open;
}

void SetOpen(bool open) noexcept
{
    g_open = open;
}

bool Failed() noexcept
{
    return g_failed;
}

std::uint64_t Commits() noexcept
{
    return g_commits;
}

void Tick() noexcept
{
    if (weak::Resolve(g_pageWeak) == nullptr)
    {
        g_filled = false;  // gone: its address may become another page's
    }
    RunQueued();
}

}  // namespace lss::menu::coop_page
