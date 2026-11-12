#include "hud/ui_effect_pass.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "hud/overlay_band.h"
#include "hud/overlay_pass.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/seqlock.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <variant>

namespace lss::hud::ui_effect_pass {

namespace {

using DrawFn          = std::uint64_t (*)(void* renderer, void* view, void* commandList);
using AllocateChunkFn = void (*)(void* stack, std::int32_t minSize);

constexpr int kMaxColumns = overlay_pass::kMaxColumns;

/// FRHICommandSetViewport's fields as the game writes them: the corners unsigned, the depth range floats.
struct Viewport
{
    std::uint32_t minX, minY;
    float         minZ;
    std::uint32_t maxX, maxY;
    float         maxZ;
};
static_assert(sizeof(Viewport) == 0x18);

enum Outcome : int
{
    kNoneYet = 0,
    kNoColumn,
    kSwitchedOff,
    kNoBand,
    kBandNotApplying,
    kNoImageSize,
    kNotAppended,
    kWholeNotRestored,  // the box appended, the whole image after the draw not: the broadcast draws in the box
    kSet
};

/// A column's viewport as published: the box's centre, width and height as fractions of the image, and whether it
/// applies (1) or not (0). A column's box is the last the overlay published for it: one whose band did not apply this
/// frame gets none; one not recorded this frame keeps its last.
std::array<platform::SeqFloats<5>, kMaxColumns> g_boxes;

// Set at install, then only read.
platform::CallRedirect g_call;
std::uintptr_t         g_sceneTargets  = 0;  // g_SceneRenderTargets
std::uintptr_t         g_executeViewport = 0;
AllocateChunkFn        g_allocateChunk = nullptr;
std::atomic<bool>      g_on{true};        // the `hud-fx-pass` switch
std::atomic<bool>      g_installed{false};
std::atomic<int>       g_uiPassColumn{-1};  // render thread: the column whose UI image the view draws now
std::atomic<unsigned>  g_calls{0};          // the replacement's calls and viewports set
std::atomic<unsigned>  g_applied{0};
std::atomic<unsigned>  g_clamped{0};        // boxes cut at the image's edge
std::atomic<unsigned>  g_wholeNotRestored{0};
std::atomic<int>       g_lastColumn{-1};    // what the last call saw: its column and outcome
std::atomic<int>       g_lastOutcome{kNoneYet};
std::array<Viewport, kMaxColumns> g_logged{};  // render thread: each column's last logged box (a line per change)

/// Appends one FRHICommandSetViewport to the command list the way AllocBind appends its own: the memstack's top aligned
/// to 8, a new chunk when the command does not fit, the list's link and count moved on, the command's execute and fields
/// written. False, with nothing appended, when a read or write faults.
bool AppendViewport(std::uint8_t* list, const Viewport& viewport) noexcept
{
    std::uintptr_t top = 0, end = 0;
    if (list == nullptr || !platform::Read(list + facts::kRhiListStackTop, top) || !platform::Read(list + facts::kRhiListStackEnd, end))
    {
        return false;
    }
    top = (top + 7) & ~std::uintptr_t{7};
    if (top == 0 || top + facts::kRhiCommandSize > end)
    {
        if (!platform::CallGuarded(g_allocateChunk, static_cast<void*>(list + facts::kRhiListStackTop), static_cast<std::int32_t>(facts::kRhiChunkRequest)) ||
            !platform::Read(list + facts::kRhiListStackTop, top))
        {
            return false;
        }
        top = (top + 7) & ~std::uintptr_t{7};
        if (top == 0)
        {
            return false;
        }
    }
    const std::uintptr_t next    = top + facts::kRhiCommandSize;
    auto* const          command = reinterpret_cast<std::uint8_t*>(top);
    const std::uintptr_t zero    = 0;
    std::uintptr_t       link    = 0;
    std::int32_t         count   = 0;
    if (!platform::WriteGuarded(command, &zero, sizeof(zero)) ||
        !platform::WriteGuarded(command + facts::kRhiCommandExecute, &g_executeViewport, sizeof(g_executeViewport)) ||
        !platform::WriteGuarded(command + facts::kRhiViewportFields, &viewport, sizeof(viewport)) ||
        !platform::Read(list + facts::kRhiListCommandLink, link) || link == 0 || !platform::Read(list + facts::kRhiListCommandCount, count))
    {
        return false;
    }
    // The command is whole before the list sees it: the top, the previous command's Next, the link, the count.
    ++count;
    return platform::WriteGuarded(list + facts::kRhiListStackTop, &next, sizeof(next)) &&
           platform::WriteGuarded(reinterpret_cast<void*>(link), &top, sizeof(top)) &&
           platform::WriteGuarded(list + facts::kRhiListCommandLink, &top, sizeof(top)) &&
           platform::WriteGuarded(list + facts::kRhiListCommandCount, &count, sizeof(count));
}

/// The box on the image, in whole pixels: the published centre, width and height. A box starting above or left of the
/// image is cut at its edge (this build's viewport cannot start there: the box then draws smaller, the particles'
/// place in that column approximate), counted and logged with the box. False for a box without area.
bool BoxViewport(const std::array<float, 5>& box, std::uint32_t imageW, std::uint32_t imageH, int column, Viewport& out) noexcept
{
    const float width   = box[2] * static_cast<float>(imageW);
    const float height  = box[3] * static_cast<float>(imageH);
    const float centreX = box[0] * static_cast<float>(imageW);
    const float centreY = box[1] * static_cast<float>(imageH);
    const long  minX    = std::lround(centreX - width / 2.0f);
    const long  minY    = std::lround(centreY - height / 2.0f);
    const long  maxX    = std::lround(centreX + width / 2.0f);
    const long  maxY    = std::lround(centreY + height / 2.0f);
    const bool  clamped = minX < 0 || minY < 0;
    out                 = {static_cast<std::uint32_t>(std::max(minX, 0L)), static_cast<std::uint32_t>(std::max(minY, 0L)), 0.0f,
                           static_cast<std::uint32_t>(std::max(maxX, 0L)), static_cast<std::uint32_t>(std::max(maxY, 0L)), 1.0f};
    if (out.maxX <= out.minX || out.maxY <= out.minY)
    {
        return false;
    }
    if (clamped)
    {
        g_clamped.fetch_add(1, std::memory_order_relaxed);
    }
    Viewport& logged = g_logged[static_cast<std::size_t>(column)];
    if (logged.minX != out.minX || logged.minY != out.minY || logged.maxX != out.maxX || logged.maxY != out.maxY)
    {
        logged = out;
        platform::Log(L"UI effect pass: column {}'s UI particles placed by its box: viewport ({},{})-({},{}) of the {}x{} image{}", column + 1,
                      out.minX, out.minY, out.maxX, out.maxY, imageW, imageH,
                      clamped ? std::format(L" (its corner would be at ({},{}): cut at the image's edge, the particles' place "
                                            L"approximate in this column)",
                                            minX, minY)
                              : std::wstring());
    }
    return true;
}

/// The replacement, render thread: the column's box as the viewport for the game's draw, the whole image after it.
std::uint64_t Draw(void* renderer, void* view, void* commandList) noexcept
{
    const auto original = reinterpret_cast<DrawFn>(g_call.Original());
    const int  column   = g_uiPassColumn.load(std::memory_order_relaxed);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    g_lastColumn.store(column, std::memory_order_relaxed);
    int                  outcome = kSet;
    std::array<float, 5> box{};
    std::uint32_t        imageW = 0, imageH = 0;
    Viewport             viewport{};
    if (column < 0 || column >= kMaxColumns)
    {
        outcome = kNoColumn;
    }
    else if (!g_on.load(std::memory_order_relaxed))
    {
        outcome = kSwitchedOff;
    }
    else if (!g_boxes[static_cast<std::size_t>(column)].Read(box))
    {
        outcome = kNoBand;
    }
    else if (box[4] < 0.5f)
    {
        outcome = kBandNotApplying;
    }
    else if (!platform::Read(reinterpret_cast<const void*>(g_sceneTargets + facts::kSceneTargetsUiWidth), imageW) ||
             !platform::Read(reinterpret_cast<const void*>(g_sceneTargets + facts::kSceneTargetsUiHeight), imageH) || imageW == 0 || imageH == 0)
    {
        outcome = kNoImageSize;
    }
    else if (!BoxViewport(box, imageW, imageH, column, viewport) || !AppendViewport(static_cast<std::uint8_t*>(commandList), viewport))
    {
        outcome = kNotAppended;
    }
    const std::uint64_t result = original(renderer, view, commandList);
    if (outcome == kSet)
    {
        // The whole image again for the overlay broadcast that follows the draw.
        const Viewport whole{0, 0, 0.0f, imageW, imageH, 1.0f};
        if (AppendViewport(static_cast<std::uint8_t*>(commandList), whole))
        {
            g_applied.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            outcome = kWholeNotRestored;
            if (g_wholeNotRestored.fetch_add(1, std::memory_order_relaxed) == 0)
            {
                platform::LogWarning(L"UI effect pass: the whole image's viewport could not be appended after column {}'s draw; the frame's "
                                     L"overlay broadcast draws in the box",
                                     column + 1);
            }
        }
    }
    g_lastOutcome.store(outcome, std::memory_order_relaxed);
    g_uiPassColumn.store(-1, std::memory_order_relaxed);  // the next draw names its own column at its bind
    return result;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto site     = image.Rebase(facts::kUiPrimitivesDrawCall, 5);
    const auto target   = image.Rebase(facts::kUiPrimitivesDraw);
    const auto execute  = image.Rebase(facts::kRhiSetViewportExecute);
    const auto allocate = image.Rebase(facts::kMemStackAllocateChunk);
    const auto targets  = image.Rebase(facts::kSceneRenderTargetsGlobal + facts::kSceneTargetsUiHeight, sizeof(std::uint32_t));
    if (!site || !target || !execute || !allocate || !targets)
    {
        platform::Log(L"UI effect pass: UNAVAILABLE, an address is outside the image; the glow is drawn where the game draws it");
        return false;
    }
    g_sceneTargets    = *targets - facts::kSceneTargetsUiHeight;
    g_executeViewport = *execute;
    g_allocateChunk   = reinterpret_cast<AllocateChunkFn>(*allocate);
    if (!g_call.Install(*site, *target, reinterpret_cast<std::uintptr_t>(&Draw)))
    {
        platform::Log(L"UI effect pass: UNAVAILABLE, the UI particles' draw call is not what the research read; the glow is drawn where "
                      L"the game draws it");
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    control::Register({.name  = L"hud-fx-pass",
                       .shape = control::Shape::OnOff,
                       .help  = L"the UI particles (the summon gauge's glow) placed per column by its box (on, the default), or over the "
                                L"whole image as the engine does (off)",
                       .set   = [](const control::Value& value) {
                           SetEnabled(std::get<bool>(value));
                           return control::Outcome{true, L"from the next frame"};
                       },
                       .get = [] { return control::Value{Enabled()}; }});
    control::Register({.name  = L"hud-fx-pass.state",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the UI particles' draw redirect: calls, viewports set, boxes moved onto the image's edge, "
                                L"the column and outcome of the last call",
                       .get   = [] {
                           static constexpr const wchar_t* kOutcomes[] = {L"none yet",
                                                                          L"no column",
                                                                          L"switched off",
                                                                          L"no box published",
                                                                          L"a box that does not apply",
                                                                          L"no image size",
                                                                          L"the viewport command not appended",
                                                                          L"the whole image not appended after the draw",
                                                                          L"set"};
                           const int outcome = std::clamp(g_lastOutcome.load(), 0, static_cast<int>(kSet));
                           const int column  = g_lastColumn.load();
                           try
                           {
                               return control::Value{std::format(L"calls {}; set {}; cut at the edge {}; last column {}, {}", g_calls.load(),
                                                                 g_applied.load(), g_clamped.load(),
                                                                 column < 0 ? std::wstring(L"none") : std::to_wstring(column + 1), kOutcomes[outcome])};
                           }
                           catch (...)
                           {
                               return control::Value{std::wstring(L"unreadable (an exception)")};
                           }
                       }});
    platform::Log(L"UI effect pass: the UI particles (the summon glow) are drawn into each column's box");
    return true;
}

void Remove() noexcept
{
    if (!g_installed.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }
    platform::Log(L"UI effect pass: the engine's call {}", g_call.Restore() ? L"put back" : L"not ours any more; left alone");
}

void PublishColumnViewport(int column, const EffectBox& box, bool applies) noexcept
{
    if (column < 0 || column >= kMaxColumns)
    {
        return;
    }
    // The box within reason: a width and height of up to two images (a wide layout beside a narrow column's), a centre at
    // most an image away.
    const bool usable = applies && std::isfinite(box.centreX) && std::isfinite(box.centreY) && std::isfinite(box.width) &&
                        std::isfinite(box.height) && box.width > 0.0f && box.width <= 2.0f && box.height > 0.0f && box.height <= 2.0f &&
                        box.centreX > -1.0f && box.centreX < 2.0f && box.centreY > -1.0f && box.centreY < 2.0f;
    g_boxes[static_cast<std::size_t>(column)].Publish(usable ? std::array<float, 5>{box.centreX, box.centreY, box.width, box.height, 1.0f}
                                                             : std::array<float, 5>{0.5f, 0.5f, 1.0f, 1.0f, 0.0f});
}

void NoteUiPass(int column) noexcept
{
    g_uiPassColumn.store(column, std::memory_order_relaxed);
}

void SetEnabled(bool on) noexcept
{
    g_on.store(on, std::memory_order_relaxed);
}

bool Enabled() noexcept
{
    return g_on.load(std::memory_order_relaxed);
}

}  // namespace lss::hud::ui_effect_pass
