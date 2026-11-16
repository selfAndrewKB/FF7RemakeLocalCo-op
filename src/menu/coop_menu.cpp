#include "menu/coop_menu.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "menu/coop_menu_rules.h"
#include "menu/coop_page.h"
#include "menu/coop_page_open.h"
#include "menu/coop_slider.h"
#include "menu/game_ui.h"
#include "menu/title_row.h"
#include "platform/entry_redirect.h"
#include "platform/game_array.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/game_globals.h"

#include <cstdint>
#include <format>
#include <span>
#include <string>

namespace lss::menu::coop_menu {

namespace {

namespace rules    = coop_menu_rules;
namespace settings = platform::settings;

using game_ui::At;
using OpenWindowFn = coop_page_open::OpenWindowFn;

/// Where the row's opener and table words go, found at install.
struct Game
{
    std::uintptr_t openWindowAt = 0;
    std::uintptr_t textSlot = 0, textWas = 0, helpSlot = 0, helpWas = 0, unlockSlot = 0, orderWord = 0;
};

// Set at install and on the first tick, then only read.
Game                    g;
platform::EntryRedirect g_openWindow;
platform::VTableEntry   g_text, g_help, g_unlock, g_order;
bool                    g_installed = false;
bool                    g_pending   = false;  // the first tick places everything
bool                    g_placed    = false;  // the row shows and opens the page
bool                    g_sliderFound = false;  // the long rows can be sliders
bool                    g_titleFound  = false;  // the title menu can have its row

/// The game's window opener: the Co-op row's kind opens the page; every other kind is the game's.
void OpenWindow(void* mgr, std::uint8_t kind)
{
    const auto original = reinterpret_cast<OpenWindowFn>(g_openWindow.Original());
    if (kind == facts::kTopRowKind && g_placed && mgr != nullptr)
    {
        coop_page_open::Open(mgr, original);
        return;
    }
    original(mgr, kind);
}

/// The redirects, the key handler's slot, then the four table words (the row last, so it never shows unopenable).
bool Place(const platform::GameImage& image) noexcept
{
    const bool redirected =
        g_openWindow.Install(g.openWindowAt, std::span(facts::kOpenWindowByKindEntry.data(), facts::kRedirectFirstBytes),
                             reinterpret_cast<std::uintptr_t>(&OpenWindow)) &&
        coop_page::Place() && coop_page_open::Place(image);
    if (!redirected)
    {
        return false;
    }
    // The sliders are their own: without them the long rows stay lists (logged by coop_slider).
    if (g_sliderFound)
    {
        coop_slider::Place(image);
    }
    // The title's row is its own too: without it the title stays as it was (logged by title_row).
    if (g_titleFound)
    {
        title_row::Place(image);
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
    // The title's row, the headings and the page's key handler, then the row (its tables back), then the values and the
    // row's opener.
    title_row::Unplace();
    coop_page_open::Unplace();
    g_order.Restore();
    g_unlock.Restore();
    g_help.Restore();
    g_text.Restore();
    coop_slider::Unplace();
    coop_page::Unplace();
    g_openWindow.Restore();
}

std::wstring Reading()
{
    std::wstring text = std::format(L"row {}; page {}{}; opened {}, values set {}", g_placed ? L"placed" : L"not placed",
                                    coop_page::IsOpen() ? L"open" : L"closed", coop_page::Failed() ? L" (failed: opens nothing)" : L"",
                                    coop_page_open::Opens(), coop_page::Commits());
    text += title_row::Reading();
    text += coop_page_open::FrameReading();
    void* const         mgr = players::game_globals::UiSlot(facts::kUiMainMenuSlot);
    void* const         top = mgr != nullptr ? game_ui::WeakAt(mgr, facts::kMainMenuMgrCurrent) : nullptr;
    platform::GameArray rows;
    if (game_ui::IsTopWindow(top) && platform::Read(At(top, facts::kTopRows), rows) && rows.data != nullptr && rows.num > 0 &&
        rows.num <= 16)
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
    const auto textSlot   = image.Rebase(f::kTopRowTextSlot, 8);
    const auto textWas    = image.Rebase(f::kTopRowTextWas);
    const auto helpSlot   = image.Rebase(f::kTopRowHelpSlot, 8);
    const auto helpWas    = image.Rebase(f::kTopRowHelpWas);
    const auto unlockSlot = image.Rebase(f::kTopRowUnlockSlot, 8);
    const auto orderWord  = image.Rebase(f::kTopRowOrderWord, 8);
    // The shared pieces first: the page writes its strings with their copy.
    if (!openWindow || !textSlot || !textWas || !helpSlot || !helpWas || !unlockSlot || !orderWord || !game_ui::Install(image) ||
        !coop_page::Install(image) || !coop_page_open::Install(image))
    {
        platform::Log(L"Co-op menu: an engine check failed; no Co-op row in the main menu (the options stay on the console and the "
                      L"settings file)");
        return false;
    }
    g.openWindowAt = *openWindow;
    g.textSlot     = *textSlot;
    g.textWas      = *textWas;
    g.helpSlot     = *helpSlot;
    g.helpWas      = *helpWas;
    g.unlockSlot   = *unlockSlot;
    g.orderWord    = *orderWord;
    g_sliderFound  = coop_slider::Install(image);
    g_titleFound   = title_row::Install(image);
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
                           const std::wstring refused = coop_page_open::SetPageHidden(std::get<bool>(value));
                           return refused.empty() ? control::Outcome{} : control::Outcome{false, refused};
                       },
                       .get = [] { return control::Value{coop_page_open::PageHidden()}; }});
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
    // The unload may run off the game thread: no UI calls off it, only the mod's own state and its changes put back.
    coop_page_open::Remove();
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
                          settings::kOptions.size(), coop_page::Categories().size());
        }
        else
        {
            Unplace();
            platform::Log(L"Co-op menu: the row's opener or tables could not be placed; no Co-op row (the options stay on the console "
                          L"and the settings file)");
        }
    }
    coop_page::Tick();
    coop_page_open::Tick();
}

}  // namespace lss::menu::coop_menu
