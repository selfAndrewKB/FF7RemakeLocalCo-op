#include "menu/coop_page_open.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/UObject.hpp>

#include "engine_facts.h"
#include "menu/coop_menu_rules.h"
#include "menu/coop_page.h"
#include "menu/game_ui.h"
#include "menu/help_spacing.h"
#include "platform/game_thread.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/game_globals.h"
#include "players/weak_object.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::menu::coop_page_open {

namespace {

namespace rules    = coop_menu_rules;
namespace settings = platform::settings;
namespace weak     = players::weak_object;

using game_ui::At;

using ShowTopFn       = void* (*)(void* mgr, bool restoreCursor);
using KeyDownFn       = std::uintptr_t (*)(void* page, void* a, void* b, void* c);
using OpenFn          = void (*)(void* page, std::uint8_t startCategory);
using RemoveFn        = void* (*)(void* widget);
using TakeWidgetFn    = void* (*)(void* widget, void* out);
using PopLayerFn      = void (*)(void* ui, void* taken);
using ObjectFn        = void* (*)(void* object);  // a window's Hide
using SetVisibilityFn = void (*)(void* widget, std::uint8_t visibility);

// The guide while the page is open: the page's own keys (the Top window's guide shows whatever has focus).
constexpr const wchar_t* kGuide = L"<button=decide> Choose  <button=cancel> Back";

/// The game's functions, found at install.
struct Game
{
    ShowTopFn       showTop       = nullptr;
    OpenFn          open          = nullptr;
    RemoveFn        remove        = nullptr;
    TakeWidgetFn    takeWidget    = nullptr;
    PopLayerFn      popLayer      = nullptr;
    SetVisibilityFn setVisibility = nullptr;  // the dev switch menu.coop-page-hidden
    std::uintptr_t  headings      = 0;        // g_OptionsMenu_CategoryHeadingIds
    std::array<std::uintptr_t, facts::kOptionsHeadingsWere.size()> headingsWere{};
};

using TitleCallFn   = void (*)(void* start);       // the start menu's Options open, its frame's
using ClosedFn      = void (*)(const void* capture);
using SetStickFn    = void (*)(void* input, std::uint8_t on);
using SetTextFn     = void (*)(void* block, const wchar_t* text);

/// The title's functions, found at install (menu/title_row).
struct TitleGame
{
    TitleCallFn openOptions = nullptr;
    TitleCallFn ensureFrame = nullptr;
    ClosedFn    closed      = nullptr;
    SetStickFn  setStick    = nullptr;
    SetTextFn   setText     = nullptr;
};

/// What the game's close function for the title holds: the start menu and a weak pointer to it.
struct TitleCapture
{
    void*      start = nullptr;
    weak::Weak weak;
};
static_assert(sizeof(TitleCapture) == 16 && offsetof(TitleCapture, weak) == 8, "read raw by the game's close function");

// Set at install and on the first tick, then only read.
Game                  g;
TitleGame             g_title;
platform::VTableEntry g_keyDown;
/// The page's categories' entries of the game's heading table, swapped for their labels while the page is open (the
/// game's own Options page reads the same table).
std::array<platform::VTableEntry, rules::kCategoryKeys.size()> g_headings;

// Game thread.
platform::GameThreadMark g_gameThread;  // set by Tick; only on the game thread may the hold be let go
void*         g_mgr = nullptr;  // the main menu manager while open
bool          g_atTitle = false;  // the page is open over the title menu, not the main menu
weak::Weak    g_titleHost;        // the start menu it opened over
weak::Weak    g_topWeak;
void*         g_heldTop = nullptr;  // the Top window the mod put on the engine's root set while the page is open (alive
                                    // while held: the collector never frees a root)
std::wstring  g_savedTitle, g_savedGuide;
std::uint64_t g_opens          = 0;
int           g_pageVisibility = -1;  // the page's own visibility while the dev switch hides it, else -1

/// The heading table's entries for the page's categories: their labels while the page is open, the game's after.
void SwapHeadings(bool labels) noexcept
{
    const auto& page = coop_page::Categories();
    for (std::size_t i = 0; i < page.size() && i < g_headings.size(); ++i)
    {
        const std::uint8_t key = page[i].key;
        if (key >= g.headingsWere.size())
        {
            continue;
        }
        if (!labels)
        {
            g_headings[i].Restore();
        }
        else if (!g_headings[i].Active() &&
                 !g_headings[i].Replace(g.headings + 8 * key, g.headingsWere[key], reinterpret_cast<std::uintptr_t>(page[i].text.label)))
        {
            platform::LogWarning(L"Co-op page: the heading of {} could not be set; it shows the game's", page[i].text.label);
        }
    }
}

/// The engine's root-set flag on `object` (UObject::AddToRoot's flag, through UE4SS's object array): set, cleared or
/// read. Guarded: false if the object array faulted. Raw: no objects to unwind.
bool RootGuarded(void* object, bool set) noexcept
{
    __try
    {
        auto* const o = static_cast<RC::Unreal::UObject*>(object);
        set ? o->SetRootSet() : o->ClearRootSet();
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool IsRootGuarded(void* object, bool& rooted) noexcept
{
    __try
    {
        rooted = static_cast<RC::Unreal::UObject*>(object)->IsRootSet();
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

void ReleaseTop() noexcept;

/// The hidden Top window kept from the garbage collector while the page is open (research/RE-game-widgets.md):
/// Top's Hide takes it out of the overlay, nothing else holds it, and the manager's tick empties the title, help and
/// guide once it is collected (about a minute in). The game's own System flow never reads Top while hidden; the page
/// keeps it the current window, so it holds it as AddToRoot does, and only if no one else rooted it.
void HoldTop(void* top) noexcept
{
    if (g_heldTop != nullptr && g_heldTop != top)
    {
        ReleaseTop();  // an older hold is let go, never lost by an overwrite
    }
    bool rooted = false;
    if (!IsRootGuarded(top, rooted))
    {
        platform::LogWarning(L"Co-op page: the main menu's first page could not be read in the object array; its title, help and "
                             L"guide may go blank at the next garbage collection");
        return;
    }
    if (rooted)
    {
        return;  // someone else's hold: left as found
    }
    if (!RootGuarded(top, true))
    {
        platform::LogWarning(L"Co-op page: the main menu's first page could not be held; its title, help and guide may go blank at "
                             L"the next garbage collection");
        return;
    }
    g_heldTop = top;
    platform::LogDebug(L"Co-op page: the main menu's hidden first page held from the garbage collection while the page is open");
}

/// The hold let go (the page closed, gone, or the mod unloading): the Top window is the game's to keep again. Only on the
/// game thread: the flag shares its word with the collector's own flags, which its worker tasks change atomically during
/// a collection the game thread runs; off it (an unload), the hold is forgotten and Top stays held until the game ends.
void ReleaseTop() noexcept
{
    if (g_heldTop == nullptr)
    {
        return;
    }
    if (!g_gameThread.Here())
    {
        g_heldTop = nullptr;
        platform::LogDebug(L"Co-op page: the main menu's first page left held: the unload runs off the game thread");
        return;
    }
    if (!RootGuarded(g_heldTop, false))
    {
        platform::LogWarning(L"Co-op page: the hold on the main menu's first page could not be let go; it stays until the game "
                             L"ends");
    }
    else
    {
        platform::LogDebug(L"Co-op page: the main menu's first page let go");
    }
    g_heldTop = nullptr;
}

/// The Top window's title and guide back as they were.
void RestoreTop() noexcept
{
    void* const top = weak::Resolve(g_topWeak);
    if (top == nullptr)
    {
        return;
    }
    if (!game_ui::AssignString(At(top, facts::kWindowTitle), g_savedTitle) ||
        !game_ui::AssignString(At(top, facts::kTopGuide), g_savedGuide))
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

/// The mod's state at a close over the title menu; the game's close function has run with the page's Back.
void ClosedAtTitle(const wchar_t* how) noexcept
{
    coop_page::SetOpen(false);
    ShowPageAgain(coop_page::Page());
    SwapHeadings(false);
    g_atTitle   = false;
    g_titleHost = {};
    platform::Log(L"Co-op page: closed over the title menu ({})", how);
}

/// The page gone or taken down over the title menu, so its Back never ran: what that Back's close function undoes, undone.
/// With the start menu gone too, only the stick's mode is left to put back.
void LeftAtTitle(void* page, const wchar_t* why) noexcept
{
    coop_page::SetOpen(false);
    ShowPageAgain(page);
    SwapHeadings(false);
    if (page != nullptr)
    {
        alignas(16) std::uint8_t taken[16]{};
        void* ref = nullptr;
        if (platform::CallGuarded(g.takeWidget, ref, page, static_cast<void*>(taken)) && ref != nullptr)
        {
            platform::CallGuarded(g.popLayer, players::game_globals::Object(players::game_globals::Global::UiSystemSlots), ref);
        }
        void* removed = nullptr;
        platform::CallGuarded(g.remove, removed, page);
        const std::uint8_t none = 0;
        platform::WriteGuarded(At(page, facts::kOptionsState), &none, sizeof(none));
    }
    if (void* const start = weak::Resolve(g_titleHost))
    {
        const TitleCapture capture{start, g_titleHost};
        platform::CallGuarded(g_title.closed, static_cast<const void*>(&capture));
    }
    else
    {
        platform::CallGuarded(g_title.setStick, players::game_globals::UiSlot(facts::kUiInputSlot), std::uint8_t{0});
    }
    g_atTitle   = false;
    g_titleHost = {};
    platform::LogWarning(L"Co-op page: left over the title menu ({})", why);
}

/// The page closed (by its own Back, or gone): the Top window back, as after any of the game's windows.
void Closed(const wchar_t* how) noexcept
{
    if (g_atTitle)
    {
        ClosedAtTitle(how);
        return;
    }
    coop_page::SetOpen(false);
    ShowPageAgain(coop_page::Page());
    SwapHeadings(false);
    RestoreTop();
    void* shown = nullptr;
    const bool ok = g_mgr != nullptr && platform::CallGuarded(g.showTop, shown, g_mgr, true) && shown != nullptr;
    ReleaseTop();  // after ShowTopWindow: it finds Top alive, the branch the game takes when no collection fell in between
    platform::Log(L"Co-op page: closed ({}); {}", how, ok ? L"the main menu is back" : L"the main menu could not be shown again (it closed)");
    g_mgr = nullptr;
}

/// The page taken down the way its own no-animation Back does (its focus layer popped, out of the overlay), for a
/// main menu closed under it.
void ForceClose(void* page, const wchar_t* why) noexcept
{
    coop_page::SetOpen(false);
    ShowPageAgain(page);
    SwapHeadings(false);
    if (page != nullptr)
    {
        alignas(16) std::uint8_t taken[16]{};
        void* ref = nullptr;
        if (platform::CallGuarded(g.takeWidget, ref, page, static_cast<void*>(taken)) && ref != nullptr)
        {
            platform::CallGuarded(g.popLayer, players::game_globals::Object(players::game_globals::Global::UiSystemSlots), ref);
        }
        void* removed = nullptr;
        platform::CallGuarded(g.remove, removed, page);
        const std::uint8_t none = 0;
        platform::WriteGuarded(At(page, facts::kOptionsState), &none, sizeof(none));
    }
    RestoreTop();
    ReleaseTop();
    g_mgr = nullptr;
    platform::Log(L"Co-op page: closed with the main menu ({}); its layer popped and the page taken out", why);
}

/// The page's state says it is closing or closed.
bool PageClosing(const void* page) noexcept
{
    std::uint8_t state = 0xFF;
    return platform::Read(static_cast<const std::uint8_t*>(page) + facts::kOptionsState, state) &&
           (state == facts::kOptionsStateClosing || state == 0);
}

/// The page's key handler (vtable slot): the game's, then the Top window back if Back closed the mod's page.
std::uintptr_t KeyDown(void* page, void* a, void* b, void* c)
{
    const std::uintptr_t reply = reinterpret_cast<KeyDownFn>(g_keyDown.Original())(page, a, b, c);
    if (coop_page::IsOpen() && coop_page::IsPage(page) && PageClosing(page))
    {
        Closed(L"Back");
    }
    return reply;
}

/// A font's drawing state: its flags and its texture pages (a null page's glyphs are skipped).
std::wstring FontState(const void* font)
{
    if (font == nullptr)
    {
        return L"none";
    }
    std::uint32_t flags = 0;
    platform::Read(static_cast<const std::uint8_t*>(font) + facts::kObjectFlags, flags);
    void* const  pages = platform::ReadPointer(font, facts::kFontPages);
    void* const  data  = platform::ReadPointer(pages, facts::kFontPagesData);
    std::int32_t count = -1;
    platform::Read(pages != nullptr ? static_cast<const std::uint8_t*>(pages) + facts::kFontPagesCount : nullptr, count);
    int nulls = 0;
    for (std::int32_t i = 0; data != nullptr && i < count && i < 64; ++i)
    {
        nulls += platform::ReadPointer(data, static_cast<std::uint32_t>(i) * 8) == nullptr ? 1 : 0;
    }
    return std::format(L"{:X} flags {:X} pages {} null {}", reinterpret_cast<std::uintptr_t>(font) & 0xFFFFFF, flags, count, nulls);
}

/// One of the main menu's frame texts as shown: its text, its own visibility, and its Slate block's drawing state.
std::wstring FrameText(const void* menu, std::uint32_t offset)
{
    void* const   block      = platform::ReadPointer(menu, offset);
    std::uint8_t  visibility = 0xFF;
    if (block == nullptr || !platform::Read(At(block, facts::kWidgetVisibility), visibility))
    {
        return L"(none)";
    }
    float alpha = -1.0f;
    platform::Read(At(block, facts::kTextBlockAlpha), alpha);
    std::wstring text = std::format(L"'{}' (visibility {}, alpha {:.2f}, font {})", game_ui::ReadString(At(block, facts::kTextBlockText)),
                                    visibility, alpha, FontState(platform::ReadPointer(block, facts::kTextBlockFont)));
    void* const slate = platform::ReadPointer(block, facts::kTextBlockSlate);
    if (slate == nullptr)
    {
        return text + L" [no Slate block]";
    }
    std::int32_t batches = -1;
    float        width   = -1.0f;
    std::uint8_t pending[2]{0xFF, 0xFF};
    platform::Read(At(slate, facts::kSlateTextBatches), batches);
    platform::Read(At(slate, facts::kSlateTextSize), width);
    platform::Read(At(slate, facts::kSlateTextPending), pending);
    return text + std::format(L" [Slate '{}', font {}, second font {}, batches {}, width {:.0f}, pending {} {}]",
                              game_ui::ReadString(At(slate, facts::kSlateTextText)),
                              FontState(platform::ReadPointer(slate, facts::kSlateTextFont)),
                              FontState(platform::ReadPointer(slate, facts::kSlateTextFontTwo)), batches, width, pending[0], pending[1]);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
try
{
    namespace f = facts;
    const auto showTop    = platform::FindFunction(image, f::kShowTopWindow);
    const auto open       = platform::FindFunction(image, f::kOptionsOpen);
    const auto remove     = platform::FindFunction(image, f::kRemoveFromRootOverlay);
    const auto takeWidget = platform::FindFunction(image, f::kTakeWidget);
    const auto popLayer   = platform::FindFunction(image, f::kPopFocusLayer);
    const auto setVisible = platform::FindFunction(image, f::kWidgetSetVisibilityFn);
    const auto headings   = image.Rebase(f::kOptionsHeadings, 8 * f::kOptionsHeadingsWere.size());
    bool       headingsOk = headings.has_value();
    for (std::size_t key = 0; key < f::kOptionsHeadingsWere.size() && headingsOk; ++key)
    {
        const auto was      = image.Rebase(f::kOptionsHeadingsWere[key]);
        headingsOk          = was.has_value();
        g.headingsWere[key] = was.value_or(0);
    }
    if (!showTop || !open || !remove || !takeWidget || !popLayer || !setVisible || !headingsOk)
    {
        return false;
    }
    g.showTop       = reinterpret_cast<ShowTopFn>(*showTop);
    g.open          = reinterpret_cast<OpenFn>(*open);
    g.remove        = reinterpret_cast<RemoveFn>(*remove);
    g.takeWidget    = reinterpret_cast<TakeWidgetFn>(*takeWidget);
    g.popLayer      = reinterpret_cast<PopLayerFn>(*popLayer);
    g.setVisibility = reinterpret_cast<SetVisibilityFn>(*setVisible);
    g.headings      = *headings;
    return true;
}
catch (...)
{
    return false;
}

bool Place(const platform::GameImage& image) noexcept
{
    return platform::ReplaceSlot(image, facts::kOptionsKeyDown, reinterpret_cast<std::uintptr_t>(&KeyDown), g_keyDown);
}

void Unplace() noexcept
{
    SwapHeadings(false);
    g_keyDown.Restore();
    ReleaseTop();
}

void Remove() noexcept
{
    if (coop_page::IsOpen())
    {
        coop_page::SetOpen(false);
        platform::Log(L"Co-op page: the mod unloads with the page open; the page is left to the game");
    }
    g_atTitle        = false;
    g_pageVisibility = -1;
    ReleaseTop();  // a flag in the object array: no UI call
}

bool InstallTitle(const platform::GameImage& image) noexcept
try
{
    namespace f = facts;
    const auto openOptions = platform::FindFunction(image, f::kStartMenuOpenOptions);
    const auto ensureFrame = platform::FindFunction(image, f::kStartMenuEnsureFrame);
    const auto closed      = platform::FindFunction(image, f::kStartMenuOptionsClosed);
    const auto setStick    = platform::FindFunction(image, f::kSetStickDPadMode);
    const auto setText     = platform::FindFunction(image, f::kTextBlockSetWide);
    if (!openOptions || !ensureFrame || !closed || !setStick || !setText)
    {
        return false;
    }
    g_title.openOptions = reinterpret_cast<TitleCallFn>(*openOptions);
    g_title.ensureFrame = reinterpret_cast<TitleCallFn>(*ensureFrame);
    g_title.closed      = reinterpret_cast<ClosedFn>(*closed);
    g_title.setStick    = reinterpret_cast<SetStickFn>(*setStick);
    g_title.setText     = reinterpret_cast<SetTextFn>(*setText);
    return true;
}
catch (...)
{
    return false;
}

void OpenAtTitle(void* start) noexcept
try
{
    if (coop_page::IsOpen() || coop_page::Failed() || g_title.openOptions == nullptr)
    {
        return;
    }
    const weak::Weak host = weak::MakeWeak(start);
    if (!host.Set())
    {
        platform::LogWarning(L"Co-op page: the title menu could not be read in the object array; not opened");
        return;
    }
    // The frame first: the overlay draws the page and the frame at one order in the order they were added.
    if (!platform::CallGuarded(g_title.ensureFrame, start))
    {
        platform::LogWarning(L"Co-op page: the title menu's frame could not be made; not opened");
        return;
    }
    void* const page = coop_page::Ready(start, facts::kTitleOptionsZ, std::uint8_t{0});
    if (page == nullptr)
    {
        // The frame is up and holds the UI filters: only the game's close function lets them go.
        const TitleCapture capture{start, host};
        platform::CallGuarded(g_title.closed, static_cast<const void*>(&capture));
        return;
    }
    g_atTitle   = true;
    g_titleHost = host;
    SwapHeadings(true);
    coop_page::SetOpen(true);
    // The game's open reads its page from one field and has no parameter for it. The page is in the overlay, so the open
    // does not add it again and its own data stays. The field is the game's again before any other code can read it.
    void* const gamesPage = platform::ReadPointer(start, facts::kStartMenuPage);
    bool        opened    = platform::WriteGuarded(At(start, facts::kStartMenuPage), &page, sizeof(page));
    opened                = opened && platform::CallGuarded(g_title.openOptions, start);
    if (!platform::WriteGuarded(At(start, facts::kStartMenuPage), &gamesPage, sizeof(gamesPage)))
    {
        platform::LogError(L"Co-op page: the title menu's Options page could not be put back");
    }
    if (!opened)
    {
        LeftAtTitle(page, L"its opening faulted");
        return;
    }
    // Written once by the frame's open, and rewritten by the next one: the game's own Options still reads Options.
    void* const frame = platform::ReadPointer(start, facts::kStartMenuFrame);
    void* const title = frame != nullptr ? platform::ReadPointer(frame, facts::kMainMenuTitleText) : nullptr;
    if (title == nullptr || !platform::CallGuarded(g_title.setText, title, rules::kTitle))
    {
        platform::LogWarning(L"Co-op page: the title menu's frame could not be titled; it reads Options");
    }
    help_spacing::Tighten(frame);
    ++g_opens;
    platform::Log(L"Co-op page: opened over the title menu ({} categories, {} options)", coop_page::Categories().size(), settings::kOptions.size());
}
catch (...)
{
    if (g_atTitle)
    {
        LeftAtTitle(coop_page::Page(), L"an exception while opening");
    }
}

void Open(void* mgr, OpenWindowFn openWindow) noexcept
try
{
    if (coop_page::IsOpen() || coop_page::Failed())
    {
        return;
    }
    std::uint8_t tutorial = 0;
    if (void* const t = players::game_globals::UiSlot(facts::kUiMenuTutorialSlot);
        t != nullptr && platform::Read(At(t, facts::kMenuTutorialState), tutorial) &&
        tutorial >= facts::kMenuTutorialBusyFirst && tutorial <= facts::kMenuTutorialBusyLast)
    {
        platform::LogDebug(L"Co-op page: the menu tutorial is busy ({}); not opened, as the game opens no window then", tutorial);
        return;
    }
    platform::CallGuarded(openWindow, mgr, facts::kOpenWindowPreludeOnly);
    void* const  top        = game_ui::WeakAt(mgr, facts::kMainMenuMgrCurrent);
    std::uint8_t topCurrent = 0;
    if (!game_ui::IsTopWindow(top) || !platform::Read(At(mgr, facts::kMainMenuMgrTopCurrent), topCurrent) || topCurrent != 1)
    {
        platform::LogWarning(L"Co-op page: the main menu's current window is not its first page; not opened");
        return;
    }
    std::int32_t z = 0;
    platform::Read(At(top, facts::kWindowZOrder), z);
    void* const page = coop_page::Ready(top, z, std::uint8_t{1});
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
    HoldTop(top);
    g_savedTitle = game_ui::ReadString(At(top, facts::kWindowTitle));
    g_savedGuide = game_ui::ReadString(At(top, facts::kTopGuide));
    game_ui::AssignString(At(top, facts::kWindowTitle), rules::kTitle);
    game_ui::AssignString(At(top, facts::kTopGuide), kGuide);
    SwapHeadings(true);
    g_mgr = mgr;
    coop_page::SetOpen(true);
    if (!platform::CallGuarded(g.open, page, std::uint8_t{0}))
    {
        platform::LogWarning(L"Co-op page: opening the page faulted; the main menu comes back");
        Closed(L"its opening faulted");
        return;
    }
    help_spacing::Tighten(game_ui::WeakAt(mgr, facts::kMainMenuMgrMenu));
    ++g_opens;
    platform::Log(L"Co-op page: opened ({} categories, {} options; the main menu's first page hidden, its title Co-op)",
                  coop_page::Categories().size(), settings::kOptions.size());
}
catch (...)
{
    if (!coop_page::IsOpen())
    {
        ReleaseTop();  // thrown after the hold, before the page opened: nothing else would let it go
    }
}

void Tick() noexcept
{
    g_gameThread.Set();
    if (!coop_page::IsOpen())
    {
        return;
    }
    if (g_atTitle)
    {
        void* const titlePage = coop_page::Page();
        if (titlePage == nullptr)
        {
            LeftAtTitle(nullptr, L"the page is gone");
        }
        else if (weak::Resolve(g_titleHost) == nullptr)
        {
            LeftAtTitle(titlePage, L"the title menu is gone");
        }
        else if (PageClosing(titlePage))
        {
            ClosedAtTitle(L"seen by the tick");
        }
        return;
    }
    // A main menu closed under the page (no game path is known to while the page has focus): take it down.
    void* const  page     = coop_page::Page();
    void* const  mgr      = players::game_globals::UiSlot(facts::kUiMainMenuSlot);
    std::int32_t menuOpen = 0;
    const bool   menuUp   = mgr == g_mgr && platform::Read(At(mgr, facts::kMainMenuMgrOpen), menuOpen) && menuOpen == 1 &&
                        game_ui::WeakAt(mgr, facts::kMainMenuMgrMenu) != nullptr;
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
            coop_page::SetOpen(false);
            ShowPageAgain(nullptr);
            SwapHeadings(false);
            RestoreTop();
            ReleaseTop();
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
    if (PageClosing(page))
    {
        Closed(L"seen by the tick");  // closed without the key handler seeing it
    }
}

std::uint64_t Opens() noexcept
{
    return g_opens;
}

std::wstring FrameReading()
{
    if (!coop_page::IsOpen() || g_mgr == nullptr)
    {
        return {};
    }
    void* const  menu           = game_ui::WeakAt(g_mgr, facts::kMainMenuMgrMenu);
    std::uint8_t pageVisibility = 0xFF;
    if (void* const page = coop_page::Page())
    {
        platform::Read(At(page, facts::kWidgetVisibility), pageVisibility);
    }
    return std::format(L"; main menu title {}, help {}, guide {}; page visibility {}", FrameText(menu, facts::kMainMenuTitleText),
                       FrameText(menu, facts::kMainMenuHelpText), FrameText(menu, facts::kMainMenuGuideText), pageVisibility);
}

std::wstring SetPageHidden(bool hide)
{
    void* const page = coop_page::IsOpen() ? coop_page::Page() : nullptr;
    if (page == nullptr)
    {
        return L"the Co-op page is not open";
    }
    if (hide == (g_pageVisibility >= 0))
    {
        return {};
    }
    std::uint8_t was = 0;
    if (hide && !platform::Read(At(page, facts::kWidgetVisibility), was))
    {
        return L"the page's visibility is unreadable";
    }
    const auto to = hide ? facts::kVisibilityHidden : static_cast<std::uint8_t>(g_pageVisibility);
    if (!platform::CallGuarded(g.setVisibility, page, to))
    {
        return L"the setter faulted";
    }
    g_pageVisibility = hide ? was : -1;
    platform::Log(L"Co-op page: {} (dev switch)", hide ? L"hidden" : L"shown again");
    return {};
}

bool PageHidden() noexcept
{
    return g_pageVisibility >= 0;
}

}  // namespace lss::menu::coop_page_open
