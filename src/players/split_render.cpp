#include "players/split_render.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"

namespace lss::players::split_render {

namespace {

std::uint32_t* g_forceOn = nullptr;                      // set once at install
std::uint32_t  g_mask    = facts::kSplitForcedFlags;    // game thread
std::uint32_t  g_written = 0;                           // what the mod last wrote (0 = nothing)

bool Write(std::uint32_t value) noexcept
{
    return g_forceOn != nullptr && platform::WriteGuarded(g_forceOn, &value, sizeof(value));
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto address = image.Rebase(facts::kShowFlagForce1Mask, sizeof(std::uint32_t));
    std::uint32_t value = 0;
    if (!address || !platform::CopyGuarded(&value, reinterpret_cast<const void*>(*address), sizeof(value)) || value != 0)
    {
        platform::Log(L"split render: the engine's show-flag force mask is missing or already in use; split halves keep the engine's reduced effects");
        return false;
    }
    g_forceOn = reinterpret_cast<std::uint32_t*>(*address);
    return true;
}

void OnJoin() noexcept
{
    if (g_forceOn == nullptr)
    {
        return;
    }
    const bool wrote = Write(g_mask);
    g_written        = wrote ? g_mask : 0;
    platform::Log(L"split render: show flags 0x{:X} forced on for both halves {}", g_mask, wrote ? L"" : L"(the write FAILED)");
}

void Tick(bool furtherViews) noexcept
{
    if (g_forceOn == nullptr)
    {
        return;
    }
    if (furtherViews)
    {
        if (g_written != g_mask)
        {
            OnJoin();  // a further view with the mask not yet written (a join whose first write failed, or a reinstall)
        }
        return;
    }
    std::uint32_t now = 0;
    if (g_written == 0 || !platform::CopyGuarded(&now, g_forceOn, sizeof(now)))
    {
        return;
    }
    if (now == g_written && Write(0))
    {
        platform::Log(L"split render: show-flag force mask back to 0 (no further view)");
    }
    else
    {
        platform::Log(L"split render: the force mask changed under the mod (0x{:X}); left alone", now);
    }
    g_written = 0;
}

void SetMask(std::uint32_t mask, bool playerTwoIn) noexcept
{
    g_mask = mask;
    if (playerTwoIn && g_forceOn != nullptr)
    {
        const bool wrote = Write(mask);
        g_written        = wrote ? mask : 0;
        platform::Log(L"split render: show flags forced on now 0x{:X}{}", mask, wrote ? L"" : L" (the write FAILED)");
    }
}

std::uint32_t Mask() noexcept
{
    return g_mask;
}

}  // namespace lss::players::split_render
