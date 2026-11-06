#include "players/game_state.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/name_text.h"
#include "platform/verified_code.h"
#include "platform/log.h"
#include "players/field_root.h"

#include <array>
#include <cstdint>
#include <string>

namespace lss::players::game_state {

namespace {

const std::uint8_t* const* g_gameMode = nullptr;  // AEndGameMode**
void (*g_pauseMenuClosed)(void* context, void* frame, std::uint8_t* result) = nullptr;  // the game's own test
const std::uint8_t* const* g_shopSlot = nullptr;  // EndUISystem[44], the shop UI (its "a shop is up" byte)
Snapshot                   g_last;
bool                       g_logged = false;

/// One guarded read of a value through a pointer chain.
template <class T>
bool ReadAt(const std::uint8_t* base, std::size_t offset, T& out) noexcept
{
    return base != nullptr && platform::CopyGuarded(&out, base + offset, sizeof(T));
}

Snapshot Read() noexcept
{
    Snapshot now;
    const std::uint8_t* gameMode  = nullptr;
    const std::uint8_t* gameState = nullptr;
    if (platform::CopyGuarded(&gameMode, g_gameMode, sizeof(gameMode)) && gameMode != nullptr &&
        ReadAt(gameMode, facts::kGameModeGameState, gameState) && gameState != nullptr)
    {
        std::uint8_t pause = 0;
        now.valid = ReadAt(gameState, facts::kGameStateState, now.state) &&
                    ReadAt(gameState, facts::kGameStateMenuKind, now.menu) &&
                    ReadAt(gameState, facts::kGameStatePause, pause) && now.state < 32;  // EGameState_MAX
        now.paused = pause != 0;
    }
    const std::uint8_t* root = field_root::Root();
    if (root != nullptr)
    {
        std::uint8_t running = 0;
        now.hasRoot = ReadAt(root, facts::kFieldRootStep, now.step) && ReadAt(root, facts::kFieldRootRunning, running);
        now.running = running != 0;
        std::int64_t name = 0;
        if (!now.hasRoot)
        {
            now.step = -1;
        }
        else if (ReadAt(root, facts::kFieldRootLocation, name) && name != 0)
        {
            now.location = platform::NameText(static_cast<std::uint64_t>(name));
        }
    }
    return now;
}

}  // namespace

const wchar_t* StateName(std::uint8_t state) noexcept
{
    switch (state)
    {
    case 0: return L"none";
    case 1: return L"boot";
    case 4: return L"title";
    case 5: return L"loading";
    case 6: return L"play";
    case 7: return L"pause";
    case 8: return L"menu";
    case 9: return L"leaving";
    case 10: return L"chapter-start";
    default: return L"other";
    }
}

const Snapshot& Current() noexcept
{
    return g_last;
}

bool Install(const platform::GameImage& image) noexcept
{
    const auto gameMode = image.Rebase(facts::kGameModeGlobal, sizeof(void*));
    if (!gameMode || !field_root::Installed())
    {
        platform::Log(L"game state: a global is outside the image; no game.state reading");
        return false;
    }
    g_gameMode = reinterpret_cast<const std::uint8_t* const*>(*gameMode);
    if (const auto shop = image.Rebase(facts::kShopSlotGlobal, sizeof(void*)))
    {
        g_shopSlot = reinterpret_cast<const std::uint8_t* const*>(*shop);
    }
    if (const auto closed = platform::FindFunction(image, facts::kIsPauseMenuClosed))
    {
        g_pauseMenuClosed = reinterpret_cast<decltype(g_pauseMenuClosed)>(*closed);
    }
    else
    {
        platform::LogWarning(L"game state: the pause menu test failed its check; a menu owns input only in the "
                             L"menu states");
    }

    using control::Register;
    using control::Shape;
    using control::Value;
    Register({.name  = L"game.state",
              .shape = Shape::Word,
              .words = {L"none", L"boot", L"title", L"loading", L"play", L"pause", L"menu", L"leaving", L"chapter-start",
                        L"other", L"unknown"},
              .help  = L"the game's EGameState (AEndGameState); unknown while it cannot be read",
              .get   = [] { return Value{std::wstring(g_last.valid ? StateName(g_last.state) : L"unknown")}; }});
    Register({.name = L"game.menu-owns-input", .shape = Shape::OnOff,
              .help = L"a menu owns the game's input (the pause or menu states, or the game's IsPauseMenuClosed "
                      L"is false)",
              .get  = [] { return Value{MenuOwnsInput()}; }});
    Register({.name = L"game.paused", .shape = Shape::OnOff, .help = L"the game's own pause flag (IsGamePause)",
              .get = [] { return Value{g_last.valid && g_last.paused}; }});
    Register({.name = L"game.menu-kind", .shape = Shape::Integer, .min = -1, .max = 255,
              .help = L"the menu's kind while the state is menu; -1 while unreadable",
              .get  = [] { return Value{static_cast<std::int64_t>(g_last.valid ? g_last.menu : -1)}; }});
    Register({.name = L"field.step", .shape = Shape::Integer, .min = -1, .max = 1000,
              .help = L"the field step machine (13 = steady field); -1 without a field root",
              .get  = [] { return Value{static_cast<std::int64_t>(g_last.step)}; }});
    Register({.name = L"field.running", .shape = Shape::OnOff, .help = L"the field step machine is running",
              .get = [] { return Value{g_last.hasRoot && g_last.running}; }});
    Register({.name = L"game.location", .shape = Shape::Text, .help = L"the field root's location name (the title screen is one too)",
              .get = [] { return Value{g_last.location.empty() ? std::wstring(L"-") : g_last.location}; }});
    platform::Log(L"game state: readings installed (game.state, game.paused, game.menu-kind, game.location, field.step, field.running)");
    return true;
}

void Tick() noexcept
{
    if (g_gameMode == nullptr)
    {
        return;
    }
    const Snapshot now = Read();
    if (!g_logged || now.valid != g_last.valid || now.state != g_last.state)
    {
        platform::Log(L"game state: {} ({}){}", now.valid ? StateName(now.state) : L"unknown", now.state,
                      now.valid ? L"" : L" (not readable)");
    }
    if (!g_logged || now.paused != g_last.paused || now.menu != g_last.menu)
    {
        platform::LogDebug(L"game state: paused {} menu kind {}", now.paused ? L"on" : L"off", now.menu);
    }
    if (!g_logged || now.hasRoot != g_last.hasRoot || now.step != g_last.step || now.running != g_last.running)
    {
        platform::LogDebug(L"field step: {} running {}{}", now.step, now.running ? L"on" : L"off",
                           now.hasRoot ? L"" : L" (no root)");
    }
    if (!g_logged || now.location != g_last.location)
    {
        platform::Log(L"location: {}", now.location.empty() ? L"-" : now.location);
    }
    g_last   = now;
    g_logged = true;
}

void* World() noexcept
{
    void*       gameMode = nullptr;
    void**      vtable   = nullptr;
    void* (*getWorld)(void*) = nullptr;
    void*       world    = nullptr;
    if (g_gameMode == nullptr || !platform::CopyGuarded(&gameMode, g_gameMode, sizeof(gameMode)) || gameMode == nullptr ||
        !platform::CopyGuarded(&vtable, gameMode, sizeof(vtable)) ||
        !platform::CopyGuarded(&getWorld, reinterpret_cast<std::uint8_t*>(vtable) + facts::kGameModeGetWorldSlot, sizeof(getWorld)) ||
        !platform::CallGuarded(getWorld, world, gameMode))
    {
        return nullptr;
    }
    return world;
}

bool MenuOwnsInput() noexcept
{
    const std::uint8_t* gameMode  = nullptr;
    const std::uint8_t* gameState = nullptr;
    std::uint8_t        state     = 0;
    if (g_gameMode != nullptr && platform::CopyGuarded(&gameMode, g_gameMode, sizeof(gameMode)) &&
        ReadAt(gameMode, facts::kGameModeGameState, gameState) && ReadAt(gameState, facts::kGameStateState, state) &&
        (state == 7 || state == 8 || state == 10))
    {
        return true;
    }
    // A shop runs in the field state (6): its UI's own "a shop is up" byte (RE-shop-two.md §3).
    return ShopUp() || PauseWidgetAlive();
}

bool ShopUp() noexcept
{
    const std::uint8_t* shop = nullptr;
    std::uint8_t        up   = 0;
    return g_shopSlot != nullptr && platform::CopyGuarded(&shop, g_shopSlot, sizeof(shop)) && shop != nullptr &&
           ReadAt(shop, facts::kShopUp, up) && up != 0;
}

// Read by the whole-screen moment too (views/whole_screen.h), so a pause-family widget alive collapses the one-window
// split; whether a widget of the family stays alive through a battle (Libra, a tutorial popup) is [UNKNOWN: the play
// check reads the Debug signal line in a fight].
bool PauseWidgetAlive() noexcept
{
    if (g_pauseMenuClosed == nullptr)
    {
        return false;
    }
    std::array<std::uint8_t, facts::kScriptFrameBytes> frame{};
    std::uint8_t closed = 1;
    return platform::CallGuarded(g_pauseMenuClosed, static_cast<void*>(nullptr), static_cast<void*>(frame.data()),
                                 &closed) &&
           closed == 0;
}

}  // namespace lss::players::game_state
