#include "players/seat_responsiveness.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/byte_scope.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "players/game_globals.h"
#include "players/scopes.h"

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <span>

namespace lss::players::seat_responsiveness {

namespace {

namespace settings = platform::settings;

/// Each seat's option, by seat (player 1's level is the game's own option).
constexpr std::array<const settings::Option*, scopes::kMaxPlayers> kSeatOptions{nullptr, &settings::kResponsivenessTwo,
                                                                                &settings::kResponsivenessThree, nullptr};
static_assert(settings::kResponsivenessWords.size() == facts::kResponsivenessMax - facts::kResponsivenessMin + 1,
              "one word per level of the game's Responsiveness");

struct Seat
{
    std::uint8_t          level = 0;  // the game's level for the seat; 0 for a seat without an option
    platform::ByteContext context;
    std::uintptr_t        definedAt = 0;  // the live byte the context is defined over
    std::uint64_t         opened    = 0;
    std::uint64_t         undone    = 0;  // game writes inside a seat's step, put back
};

// Game thread.
std::array<Seat, scopes::kMaxPlayers> g_seats;
bool                                  g_undoneWarned = false;

std::optional<std::uint8_t> LevelOf(const std::wstring& word) noexcept
{
    for (std::size_t i = 0; i < settings::kResponsivenessWords.size(); ++i)
    {
        if (word == settings::kResponsivenessWords[i])
        {
            return static_cast<std::uint8_t>(facts::kResponsivenessMin + i);
        }
    }
    return std::nullopt;
}

const wchar_t* WordOf(std::uint8_t level) noexcept
{
    return level >= facts::kResponsivenessMin && level <= facts::kResponsivenessMax
               ? settings::kResponsivenessWords[level - facts::kResponsivenessMin]
               : L"none";
}

void SetLevel(int seat, std::uint8_t level, const wchar_t* why)
{
    g_seats[static_cast<std::size_t>(seat)].level = level;
    platform::Log(L"responsiveness: player {} at {} ({})", seat + 1, level, why);
}

/// The live address of the game's Responsiveness byte, or 0 while the game has no data singleton.
std::uintptr_t LiveByte() noexcept
{
    void* const data = game_globals::Object(game_globals::Global::DataSingleton);
    return data != nullptr ? reinterpret_cast<std::uintptr_t>(data) + facts::kDataResponsiveness : 0;
}

/// The context over `live`, defined anew when the byte moved (the singleton made after the last define).
bool DefineOver(Seat& seat, std::uintptr_t live) noexcept
try
{
    if (seat.definedAt == live)
    {
        return true;
    }
    const platform::ByteContext::Range range{live, 1, platform::ByteContext::Kind::Hold};
    if (!seat.context.Define(std::span(&range, 1)))
    {
        return false;
    }
    seat.definedAt = live;
    return true;
}
catch (...)
{
    return false;
}

}  // namespace

bool Install() noexcept
try
{
    for (int seat = 1; seat < scopes::kMaxPlayers; ++seat)
    {
        const settings::Option* const option = kSeatOptions[static_cast<std::size_t>(seat)];
        if (option == nullptr)
        {
            continue;
        }
        const auto level = LevelOf(settings::Of(*option).word);
        SetLevel(seat, level.value_or(LevelOf(option->fallback).value_or(facts::kResponsivenessMin)),
                 settings::Why(*option).c_str());
        control::RegisterOption(
            *option,
            [seat](const std::wstring& word) {
                const auto chosen = LevelOf(word);
                if (!chosen)
                {
                    return control::Outcome{false, L"not a level from 1 to 5"};
                }
                SetLevel(seat, *chosen, L"set");
                return control::Outcome{};
            },
            [seat] { return std::wstring(WordOf(g_seats[static_cast<std::size_t>(seat)].level)); });
    }
    control::Register({.name  = L"responsiveness",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the game's Responsiveness byte (player 1's level) and players 2 and 3's levels, "
                                L"with the steps each was held for and the game writes put back",
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
    return true;
}
catch (...)
{
    return false;
}

Scope::Scope(int seat) noexcept : seat_(seat)
{
    if (seat <= 0 || seat >= scopes::kMaxPlayers)
    {
        return;
    }
    Seat&                seat_state = g_seats[static_cast<std::size_t>(seat)];
    const std::uintptr_t live       = LiveByte();
    if (seat_state.level == 0 || live == 0 || seat_state.context.IsOpen() || !DefineOver(seat_state, live))
    {
        return;
    }
    const auto value = seat_state.context.Value(0);
    if (value.empty())
    {
        return;
    }
    value[0] = seat_state.level;
    held_    = seat_state.context.Open();
    if (held_)
    {
        ++seat_state.opened;
    }
}

Scope::~Scope()
{
    if (!held_)
    {
        return;
    }
    Seat& seat_state = g_seats[static_cast<std::size_t>(seat_)];
    if (seat_state.context.Close() != 0)
    {
        ++seat_state.undone;
        if (!g_undoneWarned)
        {
            g_undoneWarned = true;
            platform::LogWarning(L"responsiveness: the game wrote its Responsiveness byte during player {}'s input step; put back "
                                 L"to player 1's (logged once)",
                                 seat_ + 1);
        }
    }
}

std::wstring Reading()
{
    std::uint8_t   one  = 0;
    const auto     live = LiveByte();
    std::wstring   text = live != 0 && platform::Read(reinterpret_cast<const void*>(live), one)
                              ? std::format(L"the game's byte (player 1) {}", one)
                              : std::wstring(L"the game's byte unread (no data singleton yet)");
    for (int seat = 1; seat < scopes::kMaxPlayers; ++seat)
    {
        const Seat& seat_state = g_seats[static_cast<std::size_t>(seat)];
        if (seat_state.level != 0)
        {
            text += std::format(L"; player {} {} (held for {} steps, {} game writes put back)", seat + 1, seat_state.level,
                                seat_state.opened, seat_state.undone);
        }
    }
    return text;
}

}  // namespace lss::players::seat_responsiveness
