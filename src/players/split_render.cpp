#include "players/split_render.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"

namespace lss::players::split_render {

namespace {

constexpr std::uint32_t kRetryTicks = 30;  // a failed write tried again twice a second at 60

std::uint32_t* g_forceOn = nullptr;                      // set once at install
std::uint32_t  g_mask    = facts::kSplitForcedFlags;    // game thread
std::uint32_t  g_written = 0;                           // what the mod last wrote (0 = nothing)
std::uint32_t  g_ticks   = 0;
const wchar_t* g_fault   = nullptr;                     // a failed write, logged once until one succeeds

bool Write(std::uint32_t value) noexcept
{
    return g_forceOn != nullptr && platform::WriteGuarded(g_forceOn, &value, sizeof(value));
}

void Fault(const wchar_t* why) noexcept
{
    if (g_fault != why)
    {
        g_fault = why;
        platform::LogWarning(L"split render: {}", why);
    }
}

/// The mask back to 0 if it still holds what the mod wrote. True when nothing of the mod's is left in it.
bool Clear(const wchar_t* why) noexcept
{
    std::uint32_t now = 0;
    if (g_forceOn == nullptr || g_written == 0)
    {
        return true;
    }
    if (!platform::CopyGuarded(&now, g_forceOn, sizeof(now)))
    {
        Fault(L"the show-flag force mask could not be read; tried again twice a second");
        return false;
    }
    if (now != g_written)
    {
        platform::Log(L"split render: the force mask changed under the mod (0x{:X}); left alone", now);
        g_written = 0;
        return true;
    }
    if (!Write(0))
    {
        Fault(L"the show-flag force mask could not be put back to 0; tried again twice a second");
        return false;
    }
    g_fault   = nullptr;
    g_written = 0;
    platform::Log(L"split render: show-flag force mask back to 0 ({})", why);
    return true;
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
    if (!Write(g_mask))
    {
        g_written = 0;
        Fault(L"the show-flag force mask could not be written; the further views keep the engine's reduced effects; tried again "
              L"twice a second");
        return;
    }
    const bool change = g_written != g_mask;
    g_written         = g_mask;
    g_fault           = nullptr;
    if (change)
    {
        platform::Log(L"split render: show flags 0x{:X} forced on for every view", g_mask);
    }
}

void Tick(bool furtherViews) noexcept
{
    if (g_forceOn == nullptr)
    {
        return;
    }
    ++g_ticks;
    if (furtherViews)
    {
        if (g_written != g_mask && g_ticks % kRetryTicks == 0)
        {
            OnJoin();  // a further view with the mask not written (a failed write, or a view the join did not report)
        }
        return;
    }
    if (g_written != 0 && (g_fault == nullptr || g_ticks % kRetryTicks == 0))
    {
        Clear(L"no further view");
    }
}

void Remove() noexcept
{
    Clear(L"the mod unloads");
}

void SetMask(std::uint32_t mask, bool furtherPlayerIn) noexcept
{
    g_mask = mask;
    if (furtherPlayerIn && g_forceOn != nullptr)
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
