#include "menu/coop_pause.h"

#include "engine_facts.h"
#include "menu/coop_menu_rules.h"
#include "menu/coop_page.h"
#include "menu/coop_page_open.h"
#include "menu/game_ui.h"
#include "platform/call_redirect.h"
#include "platform/game_array.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/game_state.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace lss::menu::coop_pause {

namespace {

namespace rules = coop_menu_rules;

using game_ui::At;
using platform::GameArray;

using OpenModeFn = std::uint64_t (*)(void* mgr, void* world, std::uint8_t mode);
using SetRowsFn  = void* (*)(void* widget, const GameArray* labels);
using PressedFn  = std::uint64_t (*)(void* mgr, void* reply, std::uint64_t index, std::uint64_t extra);
using CloseFn    = std::uint64_t (*)(void* mgr);
using GrowFn     = void (*)(GameArray* array);
using PauseOffFn = char (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

/// A row of the pause manager's table.
struct Row
{
    std::int32_t   kind;
    std::int32_t   pad;
    const wchar_t* text;
};
static_assert(sizeof(Row) == facts::kPauseRowBytes);

/// The game's functions and the call sites, found at install.
struct Game
{
    OpenModeFn     openMode = nullptr;
    SetRowsFn      setRows  = nullptr;
    PressedFn      pressed  = nullptr;
    CloseFn        close    = nullptr;
    GrowFn         grow     = nullptr;
    PauseOffFn     pauseOff = nullptr;
    std::uintptr_t openSite = 0;
    std::array<std::uintptr_t, facts::kPauseRowPressedCalls.size()> pressSites{};
};
Game g;

platform::CallRedirect                                         g_openCall;
std::array<platform::CallRedirect, facts::kPauseRowPressedCalls.size()> g_pressCalls;

// Game thread.
std::uint8_t g_mode = facts::kPauseModeBattle;  // the pause's mode when the page opened over it
bool         g_saidAdded = false;

constexpr std::size_t kRowsMax = 12;  // the battle pause builds at most five

using Labels = std::array<GameArray, kRowsMax>;

/// Step 1, reads only: where the Co-op row goes (above Return to Title, else last) and the labels of the rows with it
/// in. False if the table cannot take it (or holds it already, or a read faulted). Raw: no objects to unwind.
bool PlanRaw(void* mgr, std::int32_t& at, Labels& labels, std::int32_t& count) noexcept
{
    __try
    {
        const auto* const rows = reinterpret_cast<const GameArray*>(At(mgr, facts::kPauseMgrRows));
        if (rows->data == nullptr || rows->num <= 0 || rows->num + 1 > static_cast<std::int32_t>(kRowsMax))
        {
            return false;
        }
        const auto* const table = static_cast<const Row*>(rows->data);
        at = rows->num;
        for (std::int32_t i = 0; i < rows->num; ++i)
        {
            if (table[i].kind == facts::kPauseRowCoop)
            {
                return false;
            }
            if (table[i].kind == facts::kPauseRowTitle && at == rows->num)
            {
                at = i;
            }
        }
        count = rows->num + 1;
        for (std::int32_t i = 0, from = 0; i < count; ++i)
        {
            labels[static_cast<std::size_t>(i)] = platform::SourceString(i == at ? rules::kRowLabel : table[from++].text);
        }
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// Step 2: the row inserted at `at`, the table grown first as the game grows it (its growth sizes from the count). False,
/// the table as it was, if there is no room.
bool InsertRaw(void* mgr, std::int32_t at) noexcept
{
    __try
    {
        auto* const rows = reinterpret_cast<GameArray*>(At(mgr, facts::kPauseMgrRows));
        if (rows->num + 1 > rows->max)
        {
            ++rows->num;
            g.grow(rows);
            --rows->num;
        }
        if (rows->num + 1 > rows->max || rows->data == nullptr)
        {
            return false;
        }
        auto* const table = static_cast<Row*>(rows->data);
        std::memmove(table + at + 1, table + at, sizeof(Row) * static_cast<std::size_t>(rows->num - at));
        table[at] = Row{facts::kPauseRowCoop, 0, rules::kRowLabel};
        ++rows->num;
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// Step 3 undone: the row at `at` taken out again, so the table matches the labels the widget still shows.
void RemoveRaw(void* mgr, std::int32_t at) noexcept
{
    __try
    {
        auto* const rows  = reinterpret_cast<GameArray*>(At(mgr, facts::kPauseMgrRows));
        auto* const table = static_cast<Row*>(rows->data);
        std::memmove(table + at, table + at + 1, sizeof(Row) * static_cast<std::size_t>(rows->num - at - 1));
        --rows->num;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
    }
}

/// The Co-op row in the table and its label in the widget, together or neither: the labels planned first, then the
/// insert, then the game's setter; a setter that faults has the insert undone, so each label keeps its own row's press.
bool AddRow(void* mgr, void* widget) noexcept
{
    std::int32_t at = 0, count = 0;
    Labels       labels{};
    if (!PlanRaw(mgr, at, labels, count) || !InsertRaw(mgr, at))
    {
        return false;
    }
    const GameArray list{labels.data(), count, count};
    void*           ignored = nullptr;
    if (!platform::CallGuarded(g.setRows, ignored, widget, &list))
    {
        RemoveRaw(mgr, at);
        platform::LogWarning(L"Co-op pause: the battle pause's labels could not be set; its rows are the game's");
        return false;
    }
    return true;
}

/// The game's pause open, then the Co-op row in a battle pause.
std::uint64_t OpenMode(void* mgr, void* world, std::uint8_t mode)
{
    const std::uint64_t result = g.openMode(mgr, world, mode);
    std::uint8_t        now    = 0;
    void* const         widget = mgr != nullptr ? game_ui::WeakAt(mgr, facts::kPauseMgrWidget) : nullptr;
    if (widget != nullptr && platform::Read(At(mgr, facts::kPauseMgrMode), now) && now == facts::kPauseModeBattle &&
        AddRow(mgr, widget) && !g_saidAdded)
    {
        g_saidAdded = true;
        platform::LogDebug(L"Co-op pause: the Co-op row added to the battle pause (above Return to Title)");
    }
    return result;
}

/// The page's moment lasts while the game stays in its menu state and no retry is loading.
bool StillUp() noexcept
{
    if (players::game_state::Current().state != facts::kGameStateMenu)
    {
        return false;
    }
    std::int32_t state = 0;
    void* const  mgr   = game_ui::Manager(facts::kUiPauseMenuSlot);
    // An unreadable manager keeps the moment: the page stays usable, and Back resumes the game if no pause comes back.
    return mgr == nullptr || !platform::Read(At(mgr, facts::kPauseMgrState), state) || (state != 1 && state != 2);
}

/// The battle pause again after the page: the game's own open (through the redirect, so the row is there); if it cannot
/// be, the game resumes as Resume does, never left paused with no menu.
bool Back() noexcept
{
    void* const   mgr   = game_ui::Manager(facts::kUiPauseMenuSlot);
    void* const   world = players::game_state::World();
    std::uint64_t ignored = 0;
    std::int32_t  state = -1;
    std::uint8_t  mode  = 0xFF;
    // Only a whole open leaves the manager idle (the close left it closing, or the tick took the widget), in this mode,
    // with a widget.
    if (mgr != nullptr && world != nullptr && platform::CallGuarded(&OpenMode, ignored, mgr, world, g_mode) &&
        platform::Read(At(mgr, facts::kPauseMgrState), state) && state == 0 && platform::Read(At(mgr, facts::kPauseMgrMode), mode) &&
        mode == g_mode && game_ui::WeakAt(mgr, facts::kPauseMgrWidget) != nullptr)
    {
        return true;
    }
    char resumed = 0;
    platform::CallGuarded(g.pauseOff, resumed, std::uint64_t{0}, std::uint64_t{0}, std::uint64_t{0}, std::uint64_t{0});
    platform::LogWarning(L"Co-op pause: the battle pause could not be opened again; the game resumes as Resume does");
    return false;
}

constexpr coop_page_open::Host kHost{L"the battle pause", &StillUp, &Back};

/// The Co-op row's press: the pause down the game's way, the page over its moment.
void OpenPage(void* mgr) noexcept
{
    void* const pause = game_ui::WeakAt(mgr, facts::kPauseMgrWidget);
    if (pause == nullptr || game_ui::WeakAt(mgr, facts::kPauseMgrConfirm) != nullptr || coop_page::IsOpen() || coop_page::Failed())
    {
        platform::LogDebug(L"Co-op pause: the Co-op row's press opens nothing ({})",
                           pause == nullptr                                              ? L"the pause is gone"
                           : game_ui::WeakAt(mgr, facts::kPauseMgrConfirm) != nullptr ? L"a confirm window is up"
                           : coop_page::IsOpen()                                        ? L"the page is open"
                                                                                        : L"the page failed earlier");
        return;
    }
    platform::Read(At(mgr, facts::kPauseMgrMode), g_mode);
    void* const page = coop_page::Ready(pause);  // the pause's world and ZOrder; added to the overlay before the pause goes
    if (page == nullptr)
    {
        platform::LogDebug(L"Co-op pause: the Co-op row's press opens nothing (the page could not be made)");
        return;
    }
    std::uint64_t ignored = 0;
    if (!platform::CallGuarded(g.close, ignored, mgr))
    {
        platform::LogWarning(L"Co-op pause: the battle pause could not be closed; the page is not opened");
        return;
    }
    if (!coop_page_open::OpenOver(page, kHost))
    {
        Back();
    }
}

/// A row's press (both of the list delegate's invokers): the game's, then the page for the Co-op row.
std::uint64_t Pressed(void* mgr, void* reply, std::uint64_t index, std::uint64_t extra)
{
    bool         coop  = false;
    std::int32_t state = -1;
    GameArray    rows;
    const auto   i = static_cast<std::int32_t>(index);
    if (mgr != nullptr && platform::Read(At(mgr, facts::kPauseMgrState), state) && state == 0 &&
        platform::Read(At(mgr, facts::kPauseMgrRows), rows) && rows.data != nullptr && i >= 0 && i < rows.num)
    {
        std::int32_t kind = -1;
        coop = platform::Read(static_cast<const std::uint8_t*>(rows.data) + facts::kPauseRowBytes * static_cast<std::size_t>(i), kind) &&
               kind == facts::kPauseRowCoop;
    }
    const std::uint64_t result = g.pressed(mgr, reply, index, extra);  // a kind above 9: the game does nothing
    if (coop)
    {
        OpenPage(mgr);
    }
    return result;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    namespace f = facts;
    const auto openMode = platform::FindFunction(image, f::kPauseOpenMode);
    const auto setRows  = platform::FindFunction(image, f::kPauseSetRows);
    const auto pressed  = platform::FindFunction(image, f::kPauseRowPressed);
    const auto close    = platform::FindFunction(image, f::kPauseClose);
    const auto grow     = platform::FindFunction(image, f::kStringArrayGrow);  // the 16-byte element grow the pause uses
    const auto pauseOff = platform::FindFunction(image, f::kPauseOff);
    const auto openSite = image.Rebase(f::kPauseOpenModeCall, 5);
    if (!openMode || !setRows || !pressed || !close || !grow || !pauseOff || !openSite)
    {
        return false;
    }
    for (std::size_t i = 0; i < f::kPauseRowPressedCalls.size(); ++i)
    {
        const auto site = image.Rebase(f::kPauseRowPressedCalls[i], 5);
        if (!site)
        {
            return false;
        }
        g.pressSites[i] = *site;
    }
    g.openMode = reinterpret_cast<OpenModeFn>(*openMode);
    g.setRows  = reinterpret_cast<SetRowsFn>(*setRows);
    g.pressed  = reinterpret_cast<PressedFn>(*pressed);
    g.close    = reinterpret_cast<CloseFn>(*close);
    g.grow     = reinterpret_cast<GrowFn>(*grow);
    g.pauseOff = reinterpret_cast<PauseOffFn>(*pauseOff);
    g.openSite = *openSite;
    return true;
}

bool Place() noexcept
{
    // The presses first, then the open (the row only shows once its press opens the page).
    bool placed = true;
    for (std::size_t i = 0; placed && i < g_pressCalls.size(); ++i)
    {
        placed = g_pressCalls[i].Install(g.pressSites[i], reinterpret_cast<std::uintptr_t>(g.pressed),
                                         reinterpret_cast<std::uintptr_t>(&Pressed));
    }
    placed = placed && g_openCall.Install(g.openSite, reinterpret_cast<std::uintptr_t>(g.openMode),
                                          reinterpret_cast<std::uintptr_t>(&OpenMode));
    if (!placed)
    {
        Unplace();
        platform::Log(L"Co-op pause: the battle pause's open or press could not be redirected; no Co-op row in a fight");
        return false;
    }
    platform::Log(L"Co-op pause: a Co-op row in the battle pause, above Return to Title");
    return true;
}

void Unplace() noexcept
{
    g_openCall.Restore();
    for (platform::CallRedirect& call : g_pressCalls)
    {
        call.Restore();
    }
}

}  // namespace lss::menu::coop_pause
