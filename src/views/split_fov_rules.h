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
// single-player picture. A 16:9 quarter, a 16:9 monitor of a row and the single view all keep the engine's FOV unchanged.
// A span's monitor wider than 16:9 (a full-height column of a wider window) has a height ratio of 1, so the same formula
// keeps the camera's vertical FOV there (Hor+), whatever the weight.
//
// A full-height view narrower than the game's image (two players side by side, the large view of three down one side) has
// the opposite trade. Its height ratio is 1, so the formula above has nothing to weigh; the width does:
//     tan(horizontal / 2) = tan(single-player horizontal / 2) * widthRatio^weight
// with widthRatio the view's width over the single-player picture's. Weight 1 is Rebirth's rule: the single-player scale and
// vertical FOV, the sides cropped (32.2 deg wide of a 60 deg camera at 8:9). Weight 0 is the whole single-player width in
// those pixels, everything half size. Between them are Rebirth's own measured blends (48.7 deg at 0.35, 44.4 at 0.5, 40.4 at
// 0.65; plan/topics/views.md in Rebirth), which its later rule dropped because the user picked 1 by eye; here they are the
// slider. A narrower view leaves any sideways camera offset a larger share of the screen, so ShoulderFactor softens that
// offset by the view's width, no further than Rebirth's by-eye minimum (views/narrow_camera.h).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace lss::views::split_fov_rules {

/// The weight between keeping the single-player framing (0) and the single-player scale (1); Rebirth's by-eye middle.
inline constexpr float kDefaultWeight = 0.5f;

/// A narrow view's weight between the whole single-player width (0) and the single-player scale (1): Rebirth's by-eye 1.
inline constexpr float kDefaultNarrowWeight = 1.0f;

/// The weight a settings word stands for ("0.35", platform/settings.h kFovWeightWords); `fallback` for any other text.
/// Read by its digits, so no locale's decimal mark can change it.
inline float WeightOf(std::wstring_view word, float fallback = kDefaultWeight) noexcept
{
    const auto digit = [word](std::size_t at) { return word[at] >= L'0' && word[at] <= L'9'; };
    if (word.size() != 4 || word[1] != L'.' || !digit(0) || !digit(2) || !digit(3))
    {
        return fallback;
    }
    const int hundredths = (word[0] - L'0') * 100 + (word[2] - L'0') * 10 + (word[3] - L'0');
    return hundredths <= 100 ? static_cast<float>(hundredths) / 100.0f : fallback;
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

/// The view's width over the single-player picture's (the camera's image fitted into the window); 0 if unusable.
inline float WidthRatio(const Shape& shape, float cameraAspect) noexcept
{
    if (!(cameraAspect > 0.0f) || !(shape.windowWidth > 0.0f) || !(shape.windowHeight > 0.0f) || !(shape.viewWidth > 0.0f))
    {
        return 0.0f;
    }
    const float picture = std::min(shape.windowWidth, shape.windowHeight * cameraAspect);
    return picture > 0.0f ? shape.viewWidth / picture : 0.0f;
}

/// Whether the narrow rule applies: narrower than the camera's image, as tall as the single-player picture and narrower than
/// it. A quarter of a 4:3 window (shorter) and a single view of any shape (as wide as the picture) are left alone.
inline bool AppliesNarrow(const Shape& shape, float cameraAspect) noexcept
{
    if (!(shape.viewWidth > 0.0f) || !(shape.viewHeight > 0.0f) || !(cameraAspect > 0.0f))
    {
        return false;
    }
    const float width = WidthRatio(shape, cameraAspect);
    return shape.viewWidth / shape.viewHeight < cameraAspect * (1.0f - kAspectSlack) &&
           HeightRatio(shape, cameraAspect) >= 1.0f - kAspectSlack && width > 0.0f && width < 1.0f - kAspectSlack;
}

/// A narrow view's horizontal FOV (degrees) from the camera's; `fovDegrees` unchanged where the rule does not apply or an
/// input is unusable.
inline float NarrowFov(float fovDegrees, float cameraAspect, const Shape& shape, float weight) noexcept
{
    constexpr float kHalfRadians = 3.14159265358979f / 360.0f;  // degrees to half-angle radians
    if (!std::isfinite(fovDegrees) || fovDegrees <= 1.0f || fovDegrees >= 179.0f || !std::isfinite(weight) || weight < 0.0f ||
        weight > 1.0f || !AppliesNarrow(shape, cameraAspect))
    {
        return fovDegrees;
    }
    const float result = std::atan(std::tan(fovDegrees * kHalfRadians) * std::pow(WidthRatio(shape, cameraAspect), weight)) / kHalfRadians;
    return std::isfinite(result) && result > 1.0f && result < 179.0f ? result : fovDegrees;
}

/// The game's cameras ask for this image; the camera's sideways offset is only ever asked of them.
inline constexpr float kImageAspect = 16.0f / 9.0f;

/// How far a narrow view softens the camera's sideways offset: Rebirth's by-eye choice between 0.5, 0.7 and 0.8 at 8:9.
inline constexpr float kShoulderMinimum = 0.7f;

/// The factor on the camera's sideways offset in this view: the view's width against the camera's (the ratio of the two
/// tangents, widthRatio^weight), kept within the minimum and 1. So 1 wherever the view is not narrow, and at weight 0.
inline float ShoulderFactor(const Shape& shape, float weight) noexcept
{
    if (!std::isfinite(weight) || weight < 0.0f || weight > 1.0f || !AppliesNarrow(shape, kImageAspect))
    {
        return 1.0f;
    }
    return std::clamp(std::pow(WidthRatio(shape, kImageAspect), weight), kShoulderMinimum, 1.0f);
}

}  // namespace lss::views::split_fov_rules
