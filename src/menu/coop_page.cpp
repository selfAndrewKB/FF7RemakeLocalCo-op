#include "menu/coop_page.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "control/registry.h"
#include "engine_facts.h"
#include "menu/coop_slider.h"
#include "menu/game_ui.h"
#include "menu/help_spacing.h"
#include "platform/entry_redirect.h"
#include "platform/game_array.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/verified_code.h"
#include "players/game_globals.h"
#include "players/game_state.h"
#include "players/weak_object.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
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
using FindIdFn    = std::int32_t* (*)(void* map, std::int32_t* id, std::uint8_t key);
using InfoCtorFn  = void* (*)(void* info);
using InfoDtorFn  = void (*)(void* info);
using InfoMoveFn  = void* (*)(void* to, void* from);
using GrowFn      = void (*)(void* array);
using RemoveAtFn  = void (*)(void* array, std::int32_t index, std::int32_t count);
using SetCountFn  = void (*)(void* box, std::int32_t count);
using RefreshFn   = void (*)(void* box);
using ConstructFn = void (*)(void* page);
using ResetExecuteFn = void (*)(void* confirm, std::int32_t choice);
using ResetIfSafeFn  = bool (*)(void* confirm, std::int32_t choice);

/// The game's functions, found at install.
struct Game
{
    std::uintptr_t     getValueAt = 0, setValueAt = 0;  // where the redirects go
    game_ui::AssignFn  assign     = nullptr;
    CreateFn           create     = nullptr;
    AddToRootFn        addToRoot  = nullptr;
    InRootFn           inRoot     = nullptr;
    FindOrAddFn        findOrAdd  = nullptr;
    FindIdFn           findId     = nullptr;
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
std::vector<rules::Category> g_page;      // the page's categories and rows
std::vector<int>             g_gameRows;  // the options with a row in the game's own Options screen
platform::VTableEntry        g_construct, g_resetExecute, g_resetIfSafe;

// Game thread.
void*         g_class = nullptr;  // MainMenu_OptionsTop2_C, found at the first open
weak::Weak    g_pageWeak;         // the mod's page, kept while alive
bool          g_filled = false;   // the page holds the mod's data (false across a re-add, whose construct puts the game's back,
                                  // and once the page is gone: its address may become System's own page)
bool          g_open   = false;   // the page is open over the hidden Top window
bool          g_failed = false;   // a fault while writing the page: the row opens nothing until the next start
std::uint64_t g_commits = 0;
bool          g_resetting = false;  // a Reset All of Camera & Controls runs: its rows of the mod's read the game's default
weak::Weak    g_lastScreen;         // the last game Options screen built (for the console's reading)
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

/// A screen's category (its FOptionInfos) by key, found with the game's own find, which never adds one; null when the
/// screen has none yet.
void* InfosOf(void* page, std::uint8_t key) noexcept
{
    void* const   map      = At(page, facts::kOptionsItems);
    std::int32_t  id       = -1;
    std::int32_t* found    = nullptr;
    void*         elements = nullptr;
    std::int32_t  count    = 0;
    if (!platform::CallGuarded(g.findId, found, map, &id, key) || found == nullptr || id < 0 || !platform::Read(map, elements) ||
        elements == nullptr || !platform::Read(At(map, facts::kOptionItemsElementCount), count) || id >= count)
    {
        return nullptr;
    }
    return static_cast<std::uint8_t*>(elements) + facts::kOptionItemsElementSize * static_cast<std::size_t>(id) +
           facts::kOptionItemsElementValue;
}

std::wstring CameraRowsText(void* page) noexcept;

/// Logs a game screen's Camera & Controls rows when the game reads one of its values and they differ from the last
/// logged.
void NoteCameraRows(void* page)
{
    static std::wstring last;
    std::wstring        now = CameraRowsText(page);
    if (now != last)
    {
        platform::LogDebug(L"Options screen: a value of Camera & Controls read; its {}", now);
        last = std::move(now);
    }
}

/// The option a row of the game's own Options screen shows, by its category byte: one of the mod's rows there.
std::optional<int> GameRowAt(void* page, std::uint8_t key, std::int32_t row) noexcept
{
    void* const infos = InfosOf(page, key);
    if (infos == nullptr)
    {
        return std::nullopt;
    }
    GameArray    rows;
    std::uint8_t byte = 0;
    if (!platform::Read(At(infos, facts::kOptionInfosRows), rows) || rows.data == nullptr || row < 0 || row >= rows.num ||
        !platform::Read(static_cast<const std::uint8_t*>(rows.data) + facts::kOptionInfoSize * static_cast<std::size_t>(row) +
                            facts::kOptionInfoCategory,
                        byte))
    {
        return std::nullopt;
    }
    const auto option = rules::OptionOf(byte);
    return option && settings::OnGameScreen(*settings::kOptions[static_cast<std::size_t>(*option)]) ? option : std::nullopt;
}

/// The game's own Responsiveness level now (the default, while a Reset All runs: the game writes it before it reads
/// the rows), or none.
std::optional<int> GameResponsiveness() noexcept
{
    void* const  data  = players::game_globals::Object(players::game_globals::Global::DataSingleton);
    std::uint8_t level = 0;
    if (data == nullptr || !platform::Read(static_cast<const std::uint8_t*>(data) + facts::kDataResponsiveness, level) ||
        level < facts::kResponsivenessMin || level > facts::kResponsivenessMax)
    {
        return std::nullopt;
    }
    return level;
}

/// The value a row of the mod's shows: a commit not yet run, else the option's current word (while a Reset All of
/// Camera & Controls runs, a game-screen row shows the game's default instead, which the reset then commits).
int ShownValue(int option, const wchar_t* where)
{
    const auto              i     = static_cast<std::size_t>(option);
    const settings::Option& shown = *settings::kOptions[i];
    const auto              word  = CurrentWord(option);
    const int               first = rules::RowMin(shown);  // an unreadable word shows the first value
    int value = g_queued[i] ? *g_queued[i] : (word ? rules::ShownOf(shown, *word).value_or(first) : first);
    if (g_resetting && settings::OnGameScreen(shown))
    {
        value = GameResponsiveness().value_or(value);
    }
    g_shown[i] = value;
    platform::LogTrace(L"{}: {} shows {} (the option reads {})", where, shown.key, value,
                       word ? *word : std::wstring(L"nothing readable"));
    return value;
}

/// The game's value read: the mod's rows answer their option's word's value (a commit not yet run: its value), on the
/// Co-op page and in Camera & Controls of the game's own screens.
std::int32_t GetValue(void* page, std::uint8_t key, std::int32_t row)
{
    g_answered.reset();  // every read, any page: an answer belongs only to the read just made
    try
    {
        if (g_filled && IsPage(page))
        {
            if (const auto option = OptionAt(key, row))
            {
                const int value = ShownValue(*option, L"Co-op page");
                g_answered      = Answered{*option, value};
                return value;
            }
        }
        else if (key == facts::kOptionsCameraKey && !g_gameRows.empty() && !IsPage(page))
        {
            NoteCameraRows(page);
            if (const auto option = GameRowAt(page, key, row))
            {
                return ShownValue(*option, L"Options screen");
            }
        }
    }
    catch (...)
    {
        return 0;
    }
    return reinterpret_cast<GetValueFn>(g_getValue.Original())(page, key, row);
}

/// Where a row of the option is, for the log.
const wchar_t* WhereOf(const settings::Option& row) noexcept
{
    return settings::OnGameScreen(row) ? L"Options screen" : L"Co-op page";
}

/// The game's value commit: a mod row's value is queued for its option's command, run by the next tick (an unchanged
/// value does nothing: the game re-commits current values at its construct and its Reset All). A Co-op page row commits
/// only while the page is open; a game-screen row from any Options screen.
void* SetValue(void* page, std::uint8_t category, std::int32_t value)
{
    const auto option = rules::OptionOf(category);
    if (!option)
    {
        return reinterpret_cast<SetValueFn>(g_setValue.Original())(page, category, value);
    }
    try
    {
        const auto              i     = static_cast<std::size_t>(*option);
        const settings::Option& row   = *settings::kOptions[i];
        const wchar_t* const    where = WhereOf(row);
        if (!settings::OnGameScreen(row) && (!g_open || !IsPage(page)))
        {
            platform::LogDebug(L"Co-op page: a commit of {} to {} outside the open page; nothing changed", row.key, value);
            return page;
        }
        const auto word = rules::WordAt(row, value);
        if (!word)
        {
            platform::LogWarning(L"{}: {} has no value {}; nothing changed", where, row.key, value);
            return page;
        }
        const auto current = g_queued[i] ? rules::WordAt(row, *g_queued[i]) : CurrentWord(*option);
        if ((current && *current == *word) || (!current && value == g_shown[i]))
        {
            platform::LogDebug(L"{}: {} stays {}", where, row.key, *word);
            return page;
        }
        g_queued[i] = value;
        platform::LogDebug(L"{}: {} to {}, on the next tick", where, row.key, *word);
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
        const auto              word  = rules::WordAt(row, value);
        const control::Entry*   entry = EntryOf(static_cast<int>(i));
        if (!word || entry == nullptr || !entry->set)
        {
            platform::LogWarning(L"{}: {} has no command to take {}; nothing changed", WhereOf(row), row.key, value);
            continue;
        }
        const control::Outcome outcome = entry->set(rules::CommandValue(entry->shape, *word));
        ++g_commits;
        platform::Log(L"{}: {} set to {} ({} {}){}{}", WhereOf(row), row.label,
                      row.wordLabels[static_cast<std::size_t>(value - rules::RowMin(row))], row.key, *word,
                      outcome.ok ? L"" : L"; refused: ", outcome.ok ? std::wstring() : outcome.detail);
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
    // A row in the game's own screen is a Range row as the game's Responsiveness is (its integer shown, no value list);
    // a Co-op page row longer than the game's longest list is the game's slider over its values' places
    // (menu/coop_slider).
    const bool gameScreen = settings::OnGameScreen(row);
    const bool range      = gameScreen || (rules::IsSlider(row) && coop_slider::Ready());
    info[0]               = range ? facts::kOptionKindRange : facts::kOptionKindList;
    info[facts::kOptionInfoCategory] = rules::RowByte(option);
    std::memset(info + facts::kOptionInfoMin, 0, 8);  // Min, Max (the constructor leaves them)
    if (range)
    {
        const auto first = static_cast<std::int32_t>(rules::RowMin(row));
        const auto last  = first + static_cast<std::int32_t>(row.wordLabels.size()) - 1;
        std::memcpy(info + facts::kOptionInfoMin, &first, sizeof first);
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
    for (std::size_t i = 0; !gameScreen && i < row.wordLabels.size(); ++i)
    {
        if (!AppendString(values, row.wordLabels[i]) || !AppendString(helps, row.menuHelp))
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
        // The game's construct only appends rows and finds categories by key (read in IDA),
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

// --- The game's own screens: the game-screen rows in Camera & Controls ------------------------------------------

/// The game-screen rows inserted in Camera & Controls right after the game's Responsiveness, each appended and moved
/// into place (an FOptionInfo is FStrings and arrays: relocatable bitwise, as the engine's TArray moves them).
/// `inserted` counts the rows that went in; nothing changes when the rows already hold one of the mod's (a construct
/// over them again) or no row is Responsiveness. 0 when done, else what failed.
const wchar_t* InsertGameRowsRaw(void* page, std::size_t& inserted)
{
    void* const infos = InfosOf(page, facts::kOptionsCameraKey);
    if (infos == nullptr)
    {
        return L"Camera & Controls is not there yet";
    }
    auto* rows = reinterpret_cast<GameArray*>(At(infos, facts::kOptionInfosRows));
    if (rows->num < 0 || (rows->num > 0 && rows->data == nullptr))
    {
        return L"Camera & Controls' rows are unreadable";
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(rows->num));
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        bytes[i] = static_cast<const std::uint8_t*>(rows->data)[facts::kOptionInfoSize * i + facts::kOptionInfoCategory];
    }
    const auto at = rules::GameRowsAt(bytes, facts::kOptionCategoryResponsiveness);
    if (!at)
    {
        return nullptr;
    }
    std::size_t into = *at;
    for (const int option : g_gameRows)
    {
        if (const wchar_t* failed = AppendRow(rows, option))
        {
            return failed;
        }
        auto* const       data = static_cast<std::uint8_t*>(rows->data);
        const std::size_t last = static_cast<std::size_t>(rows->num) - 1;
        alignas(16) std::uint8_t moved[facts::kOptionInfoSize];
        std::memcpy(moved, data + facts::kOptionInfoSize * last, facts::kOptionInfoSize);
        std::memmove(data + facts::kOptionInfoSize * (into + 1), data + facts::kOptionInfoSize * into,
                     facts::kOptionInfoSize * (last - into));
        std::memcpy(data + facts::kOptionInfoSize * into, moved, facts::kOptionInfoSize);
        ++into;
        ++inserted;
    }
    return nullptr;
}

/// InsertGameRowsRaw under the fault guard, which also stops a C++ exception (an allocation failing) there.
const wchar_t* InsertGameRows(void* page, std::size_t& inserted) noexcept
{
    __try
    {
        return InsertGameRowsRaw(page, inserted);
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return L"a fault while writing them";
    }
}

/// A screen's Camera & Controls rows as their category bytes, for the log: "n rows: b0 b1 ...", or why unread.
std::wstring CameraRowsText(void* page) noexcept
try
{
    void* const infos = InfosOf(page, facts::kOptionsCameraKey);
    if (infos == nullptr)
    {
        return L"Camera & Controls not there";
    }
    GameArray rows;
    if (!platform::Read(At(infos, facts::kOptionInfosRows), rows) || rows.num < 0 || (rows.num > 0 && rows.data == nullptr))
    {
        return L"its rows unreadable";
    }
    std::wstring text = std::format(L"{} rows:", rows.num);
    for (std::int32_t i = 0; i < rows.num && i < 32; ++i)
    {
        std::uint8_t byte = 0;
        const bool   read = platform::Read(static_cast<const std::uint8_t*>(rows.data) +
                                             facts::kOptionInfoSize * static_cast<std::size_t>(i) + facts::kOptionInfoCategory,
                                         byte);
        text += read ? std::format(L" {}", byte) : std::wstring(L" ?");
    }
    return text;
}
catch (...)
{
    return L"unreadable (an exception)";
}

/// The Options screen's construct: the game's, then the game-screen rows on any screen but the mod's own page (whose
/// data the mod writes whole).
void Construct(void* page)
{
    reinterpret_cast<ConstructFn>(g_construct.Original())(page);
    if (g_gameRows.empty() || IsPage(page))
    {
        return;
    }
    g_lastScreen = weak::MakeWeak(page);
    platform::LogDebug(L"Options screen: built, Camera & Controls {}", CameraRowsText(page));
    try
    {
        std::size_t        inserted = 0;
        const wchar_t*     failed   = InsertGameRows(page, inserted);
        std::uint8_t       inGame   = 0;
        const bool         known    = platform::Read(At(page, facts::kOptionsInGame), inGame);
        const wchar_t*     screen   = !known ? L"an" : inGame != 0 ? L"the main menu's" : L"the title's";
        if (failed != nullptr)
        {
            platform::LogWarning(L"Options screen: {} of players 2 and 3's {} rows added to {} Options screen ({}); the rest stay "
                                 L"on the console and the settings file",
                                 inserted, g_gameRows.size(), screen, failed);
        }
        else if (inserted != 0)
        {
            platform::Log(L"Options screen: {} rows of the mod's added to {} Options screen, after the game's Responsiveness ({})",
                          inserted, screen, CameraRowsText(page));
        }
        else
        {
            platform::LogDebug(L"Options screen: {} Options screen built again; its rows of the mod's are already there (or it "
                               L"has no Responsiveness row)",
                               screen);
        }
    }
    catch (...)
    {
        platform::LogWarning(L"Options screen: adding the mod's rows raised an exception");
    }
}

/// A Reset All confirmed (`choice` 0) on Camera & Controls: the page's current category, read from the confirmation's
/// page as the game's reset reads it.
bool ResetsCameraRows(void* confirm, std::int32_t choice) noexcept
{
    void*        page  = nullptr;
    std::int32_t index = -1;
    GameArray    order;
    std::uint8_t key   = 0;
    return choice == 0 && !g_gameRows.empty() && platform::Read(At(confirm, facts::kResetConfirmPage), page) &&
           page != nullptr && !IsPage(page) && platform::Read(At(page, facts::kOptionsCategoryIndex), index) &&
           platform::Read(At(page, facts::kOptionsOrder), order) && order.data != nullptr && index >= 0 && index < order.num &&
           platform::Read(static_cast<const std::uint8_t*>(order.data) + index, key) && key == facts::kOptionsCameraKey;
}

/// The game's Reset All with the game-screen rows reading the game's default: the reset writes the defaults, then reads
/// each row's value and commits it, so those rows commit the default too.
class ResettingScope
{
public:
    explicit ResettingScope(bool on) noexcept : on_(on)
    {
        if (on_)
        {
            g_resetting = true;
            try
            {
                platform::Log(L"Options screen: Reset All on Camera & Controls; players 2 and 3's Responsiveness take the game's "
                              L"default too");
            }
            catch (...)
            {
            }
        }
    }
    ~ResettingScope()
    {
        if (on_)
        {
            g_resetting = false;
        }
    }
    ResettingScope(const ResettingScope&)            = delete;
    ResettingScope& operator=(const ResettingScope&) = delete;

private:
    bool on_ = false;
};

void ResetExecute(void* confirm, std::int32_t choice)
{
    const ResettingScope resetting(ResetsCameraRows(confirm, choice));
    reinterpret_cast<ResetExecuteFn>(g_resetExecute.Original())(confirm, choice);
}

bool ResetIfSafe(void* confirm, std::int32_t choice)
{
    const ResettingScope resetting(ResetsCameraRows(confirm, choice));
    return reinterpret_cast<ResetIfSafeFn>(g_resetIfSafe.Original())(confirm, choice);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
try
{
    namespace f = facts;
    help_spacing::Install(image);  // without it, a blank line in a help keeps the game's whole-line spacing
    const auto getValue  = platform::FindFunction(image, f::kOptionsGetValue);
    const auto setValue  = platform::FindFunction(image, f::kOptionsSetValue);
    const auto create    = platform::FindFunction(image, f::kOptionsCreate);
    const auto addToRoot = platform::FindFunction(image, f::kAddToRootOverlayFn);
    const auto inRoot    = platform::FindFunction(image, f::kIsInRootOverlay);
    const auto findOrAdd = platform::FindFunction(image, f::kOptionItemsFindOrAdd);
    const auto findId    = platform::FindFunction(image, f::kOptionItemsFindId);
    const auto infoCtor  = platform::FindFunction(image, f::kOptionInfoCtor);
    const auto infoDtor  = platform::FindFunction(image, f::kOptionInfoDtor);
    const auto infoMove  = platform::FindFunction(image, f::kOptionInfoMove);
    const auto growInfos = platform::FindFunction(image, f::kOptionInfosGrow);
    const auto growTexts = platform::FindFunction(image, f::kStringArrayGrow);
    const auto removeAt  = platform::FindFunction(image, f::kOptionInfosRemoveAt);
    const auto setCount  = platform::FindFunction(image, f::kScrollBoxSetCount);
    const auto refresh   = platform::FindFunction(image, f::kScrollBoxRefresh);
    g_page               = rules::BuildPage();
    g_gameRows           = rules::GameScreenOptions();
    // game_ui is installed first (menu/coop_menu): the page writes its strings with the game's copy.
    if (!getValue || !setValue || !create || !addToRoot || !inRoot || !findOrAdd || !findId || !infoCtor || !infoDtor || !infoMove ||
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
    g.findId     = reinterpret_cast<FindIdFn>(*findId);
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

bool Place(const platform::GameImage& image) noexcept
{
    const bool values =
        g_getValue.Install(g.getValueAt, std::span(facts::kOptionsGetValueEntry.data(), facts::kRedirectFirstBytes),
                           reinterpret_cast<std::uintptr_t>(&GetValue)) &&
        g_setValue.Install(g.setValueAt, std::span(facts::kOptionsSetValueEntry.data(), facts::kRedirectFirstBytes),
                           reinterpret_cast<std::uintptr_t>(&SetValue));
    if (!values)
    {
        return false;
    }
    // The game-screen rows are their own: without the construct's slot they are not added (the settings file and the
    // console still set them); without the confirmation's slots a Reset All leaves them as they are.
    if (!g_gameRows.empty())
    {
        if (!platform::ReplaceSlot(image, facts::kOptionsConstruct, reinterpret_cast<std::uintptr_t>(&Construct), g_construct))
        {
            platform::LogWarning(L"Options screen: its construct could not be redirected; players 2 and 3's rows are not added");
        }
        else if (!platform::ReplaceSlot(image, facts::kResetConfirmExecute, reinterpret_cast<std::uintptr_t>(&ResetExecute),
                                        g_resetExecute) ||
                 !platform::ReplaceSlot(image, facts::kResetConfirmExecuteIfSafe,
                                        reinterpret_cast<std::uintptr_t>(&ResetIfSafe), g_resetIfSafe))
        {
            g_resetIfSafe.Restore();
            g_resetExecute.Restore();
            platform::LogWarning(L"Options screen: Reset All's confirmation could not be redirected; it leaves players 2 and 3's "
                                 L"rows as they are");
        }
    }
    return true;
}

void Unplace() noexcept
{
    g_resetIfSafe.Restore();
    g_resetExecute.Restore();
    g_construct.Restore();
    g_setValue.Restore();
    g_getValue.Restore();
}

const std::vector<coop_menu_rules::Category>& Categories() noexcept
{
    return g_page;
}

void* Ready(void* host, std::int32_t z, std::uint8_t inGame) noexcept
{
    void*        page     = weak::Resolve(g_pageWeak);
    std::uint8_t madeFor  = inGame;
    if (page != nullptr && platform::Read(At(page, facts::kOptionsInGame), madeFor) && madeFor != inGame)
    {
        // Closed, so out of the overlay and held by nothing: the collector takes it.
        platform::LogDebug(L"Co-op page: the page made for the {} is left; a new one for the {}", madeFor != 0 ? L"main menu" : L"title",
                           inGame != 0 ? L"main menu" : L"title");
        g_pageWeak = {};
        g_filled   = false;
        page       = nullptr;
    }
    if (page == nullptr)
    {
        if (g_class == nullptr)
        {
            g_class = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(nullptr, nullptr, facts::kOptionsClassPath);
        }
        void* const world = players::game_state::WorldOf(host);
        if (g_class == nullptr || world == nullptr ||
            !platform::CallGuarded(g.create, page, world, g_class, inGame) || page == nullptr)
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

std::wstring GameScreenReading()
{
    void* const screen = weak::Resolve(g_lastScreen);
    return screen != nullptr ? std::format(L"the last game Options screen built: Camera & Controls {}", CameraRowsText(screen))
                             : std::wstring(L"no game Options screen alive");
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
    if (!open)
    {
        help_spacing::Restore();  // every close passes here: the help bar's own spacing back
    }
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
    if (g_open)
    {
        help_spacing::Tick();
    }
}

}  // namespace lss::menu::coop_page
