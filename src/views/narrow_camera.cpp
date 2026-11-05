#include "views/narrow_camera.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/seat_camera.h"
#include "views/split_fov.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::views::narrow_camera {

namespace {

using platform::Read;

/// FRotator::RotateVector(this, FVector& out, const FVector& vec).
using RotateFn = float* (*)(const void* rotator, float* out, const float* vec);
/// UGameplayStatics::ProjectWorldToScreen(player, world, out, viewportRelative).
using ProjectFn = bool (*)(void* controller, const float* world, float* out, bool viewportRelative);

constexpr std::size_t kSeats    = column_rules::kMaxColumns;
constexpr float       kWholeView = 0.999f;  // a view this share of the window's width or more: nothing to correct

platform::CallRedirect g_rotateCall;
platform::CallRedirect g_projectCall;
RotateFn               g_rotate        = nullptr;
ProjectFn              g_project       = nullptr;
std::uintptr_t         g_projectSite   = 0;
std::uintptr_t         g_projectTarget = 0;
bool                   g_projectPlaced = false;

std::atomic<bool> g_leanOn{true};  // read by the crossing call's replacement; Remove clears it, from any thread

// Game thread.
bool                          g_scaleOn = true;
std::array<void*, kSeats>     g_operators{};
std::array<float, kSeats>     g_factors{1.0f, 1.0f, 1.0f};
std::array<std::uint32_t, kSeats> g_scaled{};
std::uint32_t                 g_leansFixed = 0;

/// The aim offset's copy with its sideways part scaled for the player whose operator this is.
float* RotateAimOffset(const void* rotator, float* out, const float* vec)
{
    const void* op = reinterpret_cast<const std::uint8_t*>(vec) - facts::kOperatorAimOffset;
    if (g_scaleOn)
    {
        for (std::size_t seat = 0; seat < kSeats; ++seat)
        {
            if (g_operators[seat] == op && g_factors[seat] < 1.0f)
            {
                const float copy[3]{vec[0], vec[1] * g_factors[seat], vec[2]};
                ++g_scaled[seat];
                return g_rotate(rotator, out, copy);
            }
        }
    }
    return g_rotate(rotator, out, vec);
}

/// The target's screen X against the whole window, measured from this view's own place.
bool ProjectForLean(void* controller, const float* world, float* out, bool viewportRelative)
{
    void* player = nullptr;
    float sizeX  = 0.0f;
    if (!g_leanOn.load(std::memory_order_relaxed) || !Read(static_cast<const std::uint8_t*>(controller) + facts::kControllerPlayer, player) || player == nullptr ||
        !Read(static_cast<const std::uint8_t*>(player) + facts::kLocalPlayerSize, sizeX) || !(sizeX > 0.0f) || sizeX >= kWholeView)
    {
        return g_project(controller, world, out, viewportRelative);
    }
    const bool projected = g_project(controller, world, out, true);
    if (projected)
    {
        out[0] /= sizeX;
        ++g_leansFixed;
    }
    return projected;
}

std::wstring Reading()
{
    std::wstring text = std::format(L"shoulder scale {} ({}), target lean fix {} ({}, corrected {})", g_scaleOn ? L"on" : L"off",
                                    g_rotateCall.IsInstalled() ? L"placed" : L"not placed",
                                    g_leanOn.load(std::memory_order_relaxed) ? L"on" : L"off",
                                    g_projectCall.IsInstalled() ? L"placed" : L"not placed", g_leansFixed);
    for (std::size_t seat = 0; seat < kSeats; ++seat)
    {
        float sideways = 0.0f;
        if (g_operators[seat] != nullptr &&
            Read(static_cast<const std::uint8_t*>(g_operators[seat]) + facts::kOperatorAimOffset + sizeof(float), sideways))
        {
            text += std::format(L"; player {}: factor {:.3f}, the game's sideways offset {:.1f}, calls scaled {}", seat + 1, g_factors[seat], sideways,
                                g_scaled[seat]);
        }
    }
    return text;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto rotate      = platform::FindFunction(image, facts::kRotateVector);
    const auto project     = platform::FindFunction(image, facts::kProjectWorldToScreen);
    const auto rotateSite  = image.Rebase(facts::kOperatorAimRotateCall, 5);
    const auto projectSite = image.Rebase(facts::kTargetLeanProjectCall, 5);
    if (!rotate || !project || !rotateSite || !projectSite)
    {
        platform::LogWarning(L"narrow camera: an engine check failed; narrow views keep the game's sideways offset and battle lean");
        return false;
    }
    g_rotate        = reinterpret_cast<RotateFn>(*rotate);
    g_project       = reinterpret_cast<ProjectFn>(*project);
    g_projectSite   = *projectSite;
    g_projectTarget = *project;
    if (!g_rotateCall.Install(*rotateSite, *rotate, reinterpret_cast<std::uintptr_t>(&RotateAimOffset)))
    {
        platform::LogWarning(L"narrow camera: the aim offset's call could not be redirected; narrow views keep the game's sideways offset");
    }
    using control::Value;
    control::Register({.name  = L"shoulder-scale",
                       .shape = control::Shape::OnOff,
                       .help  = L"a narrow view softens the camera's sideways offset by its width, no further than 0.7",
                       .set   = [](const Value& v) {
                           g_scaleOn = std::get<bool>(v);
                           platform::Log(L"narrow camera: shoulder scale {}", g_scaleOn ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return Value{g_scaleOn}; }});
    control::Register({.name  = L"target-lean-fix",
                       .shape = control::Shape::OnOff,
                       .help  = L"a view narrower than the window measures the battle camera's target from its own place, not the window's",
                       .set   = [](const Value& v) {
                           const bool on = std::get<bool>(v);
                           g_leanOn.store(on, std::memory_order_relaxed);
                           platform::Log(L"narrow camera: target lean fix {}", on ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return Value{g_leanOn.load(std::memory_order_relaxed)}; }});
    control::Register({.name  = L"narrow-camera.state",
                       .shape = control::Shape::Text,
                       .help  = L"each player's factor on the camera's sideways offset, the game's own offset, and what was corrected",
                       .get   = [] {
                           try
                           {
                               return Value{Reading()};
                           }
                           catch (...)
                           {
                               return Value{std::wstring(L"unreadable (an exception)")};
                           }
                       }});
    platform::Log(L"narrow camera: ready (the sideways offset {}; the battle lean's call is placed outside the world's tick)",
                  g_rotateCall.IsInstalled() ? L"follows a narrow view's width" : L"stays the game's");
    return true;
}

void Tick(const SeatPlayers& players) noexcept
{
    if (g_project == nullptr)
    {
        return;
    }
    if (!g_projectPlaced)
    {
        g_projectPlaced = true;  // once, whatever the outcome; outside the world's tick, as the crossing call needs
        if (g_projectCall.InstallFromOwnThread(g_projectSite, g_projectTarget, reinterpret_cast<std::uintptr_t>(&ProjectForLean)))
        {
            platform::Log(L"narrow camera: the battle lean's call is placed");
        }
        else
        {
            platform::LogWarning(L"narrow camera: the battle lean's call could not be redirected; views off the window's middle keep the lean");
        }
    }
    for (std::size_t seat = 0; seat < kSeats; ++seat)
    {
        g_operators[seat]  = seat == 0 ? players::seat_camera::OperatorOne() : players::seat_camera::Operator(static_cast<int>(seat));
        const float factor = split_fov::ShoulderFactor(players[seat]);
        if (factor != g_factors[seat])
        {
            platform::Log(L"narrow camera: player {}'s view now asks {:.3f} of the camera's sideways offset (shoulder scale {}, {})", seat + 1, factor,
                          g_scaleOn ? L"on" : L"off", g_rotateCall.IsInstalled() ? L"placed" : L"not placed");
            g_factors[seat] = factor;
        }
    }
}

void Remove() noexcept
{
    g_leanOn.store(false, std::memory_order_relaxed);  // the crossing call stays placed and passes through from here
    g_rotateCall.Restore();                            // inside one block: safe from any thread
}

}  // namespace lss::views::narrow_camera
