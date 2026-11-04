#pragma once

// The split FOV rule as arithmetic, apart from the engine so it is tested on the host (views/split_fov.h applies it).
//
// The game's cameras give a horizontal FOV for a 16:9 image. With the constraint cleared, a view takes its whole part of
// the window and the engine keeps that FOV as the view's horizontal one (MaintainXFOV), so a view as wide as the screen
// and half as tall (two players in one window, the top view of three) shows the single-player width and half its height:
// characters cut at the knees. Keeping the single-player height instead would make everything half size. Rebirth's rule
// (views/view_shape.h SplitFov, chosen by eye 2026-09-25) takes the geometric middle:
//     tan(vertical / 2) = tan(single-player vertical / 2) * heightRatio^weight      (weight 0.5)
// with heightRatio the view's height over the single-player picture's. Remake improves on it in two ways
// (research/RE-view-size-fov.md §4.4, §7 item 3): the single-player picture is the camera's image fitted into the
// window (min(window height, window width / camera aspect)), so a window of another shape keeps the single-player
// framing; and the rule applies only where it is needed: a view wider than the camera's image and shorter than the
// single-player picture. A 16:9 quarter, a 16:9 monitor of a row, a narrow full-height column and the single view all
// keep the engine's FOV unchanged (the user, 2026-10-27: "3 player FOV only for the player with the widest view").
// v0.147.0 adds a span's monitor wider than 16:9 (a full-height column of a wider window): its height ratio is 1, so
// the same formula keeps the camera's vertical FOV there (Hor+), whatever the weight.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace lss::views::split_fov_rules {

/// The weight between keeping the single-player framing (0) and the single-player scale (1); Rebirth's by-eye middle.
inline constexpr float kDefaultWeight = 0.5f;

/// The weight a settings word stands for ("0.35", platform/settings.h kFovWeightWords); the default for any other text.
/// Read by its digits, so no locale's decimal mark can change it.
inline float WeightOf(std::wstring_view word) noexcept
{
    const auto digit = [word](std::size_t at) { return word[at] >= L'0' && word[at] <= L'9'; };
    if (word.size() != 4 || word[1] != L'.' || !digit(0) || !digit(2) || !digit(3))
    {
        return kDefaultWeight;
    }
    const int hundredths = (word[0] - L'0') * 100 + (word[2] - L'0') * 10 + (word[3] - L'0');
    return hundredths <= 100 ? static_cast<float>(hundredths) / 100.0f : kDefaultWeight;
}

/// A view shape the rule leaves alone: its width over height is within this fraction of the camera's aspect.
inline constexpr float kAspectSlack = 0.01f;

/// Inputs the rule reads, each in pixels or degrees.
struct Shape
{
    float viewWidth    = 0.0f;  // the view's part of the window
    float viewHeight   = 0.0f;
    float windowWidth  = 0.0f;  // the whole game viewport
    float windowHeight = 0.0f;
};

/// The view's height over the single-player picture's (the camera's image fitted into the window); 0 if unusable.
inline float HeightRatio(const Shape& shape, float cameraAspect) noexcept
{
    if (!(cameraAspect > 0.0f) || !(shape.windowWidth > 0.0f) || !(shape.windowHeight > 0.0f) || !(shape.viewHeight > 0.0f))
    {
        return 0.0f;
    }
    const float picture = std::min(shape.windowHeight, shape.windowWidth / cameraAspect);
    return picture > 0.0f ? shape.viewHeight / picture : 0.0f;
}

/// Whether the rule applies: the view is wider than the camera's image, and either shorter than the single-player
/// picture or a full-height column of a wider window. The column is a span's monitor wider than 16:9 (the 3440 between
/// two 2560s, v0.147.0): at its full height the rule keeps the camera's vertical FOV, so it shows more to the sides at
/// the 16:9 monitors' scale instead of the same width zoomed in (the user, 2026-11-02: each view fits the width of its
/// monitor). A single view filling the window is left to the engine.
inline bool Applies(const Shape& shape, float cameraAspect) noexcept
{
    if (!(shape.viewWidth > 0.0f) || !(shape.viewHeight > 0.0f) || !(cameraAspect > 0.0f))
    {
        return false;
    }
    const float ratio   = HeightRatio(shape, cameraAspect);
    const bool  wider   = shape.viewWidth / shape.viewHeight > cameraAspect * (1.0f + kAspectSlack);
    const bool  shorter = ratio > 0.0f && ratio < 1.0f - kAspectSlack;
    const bool  column  = ratio >= 1.0f - kAspectSlack && shape.viewWidth < shape.windowWidth;
    return wider && (shorter || column);
}

/// The view's horizontal FOV (degrees) from the camera's (`fovDegrees`, horizontal at `cameraAspect`); `fovDegrees`
/// unchanged where the rule does not apply or an input is unusable.
inline float SplitFov(float fovDegrees, float cameraAspect, const Shape& shape, float weight) noexcept
{
    constexpr float kHalfRadians = 3.14159265358979f / 360.0f;  // degrees to half-angle radians
    if (!std::isfinite(fovDegrees) || fovDegrees <= 1.0f || fovDegrees >= 179.0f || !std::isfinite(weight) || weight < 0.0f ||
        weight > 1.0f || !Applies(shape, cameraAspect))
    {
        return fovDegrees;
    }
    const float tanVertical = std::tan(fovDegrees * kHalfRadians) / cameraAspect * std::pow(HeightRatio(shape, cameraAspect), weight);
    const float result      = std::atan(tanVertical * shape.viewWidth / shape.viewHeight) / kHalfRadians;
    return std::isfinite(result) && result > 1.0f && result < 179.0f ? result : fovDegrees;
}

}  // namespace lss::views::split_fov_rules
