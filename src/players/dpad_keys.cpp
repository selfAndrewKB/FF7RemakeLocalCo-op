#include "players/dpad_keys.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/name_text.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace lss::players::dpad_keys {

namespace {

struct KeyGlobal
{
    std::uintptr_t ida  = 0;
    const wchar_t* name = nullptr;
};
constexpr std::array<KeyGlobal, 4> kGlobals{{{facts::kKeyDPadUp, L"Gamepad_DPad_Up"},
                                             {facts::kKeyDPadDown, L"Gamepad_DPad_Down"},
                                             {facts::kKeyDPadLeft, L"Gamepad_DPad_Left"},
                                             {facts::kKeyDPadRight, L"Gamepad_DPad_Right"}}};
static_assert(std::size(kDpadKeys) == kGlobals.size(), "one global per key, in the enum's order");

constexpr int kNameLookups = 600;  // frames the name table may still be silent before the check gives up

std::array<std::uintptr_t, 4> g_keys{};  // set at install, then only read
int                           g_checked = 0;  // game thread: 0 not yet, 1 named as the research read them, -1 not
int                           g_lookups = 0;

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    bool all = true;
    for (std::size_t i = 0; i < kGlobals.size(); ++i)
    {
        g_keys[i] = image.Rebase(kGlobals[i].ida, sizeof(std::uint64_t)).value_or(0);
        all       = all && g_keys[i] != 0;
    }
    return all;
}

std::uintptr_t Key(Dpad key) noexcept
{
    return g_keys[static_cast<std::size_t>(key)];
}

bool Name(Dpad key, std::uint64_t& out) noexcept
{
    const std::uintptr_t at = Key(key);
    return at != 0 && platform::Read(reinterpret_cast<const void*>(at), out);
}

bool Named() noexcept
{
    if (g_checked == 0)
    {
        g_checked = 1;
        for (const Dpad key : kDpadKeys)
        {
            std::uint64_t        bits     = 0;
            const bool           read     = Name(key, bits);
            const auto           name     = read ? platform::NameText(bits) : std::wstring();
            const wchar_t* const expected = kGlobals[static_cast<std::size_t>(key)].name;
            if (read && name.empty() && ++g_lookups < kNameLookups)
            {
                g_checked = 0;  // asked again next frame
                return false;
            }
            if (name != expected)
            {
                g_checked = -1;
                platform::Log(L"d-pad keys: the key global for {} {}; the reads that need the d-pad keys' names stand aside", expected,
                              name.empty() ? L"could not be read" : L"is not as the research read it");
                break;
            }
        }
    }
    return g_checked == 1;
}

}  // namespace lss::players::dpad_keys
