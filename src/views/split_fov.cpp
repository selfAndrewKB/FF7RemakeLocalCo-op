#include "views/split_fov.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/seat_camera.h"
#include "views/split_fov_rules.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace lss::views::split_fov {

namespace {

using platform::Read;
namespace rules = split_fov_rules;

/// ULocalPlayer::GetViewPoint(this, FMinimalViewInfo& out, EStereoscopicPass pass).
using GetViewPointFn = void (*)(void* localPlayer, std::byte* viewInfo, std::int32_t pass);

platform::VTableEntry g_slot;
GetViewPointFn        g_engine = nullptr;

// Set from the console on the game thread, read by the replacement (the engine calls it from Draw on the game thread;
// atomics keep that true if it ever changes).
std::atomic<float> g_weight{rules::kDefaultWeight};
std::atomic<float> g_narrowWeight{rules::kDefaultNarrowWeight};

/// What a view's last call decided, for the Info line on each change. Game thread only (the replacement skips the
/// record off it). One record per local player the engine allows.
struct Seen
{
    const void*   player   = nullptr;
    std::uint64_t lastSeen = 0;  // the note counter at this view's last call (the least recently seen is reused)
    int         width   = 0;  // the view and the window, in pixels
    int         height  = 0;
    int         windowW = 0;
    int         windowH = 0;
    bool        applies = false;
    bool        axisRefused = false;  // the axis rule was not MaintainXFOV (logged once per value)
    std::uint8_t axis   = 0;
};
std::array<Seen, 4> g_seen;
std::uint64_t       g_notes = 0;  // counts the notes, for the records' last-seen order

/// The view's record, or (with `create`) a new one in an empty or the least recently seen record; null otherwise.
Seen* RecordOf(const void* player, bool create) noexcept
{
    ++g_notes;
    Seen* oldest = &g_seen[0];
    for (Seen& seen : g_seen)
    {
        if (seen.player == player)
        {
            seen.lastSeen = g_notes;
            return &seen;
        }
        if (seen.lastSeen < oldest->lastSeen)
        {
            oldest = &seen;  // an empty record has lastSeen 0, so it is taken first
        }
    }
    if (!create)
    {
        return nullptr;
    }
    *oldest          = {};
    oldest->player   = player;
    oldest->lastSeen = g_notes;
    return oldest;
}

/// The view's part of the window and the window, in pixels, as GetProjectionData sizes them (the player's Size
/// fractions times the viewport's pixels); false if a read fails.
bool ReadShape(const void* localPlayer, rules::Shape& shape) noexcept
{
    const auto* player = static_cast<const std::uint8_t*>(localPlayer);
    float       size[2]{};
    void*       client   = nullptr;
    void*       viewport = nullptr;
    std::int32_t pixels[2]{};
    if (!platform::CopyGuarded(size, player + facts::kLocalPlayerSize, sizeof(size)) ||
        !Read(player + facts::kLocalPlayerViewportClient, client) || client == nullptr ||
        !Read(static_cast<const std::uint8_t*>(client) + facts::kViewportClientViewport, viewport) || viewport == nullptr ||
        !platform::CopyGuarded(pixels, static_cast<const std::uint8_t*>(viewport) + facts::kViewportSizeX, sizeof(pixels)) ||
        pixels[0] <= 0 || pixels[1] <= 0 || !(size[0] > 0.0f) || !(size[1] > 0.0f) || size[0] > 1.0f || size[1] > 1.0f)
    {
        return false;
    }
    shape.windowWidth  = static_cast<float>(pixels[0]);
    shape.windowHeight = static_cast<float>(pixels[1]);
    shape.viewWidth    = std::trunc(size[0] * shape.windowWidth);  // as GetProjectionData sizes the view (TruncToInt)
    shape.viewHeight   = std::trunc(size[1] * shape.windowHeight);
    return shape.viewWidth > 0.0f && shape.viewHeight > 0.0f;
}

/// The Info line when a view starts or stops taking the rule, or changes shape while taking it.
void NoteShape(const void* localPlayer, const rules::Shape& shape, bool applies, float aspect, float fov, float split) noexcept
{
    if (!players::seat_camera::IsGameThread())
    {
        return;
    }
    Seen* seen = RecordOf(localPlayer, applies);  // a view that never takes the rule holds no record
    if (seen == nullptr)
    {
        return;
    }
    const int width = static_cast<int>(shape.viewWidth), height = static_cast<int>(shape.viewHeight);
    const int windowW = static_cast<int>(shape.windowWidth), windowH = static_cast<int>(shape.windowHeight);
    if (seen->applies == applies &&
        (!applies || (seen->width == width && seen->height == height && seen->windowW == windowW && seen->windowH == windowH)))
    {
        return;
    }
    if (applies && rules::AppliesNarrow(shape, aspect))
    {
        platform::Log(L"split FOV: a {}x{} view of a {}x{} window takes the narrow view's FOV (width {:.2f} of the single-player picture, "
                      L"weight {:.2f}): the camera's {:.1f} deg at {:.3f} becomes {:.1f} deg wide",
                      width, height, windowW, windowH, rules::WidthRatio(shape, aspect), g_narrowWeight.load(std::memory_order_relaxed),
                      fov, aspect, split);
    }
    else if (applies)
    {
        platform::Log(L"split FOV: a {}x{} view of a {}x{} window takes the split FOV (height {:.2f} of the single-player picture, "
                      L"weight {:.2f}): the camera's {:.1f} deg at {:.3f} becomes {:.1f} deg wide",
                      width, height, windowW, windowH, rules::HeightRatio(shape, aspect), g_weight.load(std::memory_order_relaxed),
                      fov, aspect, split);
    }
    else
    {
        platform::Log(L"split FOV: the {}x{} view of a {}x{} window keeps the engine's FOV (the shape of the camera's image, or the "
                      L"whole window at the single-player picture's size)",
                      width, height, windowW, windowH);
    }
    seen->applies = applies;
    seen->width   = width;
    seen->height  = height;
    seen->windowW = windowW;
    seen->windowH = windowH;
}

/// The axis rule is not MaintainXFOV: the FOV would not be read as the horizontal one, so the view is left alone.
void NoteAxis(const void* localPlayer, std::uint8_t axis) noexcept
{
    if (!players::seat_camera::IsGameThread())
    {
        return;
    }
    Seen* seen = RecordOf(localPlayer, true);
    if (seen == nullptr || (seen->axisRefused && seen->axis == axis))
    {
        return;
    }
    seen->axisRefused = true;
    seen->axis        = axis;
    platform::LogWarning(L"split FOV: a player's FOV axis rule is {} (not MaintainXFOV, 1); that view keeps the engine's FOV", axis);
}

void GetViewPoint(void* localPlayer, std::byte* viewInfo, std::int32_t pass)
{
    g_engine(localPlayer, viewInfo, pass);
    if (viewInfo == nullptr)
    {
        return;
    }
    std::uint8_t flags  = 0;
    std::uint8_t axis   = 0;
    float        fov    = 0.0f;
    float        aspect = 0.0f;
    if (!Read(viewInfo + facts::kViewInfoFlags, flags) || (flags & facts::kViewInfoConstrainBit) != 0 ||
        !Read(static_cast<const std::uint8_t*>(localPlayer) + facts::kLocalPlayerAxisRule, axis) ||
        !Read(viewInfo + facts::kViewInfoFov, fov) || !Read(viewInfo + facts::kViewInfoAspectRatio, aspect))
    {
        return;  // held to its camera's aspect (single player, a bar layout) or unreadable: the engine's view
    }
    rules::Shape shape;
    if (!ReadShape(localPlayer, shape))
    {
        return;
    }
    const bool wide    = rules::Applies(shape, aspect);
    const bool narrow  = rules::AppliesNarrow(shape, aspect);
    const bool applies = wide || narrow;
    if (applies && axis != facts::kAxisRuleMaintainXFov)
    {
        NoteAxis(localPlayer, axis);
        return;
    }
    const float split = wide     ? rules::SplitFov(fov, aspect, shape, g_weight.load(std::memory_order_relaxed))
                        : narrow ? rules::NarrowFov(fov, aspect, shape, g_narrowWeight.load(std::memory_order_relaxed))
                                 : fov;
    NoteShape(localPlayer, shape, applies, aspect, fov, split);
    if (split != fov)
    {
        std::memcpy(viewInfo + facts::kViewInfoFov, &split, sizeof(split));
        platform::LogTrace(L"split FOV: {:.0f}x{:.0f}: {:.2f} -> {:.2f} deg", shape.viewWidth, shape.viewHeight, fov, split);
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (g_slot.Active())
    {
        return false;  // already installed
    }
    const auto engine = platform::FindFunction(image, facts::kGetViewPoint.target);
    if (!engine)
    {
        platform::Log(L"split FOV: the engine's view point is not what the research read; views keep the engine's FOV");
        return false;
    }
    g_engine = reinterpret_cast<GetViewPointFn>(*engine);
    if (!platform::ReplaceSlot(image, facts::kGetViewPoint, reinterpret_cast<std::uintptr_t>(&GetViewPoint), g_slot))
    {
        platform::Log(L"split FOV: the view point's slot could not be replaced; views keep the engine's FOV");
        return false;
    }
    namespace settings = platform::settings;
    g_weight.store(rules::WeightOf(settings::Of(settings::kHorizontalFov).word), std::memory_order_relaxed);
    platform::Log(L"split FOV: horizontal split weight {:.2f} ({})", g_weight.load(std::memory_order_relaxed), settings::Why(settings::kHorizontalFov));
    control::RegisterOption(
        settings::kHorizontalFov,
        [](const std::wstring& word) {
            g_weight.store(rules::WeightOf(word), std::memory_order_relaxed);
            platform::Log(L"split FOV: horizontal split weight {}", word);
            for (Seen& seen : g_seen)
            {
                seen.applies = false;  // the next call logs the new FOV
            }
            return control::Outcome{};
        },
        [] { return std::format(L"{:.2f}", g_weight.load(std::memory_order_relaxed)); });
    g_narrowWeight.store(rules::WeightOf(settings::Of(settings::kVerticalFov).word, rules::kDefaultNarrowWeight), std::memory_order_relaxed);
    platform::Log(L"split FOV: vertical split weight {:.2f} ({})", g_narrowWeight.load(std::memory_order_relaxed), settings::Why(settings::kVerticalFov));
    control::RegisterOption(
        settings::kVerticalFov,
        [](const std::wstring& word) {
            g_narrowWeight.store(rules::WeightOf(word, rules::kDefaultNarrowWeight), std::memory_order_relaxed);
            platform::Log(L"split FOV: vertical split weight {}", word);
            for (Seen& seen : g_seen)
            {
                seen.applies = false;  // the next call logs the new FOV
            }
            return control::Outcome{};
        },
        [] { return std::format(L"{:.2f}", g_narrowWeight.load(std::memory_order_relaxed)); });
    control::Register({.name  = L"split-fov.state",
                       .shape = control::Shape::Text,
                       .help  = L"each view's last shape and whether it takes the split FOV",
                       .get   = [] {
                           std::wstring text;
                           for (const Seen& seen : g_seen)
                           {
                               if (seen.player != nullptr)
                               {
                                   text += std::format(L"{}{}x{} of {}x{}: {}{}", text.empty() ? L"" : L"; ", seen.width, seen.height, seen.windowW, seen.windowH,
                                                       seen.applies ? L"split FOV" : L"engine's FOV",
                                                       seen.axisRefused ? std::format(L" (axis rule {})", seen.axis) : L"");
                               }
                           }
                           return control::Value{text.empty() ? std::wstring(L"no view has taken the split FOV") : text};
                       }});
    platform::Log(L"split FOV: ready (a view wider than the game's image shows more height, a full-height narrower one less width)");
    return true;
}

float ShoulderFactor(const void* localPlayer) noexcept
{
    rules::Shape shape;
    return g_slot.Active() && localPlayer != nullptr && ReadShape(localPlayer, shape)
               ? rules::ShoulderFactor(shape, g_narrowWeight.load(std::memory_order_relaxed))
               : 1.0f;
}

void Remove() noexcept
{
    g_slot.Restore();
}

}  // namespace lss::views::split_fov
