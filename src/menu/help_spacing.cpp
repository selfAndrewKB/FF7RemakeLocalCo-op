#include "menu/help_spacing.h"

#include "engine_facts.h"
#include "menu/coop_slider.h"
#include "menu/game_ui.h"
#include "platform/game_thread.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/weak_object.h"

#include <cstdint>

namespace lss::menu::help_spacing {

namespace {

namespace weak = players::weak_object;
using game_ui::At;

using SynchronizeFn = void (*)(void* widget);

/// The page's line height: a blank line half a line, a line with glyphs at its glyphs' height.
constexpr float kPageLineHeight = 0.5f;

// Game thread.
std::uintptr_t g_synchronize = 0;
weak::Weak     g_block{};                                     // the help bar while the page holds its line height
float          g_gameHeight = facts::kTextBlockLineHeightDefault;  // the bar's own line height, put back at the close
weak::Weak     g_slot{};          // the bar's canvas slot while the page may lift it
float          g_homeX = 0.0f, g_homeY = 0.0f;  // the slot's own position, put back at the close
float          g_lift  = 0.0f;    // how far the bar stands above its own position now
bool           g_liftSaid = false;  // a failed lift logged for this open

/// The slot back at its own position, if the page lifted it.
void PutBackSlot() noexcept
{
    void* const slot = weak::Resolve(g_slot);
    g_slot           = {};
    if (slot == nullptr || g_lift == 0.0f)
    {
        return;
    }
    if (coop_slider::PlaceCanvasSlot(slot, g_homeX, g_homeY))
    {
        platform::Log(L"help spacing: the help bar back at its own place");
    }
    else
    {
        platform::LogWarning(L"help spacing: the help bar could not be put back at its own place");
    }
    g_lift = 0.0f;
}

/// The bar's line height set to `height` through its own SynchronizeProperties, if its vtable holds the text block's. A
/// call that faults after the write puts the old value back, so the bar never keeps a height the mod no longer tracks.
bool Apply(void* block, float height) noexcept
{
    const auto synchronize =
        reinterpret_cast<SynchronizeFn>(platform::ReadPointer(platform::ReadPointer(block, 0), facts::kWidgetSynchronizeSlot));
    if (synchronize == nullptr || reinterpret_cast<std::uintptr_t>(synchronize) != g_synchronize)
    {
        platform::LogWarning(L"help spacing: the help bar is not the game's text block; its spacing is left as it is");
        return false;
    }
    float before = 0.0f;
    if (!platform::Read(At(block, facts::kTextBlockLineHeight), before))
    {
        platform::LogWarning(L"help spacing: the help bar's line height could not be read; its spacing is left as it is");
        return false;
    }
    if (!platform::WriteGuarded(At(block, facts::kTextBlockLineHeight), &height, sizeof height))
    {
        platform::LogWarning(L"help spacing: the help bar's line height could not be written");
        return false;
    }
    if (!platform::CallGuarded(synchronize, block))
    {
        if (platform::WriteGuarded(At(block, facts::kTextBlockLineHeight), &before, sizeof before))
        {
            platform::LogWarning(L"help spacing: the help bar's refresh faulted; its line height is back to {:.2f}", before);
        }
        else
        {
            platform::LogError(L"help spacing: the help bar's refresh faulted and its line height could not be put back to {:.2f}",
                               before);
        }
        return false;
    }
    return true;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto synchronize = platform::FindFunction(image, facts::kTextBlockSynchronize);
    if (!synchronize)
    {
        platform::LogWarning(L"help spacing: an engine check failed; a blank line in a Co-op help stays a whole line");
        return false;
    }
    g_synchronize = *synchronize;
    return true;
}

void Tighten(void* menu) noexcept
{
    if (g_synchronize == 0 || menu == nullptr)
    {
        return;
    }
    Restore();
    void* const block = platform::ReadPointer(menu, facts::kMainMenuHelpText);
    float       game  = 0.0f;
    if (block == nullptr || !platform::Read(At(block, facts::kTextBlockLineHeight), game))
    {
        platform::LogWarning(L"help spacing: the menu's help bar could not be read; its spacing is left as it is");
        return;
    }
    // The page's own height still there means a close could not put the game's back (an unload off the game thread):
    // the class default is the game's.
    if (game == kPageLineHeight)
    {
        platform::Log(L"help spacing: the help bar still holds the page's line height; the game's default {:.2f} is its own",
                      facts::kTextBlockLineHeightDefault);
        game = facts::kTextBlockLineHeightDefault;
    }
    const weak::Weak held = weak::MakeWeak(block);
    if (!held.Set())
    {
        platform::LogWarning(L"help spacing: the help bar could not be held; its spacing is left as it is");
        return;
    }
    if (!Apply(block, kPageLineHeight))
    {
        return;
    }
    g_gameHeight = game;
    g_block      = held;
    platform::Log(L"help spacing: the help bar's line height {:.2f} -> {:.2f} while the Co-op page is open", game, kPageLineHeight);

    // The slot the bar is lifted by, with its own position to put back.
    void* const slot = game_ui::SlotOf(block);
    if (slot == nullptr || !platform::Read(At(slot, facts::kCanvasSlotLeft), g_homeX) ||
        !platform::Read(At(slot, facts::kCanvasSlotTop), g_homeY))
    {
        platform::LogWarning(L"help spacing: the help bar's slot could not be read; a help of several lines is not lifted");
        return;
    }
    g_slot     = weak::MakeWeak(slot);
    g_lift     = 0.0f;
    g_liftSaid = false;
    platform::LogDebug(L"help spacing: the help bar's own place ({:.1f}, {:.1f})", g_homeX, g_homeY);
}

void Tick() noexcept
{
    if (!g_slot.Set() || g_liftSaid)
    {
        return;  // no slot, or a lift failed this open: not tried again until the next open
    }
    void* const block = weak::Resolve(g_block);
    void* const slot  = weak::Resolve(g_slot);
    void* const text  = block != nullptr ? platform::ReadPointer(block, facts::kTextBlockSlate) : nullptr;
    float       height = 0.0f;
    if (slot == nullptr || text == nullptr || !platform::Read(At(text, facts::kTextBlockDesiredHeight), height) || height <= 0.0f)
    {
        return;
    }
    // The help's last line where a one-line help's line ends: a taller help stands up by the difference.
    const float lift = height > facts::kHelpBarOneLineHeight ? height - facts::kHelpBarOneLineHeight : 0.0f;
    if (lift == g_lift)
    {
        return;
    }
    if (!coop_slider::PlaceCanvasSlot(slot, g_homeX, g_homeY - lift))
    {
        g_liftSaid = true;
        platform::LogWarning(L"help spacing: the help bar could not be lifted (its slot is no canvas slot, or the call failed); "
                             L"not tried again until the page opens again");
        return;
    }
    platform::LogDebug(L"help spacing: a help {:.1f} tall: the bar {:.1f} above its own place", height, lift);
    g_lift = lift;
}

void Restore() noexcept
{
    if (!g_block.Set())
    {
        return;
    }
    // The mod's unload may run off the game thread: no UI call there. The bar keeps the page's height and place until the
    // game makes it again. The next open takes the class default as the game's height; a place left lifted is not
    // recognised, so the bar stays that much higher until the game makes it again.
    if (!platform::OnGameThread())
    {
        g_block = {};
        g_slot  = {};
        platform::LogWarning(L"help spacing: closed off the game thread; the help bar keeps the page's line height and place "
                             L"until the game makes it again");
        return;
    }
    PutBackSlot();
    void* const block = weak::Resolve(g_block);
    g_block           = {};
    if (block == nullptr)
    {
        platform::Log(L"help spacing: the help bar is gone; nothing to put back");
        return;
    }
    if (Apply(block, g_gameHeight))
    {
        platform::Log(L"help spacing: the help bar's line height back to {:.2f}", g_gameHeight);
    }
}

}  // namespace lss::menu::help_spacing
