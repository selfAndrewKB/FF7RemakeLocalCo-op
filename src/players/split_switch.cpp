#include "players/split_switch.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/game_globals.h"

#include <cstdint>
#include <string>

namespace lss::players::split_switch {

namespace {

/// The game viewport client's switch byte, or null.
std::uint8_t* SwitchByte() noexcept
{
    const auto client = reinterpret_cast<std::uintptr_t>(game_globals::GameViewportClient());
    if (client == 0)
    {
        return nullptr;
    }
    return reinterpret_cast<std::uint8_t*>(client + facts::kViewportClientDisableSplitOverride);
}

}  // namespace

bool Install(const platform::GameImage&) noexcept
{
    if (game_globals::Address(game_globals::Global::Engine) == 0)
    {
        platform::Log(L"split switch: GEngine is outside the image; the engine's single view is unavailable (the split stays "
                      L"for the whole-screen moments)");
        return false;
    }
    control::Register({.name  = L"split-switch",
                       .shape = control::Shape::Word,
                       .words = {L"set", L"clear", L"unknown"},
                       .help  = L"the viewport's split switch (bDisableSplitScreenOverride): set = player 1's view alone, "
                                L"clear = the engine's split layout; unknown while it cannot be read",
                       .get   = [] {
                           const auto value = Read();
                           return control::Value{std::wstring(!value ? L"unknown" : *value ? L"set" : L"clear")};
                       }});
    platform::Log(L"split switch: the viewport's split switch (bDisableSplitScreenOverride) is the one-window single view's lever");
    return true;
}

std::optional<bool> Read() noexcept
{
    std::uint8_t value = 0;
    if (std::uint8_t* byte = SwitchByte(); byte != nullptr && platform::Read(byte, value))
    {
        return value != 0;
    }
    return std::nullopt;
}

bool Write(bool disabled) noexcept
{
    const std::uint8_t value = disabled ? 1 : 0;
    std::uint8_t*      byte  = SwitchByte();
    return byte != nullptr && platform::WriteGuarded(byte, &value, sizeof(value));
}

}  // namespace lss::players::split_switch
