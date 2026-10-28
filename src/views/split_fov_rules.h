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
// single-player picture. A 16:9 quarter, one monitor of a row, a full-height column and the single view all keep the
// engine's FOV unchanged (the user, 2026-10-27: "3 player FOV only for the player with the widest view").

#include <algorithm>
#include <cmath>

namespace lss::views::split_fov_rules {

/// The weight between keeping the single-player framing (0) and the single-player scale (1); Rebirth's by-eye middle.
inline constexpr float kDefaultWeight = 0.5f;

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

/// Whether the rule applies: the view is wider than the camera's image and shorter than the single-player picture.
inline bool Applies(const Shape& shape, float cameraAspect) noexcept
{
    if (!(shape.viewWidth > 0.0f) || !(shape.viewHeight > 0.0f) || !(cameraAspect > 0.0f))
    {
        return false;
    }
    const float ratio = HeightRatio(shape, cameraAspect);
    return shape.viewWidth / shape.viewHeight > cameraAspect * (1.0f + kAspectSlack) && ratio > 0.0f && ratio < 1.0f - kAspectSlack;
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
