#include "hud/view_prompt.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/FProperty.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/seats.h"
#include "players/unreal_call.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace lss::hud::view_prompt {

namespace {

using RC::Unreal::UFunction;
using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

using PostRenderFn = void (*)(void* hud);
using DrawStringFn = std::int32_t (*)(void* canvas, float x, float y, const wchar_t* text, const void* font,
                                      const float* color, float scale);

constexpr int kPlayers = players::scopes::kMaxPlayers;

// The look: a dark translucent box in the lower middle of the view, white lines, one in a warm highlight. Sizes are
// for a 1080-pixel-high view and scale with the view's height (the font's legacy size is small: RE-view-text.md §2.5).
constexpr float kBox[4]       = {0.0f, 0.0f, 0.0f, 0.65f};
constexpr float kText[4]      = {1.0f, 1.0f, 1.0f, 1.0f};
constexpr float kHighlight[4] = {1.0f, 0.85f, 0.35f, 1.0f};
constexpr float kScaleAt1080  = 2.2f;   // the font's scale in a 1080-pixel-high view
constexpr float kMinScale     = 1.2f;
constexpr float kLineAt1      = 20.0f;  // a line's height at scale 1, before the first draw reports the real one
constexpr float kPadAt1       = 8.0f;
constexpr float kBoxWidth     = 0.70f;  // of the view's width, centred
constexpr float kBoxTop       = 0.60f;  // of the view's height

// Set at install, then only read.
platform::VTableEntry g_entry;
PostRenderFn          g_postRender = nullptr;
DrawStringFn          g_drawString = nullptr;
std::uintptr_t        g_engine     = 0;  // &GEngine
UFunction*            g_drawRect   = nullptr;
std::int32_t          g_rectColor = -1, g_rectX = -1, g_rectY = -1, g_rectW = -1, g_rectH = -1;
bool                  g_installed  = false;

// Game thread.
std::array<void*, kPlayers>  g_controllers{};
std::array<Prompt, kPlayers> g_prompts{};
std::array<bool, kPlayers>   g_shown{};
std::array<bool, kPlayers>   g_logged{};  // the first draw in that view logged
float                        g_lineAt1 = kLineAt1;
std::uint64_t                g_draws = 0, g_noFont = 0, g_noCanvas = 0;

/// The offsets of the reflected `HUD:DrawRect`'s parameters, by name (RE-view-text.md §2.4 lays them out from the dump;
/// read here so a different layout is refused rather than written into).
bool LookUpDrawRect()
{
    g_drawRect = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.HUD:DrawRect"));
    if (g_drawRect == nullptr || g_drawRect->GetParmsSize() > 64)
    {
        g_drawRect = nullptr;
        return false;
    }
    const auto offset = [](const wchar_t* name, std::int32_t size) {
        auto* property = g_drawRect->FindProperty(RC::Unreal::FName(name, RC::Unreal::FNAME_Find));
        return property != nullptr && property->GetSize() == size ? property->GetOffset_Internal() : -1;
    };
    g_rectColor = offset(STR("RectColor"), 16);
    g_rectX     = offset(STR("ScreenX"), 4);
    g_rectY     = offset(STR("ScreenY"), 4);
    g_rectW     = offset(STR("ScreenW"), 4);
    g_rectH     = offset(STR("ScreenH"), 4);
    if (g_rectColor < 0 || g_rectX < 0 || g_rectY < 0 || g_rectW < 0 || g_rectH < 0)
    {
        g_drawRect = nullptr;
        return false;
    }
    return true;
}

void DrawRect(void* hud, float x, float y, float w, float h) noexcept
{
    if (g_drawRect == nullptr)
    {
        return;
    }
    alignas(16) std::uint8_t frame[64]{};
    std::memcpy(frame + g_rectColor, kBox, sizeof(kBox));
    std::memcpy(frame + g_rectX, &x, 4);
    std::memcpy(frame + g_rectY, &y, 4);
    std::memcpy(frame + g_rectW, &w, 4);
    std::memcpy(frame + g_rectH, &h, 4);
    players::GuardedProcessEvent(static_cast<UObject*>(hud), g_drawRect, frame);
}

int SeatOf(const void* controller) noexcept
{
    for (int seat = 0; seat < kPlayers && controller != nullptr; ++seat)
    {
        if (g_controllers[static_cast<std::size_t>(seat)] == controller)
        {
            return seat;
        }
    }
    return -1;
}

/// After the game's own HUD pass of one player's view: that player's prompt, if any.
void Draw(void* hud) noexcept
{
    void* const canvas = platform::ReadPointer(hud, facts::kHudCanvas);
    const int   seat   = SeatOf(platform::ReadPointer(hud, facts::kHudOwner));
    if (seat < 0 || !g_shown[static_cast<std::size_t>(seat)])
    {
        return;
    }
    float clipX = 0, clipY = 0;
    void* const fcanvas = canvas != nullptr ? platform::ReadPointer(canvas, facts::kCanvasFCanvas) : nullptr;
    if (fcanvas == nullptr || !platform::Read(static_cast<const std::uint8_t*>(canvas) + facts::kCanvasClipX, clipX) ||
        !platform::Read(static_cast<const std::uint8_t*>(canvas) + facts::kCanvasClipY, clipY) || !(clipX > 1.0f) ||
        !(clipY > 1.0f))
    {
        ++g_noCanvas;
        return;
    }
    void* const engine = platform::ReadPointer(reinterpret_cast<const void*>(g_engine), 0);
    void* const font   = engine != nullptr ? platform::ReadPointer(engine, facts::kEngineMediumFont) : nullptr;
    if (font == nullptr)
    {
        ++g_noFont;
        return;
    }
    const Prompt& prompt = g_prompts[static_cast<std::size_t>(seat)];
    int           lines  = 0;
    for (const std::wstring& line : prompt.lines)
    {
        lines += line.empty() ? 0 : 1;
    }
    const float scale = std::max(kMinScale, clipY / 1080.0f * kScaleAt1080);
    const float pad   = kPadAt1 * scale;
    const float lineH = g_lineAt1 * scale;
    const float boxX  = clipX * (1.0f - kBoxWidth) / 2.0f;
    const float boxY  = clipY * kBoxTop;
    DrawRect(hud, boxX, boxY, clipX * kBoxWidth, lineH * static_cast<float>(lines) + 2.0f * pad);
    float y = boxY + pad;
    for (int i = 0; i < kLines; ++i)
    {
        const std::wstring& line = prompt.lines[static_cast<std::size_t>(i)];
        if (line.empty())
        {
            continue;
        }
        std::int32_t height = 0;
        platform::CallGuarded(g_drawString, height, fcanvas, boxX + 2.0f * pad, y, line.c_str(),
                              static_cast<const void*>(font), i == prompt.highlight ? kHighlight : kText, scale);
        if (height > 0)
        {
            g_lineAt1 = static_cast<float>(height) / scale;  // the font's real line height, for the box from now on
        }
        y += height > 0 ? static_cast<float>(height) : lineH;
    }
    ++g_draws;
    if (!g_logged[static_cast<std::size_t>(seat)])
    {
        g_logged[static_cast<std::size_t>(seat)] = true;
        platform::Log(L"view prompt: drawn in {}'s view ({:.0f} x {:.0f} pixels, text scale {:.2f})",
                      players::SeatName(seat), clipX, clipY, scale);
    }
}

/// The HUD's PostRender (vtbl_AHUD +0x660): the game's, then the prompt.
void PostRenderFor(void* hud) noexcept
{
    g_postRender(hud);
    try
    {
        bool any = false;
        for (const bool shown : g_shown)
        {
            any = any || shown;
        }
        if (any && hud != nullptr)
        {
            Draw(hud);
        }
    }
    catch (...)
    {
    }
}

std::wstring Reading()
{
    std::wstring shown;
    for (int seat = 0; seat < kPlayers; ++seat)
    {
        if (g_shown[static_cast<std::size_t>(seat)])
        {
            shown += std::format(L"{}{}", shown.empty() ? L"" : L", ", players::SeatName(seat));
        }
    }
    return std::format(L"{}; prompts in: {}; drawn {}, no font {}, no canvas {}; box {}",
                       g_installed ? L"on" : L"not installed", shown.empty() ? L"none" : shown, g_draws, g_noFont,
                       g_noCanvas, g_drawRect != nullptr ? L"the HUD's DrawRect" : L"none (DrawRect not found)");
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
try
{
    const auto drawString = platform::FindFunction(image, facts::kDrawShadowedString);
    const auto postRender = platform::FindFunction(image, facts::kHudPostRender);
    const auto engine     = image.Rebase(facts::kGEngine, sizeof(void*));
    if (!drawString || !postRender || !engine)
    {
        platform::Log(L"view prompt: an engine check failed; no prompt can be drawn in a player's view");
        return false;
    }
    g_drawString = reinterpret_cast<DrawStringFn>(*drawString);
    g_postRender = reinterpret_cast<PostRenderFn>(*postRender);
    g_engine     = *engine;
    if (!LookUpDrawRect())
    {
        platform::Log(L"view prompt: the HUD's DrawRect was not found as expected; prompts are drawn without their box");
    }
    if (!platform::ReplaceSlot(image, facts::kHudPostRenderSlot, reinterpret_cast<std::uintptr_t>(&PostRenderFor), g_entry))
    {
        platform::Log(L"view prompt: the HUD pass could not be taken; no prompt can be drawn in a player's view");
        return false;
    }
    g_installed = true;
    control::Register({.name  = L"views.prompt",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the prompts shown in the players' views (the pad claim's) and how often drawn",
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
    platform::Log(L"view prompt: each player's HUD pass can draw a prompt in that player's view");
    return true;
}
catch (...)
{
    return false;
}

void Remove() noexcept
{
    g_shown = {};
    g_entry.Restore();
    g_installed = false;
}

void SetControllers(const std::array<void*, players::scopes::kMaxPlayers>& controllers) noexcept
{
    g_controllers = controllers;
}

void Show(int seat, const Prompt& prompt) noexcept
try
{
    if (seat < 0 || seat >= kPlayers)
    {
        return;
    }
    g_prompts[static_cast<std::size_t>(seat)] = prompt;
    g_shown[static_cast<std::size_t>(seat)]   = true;
}
catch (...)
{
}

void ClearAll() noexcept
{
    g_shown = {};
}

}  // namespace lss::hud::view_prompt
