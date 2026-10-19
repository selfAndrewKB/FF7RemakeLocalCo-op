#include "players/game_state.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/log.h"

#include <Unreal/NameTypes.hpp>

#include <cstdint>
#include <string>

namespace lss::players::game_state {

namespace {

const std::uint8_t* const* g_gameMode = nullptr;  // AEndGameMode**
const std::uint8_t* const* g_root     = nullptr;  // the field root**
Snapshot                   g_last;
bool                       g_logged = false;

/// The name's text through UE4SS's FName (the game's own name pool). A free function, so a fault inside the
/// lookup is caught by the guard rather than ending the game.
void NameText(std::int64_t bits, std::wstring* out) noexcept
try
{
    *out = RC::Unreal::FName(bits).ToString();
}
catch (...)
{
    out->clear();
}

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
    const std::uint8_t* root = nullptr;
    if (platform::CopyGuarded(&root, g_root, sizeof(root)) && root != nullptr)
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
            platform::CallGuarded(&NameText, name, &now.location);
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

Snapshot Current() noexcept
{
    return g_last;
}

bool Install(const platform::GameImage& image) noexcept
{
    const auto gameMode = image.Rebase(facts::kGameModeGlobal, sizeof(void*));
    const auto root     = image.Rebase(facts::kEndGameRootGlobal, sizeof(void*));
    if (!gameMode || !root)
    {
        platform::Log(L"game state: a global is outside the image; no game.state reading");
        return false;
    }
    g_gameMode = reinterpret_cast<const std::uint8_t* const*>(*gameMode);
    g_root     = reinterpret_cast<const std::uint8_t* const*>(*root);

    using control::Register;
    using control::Shape;
    using control::Value;
    Register({.name  = L"game.state",
              .shape = Shape::Word,
              .words = {L"none", L"boot", L"title", L"loading", L"play", L"pause", L"menu", L"leaving", L"chapter-start",
                        L"other", L"unknown"},
              .help  = L"the game's EGameState (AEndGameState); unknown while it cannot be read",
              .get   = [] { return Value{std::wstring(g_last.valid ? StateName(g_last.state) : L"unknown")}; }});
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

}  // namespace lss::players::game_state
