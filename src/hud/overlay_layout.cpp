#include "hud/overlay_layout.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "hud/image_entries.h"
#include "hud/layout_code.h"
#include "hud/overlay_band.h"
#include "hud/overlay_pass.h"
#include "hud/second_image.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace lss::hud::overlay_layout {

namespace {

constexpr int kMaxColumns = overlay_pass::kMaxColumns;

using GetRectFn        = IntRect* (*)(IntRect* out);
using FindOrAddEntryFn = void* (*)(void* slot, std::uintptr_t name, const IntRect* rect);
using SubmitSlotFn     = void (*)(void* renderer, std::uintptr_t pool, void* slot);

/// FVector2D by value: 8 bytes, so the x64 convention passes it packed in rcx, as the game's callers do.
struct Size2D
{
    float x = 0.0f;
    float y = 0.0f;
};
static_assert(sizeof(Size2D) == 8);
using ContainScaleFn = float (*)(Size2D size);

// Set once at install, then only read.
platform::CallRedirect g_rectCall;
platform::CallRedirect g_entryCall;
platform::CallRedirect g_submitCall;  // crosses a block, like the rect call: installed with it
std::uintptr_t         g_submitSite   = 0;
std::uintptr_t         g_submitTarget = 0;
std::uintptr_t         g_rectSite     = 0;
std::uintptr_t         g_rectTarget   = 0;
std::uintptr_t         g_overlay      = 0;  // &g_SEndRootOverlay
ContainScaleFn         g_containScale = nullptr;
bool                   g_installed    = false;

// The 16:9 assumptions (hud/layout_code.h: the frame, the field arrows, the battle arrows), acting while this byte is
// set: by the rect call, for the render it lays out, so it describes the UI size every HUD placement reads until the
// next render.
platform::CodeRedirect  g_frameCode;
platform::CodeRedirect  g_arrowCode;
platform::CodeRedirect  g_battleArrowCode;
alignas(8) std::uint8_t g_bandApplies = 0;

// Game thread only.
DWORD     g_gameThread  = 0;
bool      g_rectPending = false;  // the rect call waits for the game thread, outside the render
bool      g_splitShown  = false;
bool      g_bandOn      = true;
ViewShares g_shares;  // each column's view (the whole window for a column without one)

/// What the rect call decided for this render, for the entry call a few instructions later in the same function.
struct Pending
{
    bool           active = false;
    EntryPlacement place{};
    float          originX = 0.0f, originY = 0.0f, viewW = 0.0f, viewH = 0.0f;
    float          hostScale = 0.0f;
    ClipMap        clip{};
};
Pending g_pending;

/// Each column's whole marker host set (overlay +0x388..+0x3A8) as that column's pass last laid it out, for writing
/// outside the render (HostSetScope); invalid while the column's layout is not applied or the split is not shown.
struct HostSet
{
    bool       valid = false;
    HostValues values{};
};
HostSet g_hostSet[kMaxColumns];

// HostValues is the overlay's block as it lies, so it is read and written in one guarded copy.
static_assert(sizeof(HostValues) == facts::kOverlayHostScale + sizeof(float) - facts::kOverlayLayoutMin);
static_assert(offsetof(HostValues, origin) == facts::kOverlayHostOrigin - facts::kOverlayLayoutMin);
static_assert(offsetof(HostValues, ui) == facts::kOverlayUiSize - facts::kOverlayLayoutMin);
static_assert(offsetof(HostValues, size) == facts::kOverlayHostSize - facts::kOverlayLayoutMin);
static_assert(offsetof(HostValues, scale) == facts::kOverlayHostScale - facts::kOverlayLayoutMin);

/// The entry the layer-entry call placed in this render, for the submit call at its end: the per-child matrices
/// the paint pushed are complete only then.
struct Submit
{
    bool           active = false;
    std::uintptr_t entry  = 0;
    ClipMap        clip{};
};
Submit       g_submit;
std::int32_t g_loggedMatrices = -1;

/// What the log last said, so each change is logged once.
struct Logged
{
    IntRect image{};
    IntRect band{};
    float   viewW   = -1.0f;
    float   viewH   = -1.0f;
    int     applied = -1;
    int     seat    = -2;  // whose view the column is (a seat's change of column is logged too)
};
Logged      g_logged[kMaxColumns];  // per column: player 1's (and the single render), then the further columns'
bool        g_entryLogged[kMaxColumns] = {};
int         g_loggedColumn = 0;
std::wstring g_lastText    = L"not yet rendered";

float ReadFloat(std::uintptr_t address) noexcept
{
    float value = 0.0f;
    platform::CopyGuarded(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

void WriteFloat(std::uintptr_t address, float value) noexcept
{
    platform::WriteGuarded(reinterpret_cast<void*>(address), &value, sizeof(value));
}

std::uintptr_t Overlay() noexcept
{
    std::uintptr_t overlay = 0;
    platform::CopyGuarded(&overlay, reinterpret_cast<const void*>(g_overlay), sizeof(overlay));
    return overlay;
}

/// Replaces the render's call to EndUI__GetInGameUIRect (same signature).
IntRect* LayoutRect(IntRect* out)
{
    IntRect* result  = reinterpret_cast<GetRectFn>(g_rectCall.Original())(out);
    g_pending.active = false;
    g_submit.active  = false;
    g_bandApplies    = 0;
    if (!g_splitShown || GetCurrentThreadId() != g_gameThread)
    {
        return result;  // one view (its sets dropped by Tick), or another thread: the engine's own result
    }
    // The single render (the mirror in, no further pass) lays out for column 0, player 1's, as its own pass does.
    const int            column  = std::max(overlay_pass::Column(), 0);
    const std::uintptr_t overlay = Overlay();
    if (out == nullptr || out->minX != 0 || out->minY != 0 || overlay == 0)
    {
        g_hostSet[column].valid = false;  // an unexpected rect or no overlay: no band in this pass either
        return result;
    }
    // The host size the render stored just before this call: the game viewport widget's size, in window pixels.
    const float   hostW   = ReadFloat(overlay + facts::kOverlayHostSize);
    const float   hostH   = ReadFloat(overlay + facts::kOverlayHostSize + 4);
    const ViewShare& share   = g_shares[column];
    const float   viewW   = share.sizeX * hostW;
    const float   viewH   = share.sizeY * hostH;
    const IntRect image   = *out;
    const IntRect band    = VisibleBand(image.Width(), image.Height(), viewW, viewH);
    const float   uiScale = g_containScale({static_cast<float>(band.Width()), static_cast<float>(band.Height())});
    const float   host    = MarkerHostScale(band, uiScale, image.Width(), image.Height(), viewW, viewH);
    const auto    place   = PlaceInBand(band, image.Width(), image.Height());
    const bool    applied = g_bandOn && host > 0.0f && place.scaleX != 0.0f;
    g_loggedColumn = column;
    Logged& logged = g_logged[column];
    // Column 0 (and the single render) is player 1's; a further column is the seat the engine lists there (with player 2
    // out, player 3 fills column 1).
    const int seat = column == 0 ? 0 : overlay_pass::SeatOf(column);
    if (!(image == logged.image) || !(band == logged.band) || viewW != logged.viewW || viewH != logged.viewH ||
        static_cast<int>(applied) != logged.applied || seat != logged.seat)
    {
        logged                = {image, band, viewW, viewH, applied ? 1 : 0, seat};
        g_entryLogged[column] = false;
        g_lastText    = std::format(L"image {}x{}, {}'s view {:.0f}x{:.0f} at ({:.0f},{:.0f}); band ({},{})-({},{}), "
                                    L"layout scale {:.3f}, marker host scale {:.4f}: {}",
                                    image.Width(), image.Height(),
                                    seat >= 0 ? std::format(L"player {}", seat + 1) : std::format(L"column {}", column + 1),
                                    viewW, viewH, share.originX * hostW,
                                    share.originY * hostH, band.minX, band.minY, band.maxX, band.maxY, uiScale, host,
                                    applied ? L"applied" : g_bandOn ? L"NOT applied (unusable band)" : L"off (hud-band off)");
        platform::Log(L"HUD layout: {}", g_lastText);
    }
    if (applied)
    {
        *out          = band;
        g_bandApplies = 1;
        g_pending = {true, place, share.originX * hostW, share.originY * hostH, viewW, viewH, host,
                     BandClipMap(band, image.Width(), image.Height())};
        g_hostSet[column] = {true,
                             {{static_cast<float>(band.minX), static_cast<float>(band.minY)},
                              {share.originX * hostW, share.originY * hostH},
                              {static_cast<float>(band.Width()), static_cast<float>(band.Height())},
                              {viewW, viewH},
                              host}};
    }
    else
    {
        g_hostSet[column].valid = false;  // no band in this pass: nothing is placed outside the render with its set
    }
    return result;
}

/// Replaces the render's call to FEndMenuRenderer__FindOrAddNamedLayerEntry (same register arguments).
void* LayerEntry(void* slot, std::uintptr_t name, const IntRect* rect)
{
    // A further column's pass names its own entry (hud/second_image.h).
    void* entry = reinterpret_cast<FindOrAddEntryFn>(g_entryCall.Original())(
        slot, static_cast<std::uintptr_t>(second_image::EntryNameForPass(name)), rect);
    if (overlay_pass::Split())
    {
        image_entries::RecordEntry(overlay_pass::Column(), entry);
    }
    if (!g_pending.active || entry == nullptr || GetCurrentThreadId() != g_gameThread)
    {
        return entry;
    }
    g_pending.active             = false;
    const std::uintptr_t e       = reinterpret_cast<std::uintptr_t>(entry);
    const std::uintptr_t overlay = Overlay();
    if (!g_entryLogged[g_loggedColumn] && platform::Enabled(platform::Level::Debug))
    {
        g_entryLogged[g_loggedColumn] = true;
        platform::LogDebug(L"HUD layout: entry matrix x/y ({:.6f}, {:.6f}, {:.3f}, {:.3f}) -> ({:.6f}, {:.6f}, {:.3f}, {:.3f}); "
                           L"host ({:.0f},{:.0f}) {:.0f}x{:.0f} scale {:.4f} -> ({:.0f},{:.0f}) {:.0f}x{:.0f} scale {:.4f}",
                           ReadFloat(e + facts::kEntryMatrixScaleX), ReadFloat(e + facts::kEntryMatrixScaleY),
                           ReadFloat(e + facts::kEntryMatrixTranslateX), ReadFloat(e + facts::kEntryMatrixTranslateY),
                           g_pending.place.scaleX, g_pending.place.scaleY, g_pending.place.translateX,
                           g_pending.place.translateY, ReadFloat(overlay + facts::kOverlayHostOrigin),
                           ReadFloat(overlay + facts::kOverlayHostOrigin + 4), ReadFloat(overlay + facts::kOverlayHostSize),
                           ReadFloat(overlay + facts::kOverlayHostSize + 4), ReadFloat(overlay + facts::kOverlayHostScale),
                           g_pending.originX, g_pending.originY, g_pending.viewW, g_pending.viewH, g_pending.hostScale);
    }
    WriteFloat(e + facts::kEntryMatrixScaleX, g_pending.place.scaleX);
    WriteFloat(e + facts::kEntryMatrixScaleY, g_pending.place.scaleY);
    WriteFloat(e + facts::kEntryMatrixTranslateX, g_pending.place.translateX);
    WriteFloat(e + facts::kEntryMatrixTranslateY, g_pending.place.translateY);
    if (overlay != 0)
    {
        WriteFloat(overlay + facts::kOverlayHostOrigin, g_pending.originX);
        WriteFloat(overlay + facts::kOverlayHostOrigin + 4, g_pending.originY);
        WriteFloat(overlay + facts::kOverlayHostSize, g_pending.viewW);
        WriteFloat(overlay + facts::kOverlayHostSize + 4, g_pending.viewH);
        WriteFloat(overlay + facts::kOverlayHostScale, g_pending.hostScale);
    }
    g_submit = {true, e, g_pending.clip};
    return entry;
}

/// Moves the per-child view-projections onto the band (the equivalent of a viewport the draw lacks).
void PlaceChildMatrices(const Submit& submit) noexcept
{
    std::int32_t count = 0;
    float*       data  = nullptr;
    if (!platform::CopyGuarded(&count, reinterpret_cast<const void*>(submit.entry + facts::kEntryChildMatrixCount), sizeof(count)) ||
        !platform::CopyGuarded(&data, reinterpret_cast<const void*>(submit.entry + facts::kEntryChildMatrices), sizeof(data)) ||
        count < 0 || count > 4096 || (count > 0 && data == nullptr))
    {
        if (g_loggedMatrices != -2)
        {
            g_loggedMatrices = -2;
            platform::Log(L"HUD layout: the entry's child matrices do not look like a list (count {}); left alone", count);
        }
        return;
    }
    float first[16] = {};
    if (count > 0)
    {
        platform::CopyGuarded(first, data, sizeof(first));
    }
    for (std::int32_t i = 0; i < count; ++i)
    {
        ApplyClipMap(*reinterpret_cast<float(*)[16]>(data + static_cast<std::ptrdiff_t>(i) * 16), submit.clip);
    }
    if (count != g_loggedMatrices)
    {
        g_loggedMatrices = count;
        platform::LogDebug(L"HUD layout: {} per-child matrices placed in the band{}", count,
                           count > 0 ? std::format(L"; the first x/y/w columns ({:.4f} {:.4f} {:.4f} | {:.4f} {:.4f} {:.4f}) -> x/y "
                                                   L"({:.4f} {:.4f} | {:.4f} {:.4f})",
                                                   first[0], first[4], first[12], first[1], first[5], first[13], data[0],
                                                   data[12], data[5], data[13])
                                     : std::wstring());
    }
}

/// Replaces the render's call to FEndMenuRenderer__SubmitLayerSlot (same register arguments; no result).
void SubmitSlot(void* renderer, std::uintptr_t pool, void* slot)
{
    if (g_submit.active && GetCurrentThreadId() == g_gameThread)
    {
        g_submit.active = false;
        PlaceChildMatrices(g_submit);
    }
    if (overlay_pass::Further())
    {
        return;  // player 1's pass submits the slot holding every column's entry (hud/second_image.h)
    }
    reinterpret_cast<SubmitSlotFn>(g_submitCall.Original())(renderer, pool, slot);
}

/// Optional: the centred 1920x1080 frame is the whole layout in a band (hud/layout_code.h).
void InstallFrameCode(const platform::GameImage& image) noexcept
{
    namespace c     = layout_code;
    const auto site = image.Rebase(c::kFrameSite, sizeof(c::kFrameStore));
    const auto back = image.Rebase(c::kFrameResume);
    const bool held = site && back && image.BytesMatch(c::kFrameFunction, c::kFrameBytes) &&
                      g_frameCode.Install(*site, c::kFrameStore, c::FrameCode(g_bandApplies), *back);
    platform::Log(L"HUD layout: the centred 1920x1080 frame {}",
                  held ? L"is the whole layout in a band" : L"keeps the game's 16:9 box (its code differs)");
}

/// Optional: the off-screen arrows pick their edge by where the ray leaves a band (hud/layout_code.h).
void InstallArrowCode(const platform::GameImage& image) noexcept
{
    namespace c          = layout_code;
    const auto site      = image.Rebase(c::kArrowSite, sizeof(c::kArrowCompare));
    const auto side      = image.Rebase(c::kArrowSide);
    const auto topBottom = image.Rebase(c::kArrowTopBottom);
    const auto sixty     = image.Rebase(c::kArrowSixty, sizeof(float));
    const auto oneTwenty = image.Rebase(c::kArrowOneTwenty, sizeof(float));
    float      sixtyValue = 0.0f, oneTwentyValue = 0.0f;
    bool held = site && side && topBottom && sixty && oneTwenty && image.BytesMatch(c::kArrowCheckStart, c::kArrowBytes);
    if (held)
    {
        std::memcpy(&sixtyValue, reinterpret_cast<const void*>(*sixty), sizeof(float));
        std::memcpy(&oneTwentyValue, reinterpret_cast<const void*>(*oneTwenty), sizeof(float));
        held = sixtyValue == 60.0f && oneTwentyValue == 120.0f &&
               g_arrowCode.Install(*site, c::kArrowCompare,
                                   c::ArrowCode(g_bandApplies, *side, *topBottom, sixtyValue, oneTwentyValue),
                                   *site + sizeof(c::kArrowCompare));
    }
    platform::Log(L"HUD layout: the off-screen arrows {}", held ? L"pick their edge by where the ray leaves a band"
                                                                : L"keep the game's 16:9 edge split (its code differs)");
}

/// Optional: the battle's off-screen enemy arrows, the same rule in their own placement (hud/layout_code.h).
void InstallBattleArrowCode(const platform::GameImage& image) noexcept
{
    namespace c          = layout_code;
    const auto site      = image.Rebase(c::kBattleArrowSite, sizeof(c::kBattleArrowAdd));
    const auto resume    = image.Rebase(c::kBattleArrowResume);
    const auto side      = image.Rebase(c::kBattleArrowSide);
    const auto topBottom = image.Rebase(c::kBattleArrowTopBottom);
    const auto sixty     = image.Rebase(c::kArrowSixty, sizeof(float));
    float      sixtyValue = 0.0f;
    bool       held = site && resume && side && topBottom && sixty &&
                image.BytesMatch(c::kBattleArrowCheckStart, c::kBattleArrowBytes);
    if (held)
    {
        std::memcpy(&sixtyValue, reinterpret_cast<const void*>(*sixty), sizeof(float));
        held = sixtyValue == 60.0f &&
               g_battleArrowCode.Install(*site, c::kBattleArrowAdd, c::BattleArrowCode(g_bandApplies, *side, *topBottom, sixtyValue),
                                         *resume);
    }
    platform::Log(L"HUD layout: the battle's off-screen enemy arrows {}",
                  held ? L"pick their edge by where the ray leaves a band"
                       : L"keep the game's 16:9 edge split (its code differs)");
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto rectCall  = image.Rebase(facts::kInGameUIRectCall, 5);
    const auto rectFn    = image.Rebase(facts::kGetInGameUIRect);
    const auto entryCall = image.Rebase(facts::kLayerEntryCall, 5);
    const auto entryFn   = image.Rebase(facts::kFindOrAddLayerEntry);
    const auto overlay   = image.Rebase(facts::kRootOverlayGlobal, sizeof(void*));
    const auto submit    = image.Rebase(facts::kLayerSubmitCall, 5);
    const auto submitFn  = image.Rebase(facts::kSubmitLayerSlot);
    const auto contain   = platform::FindFunction(image, facts::kContainScale);
    if (!rectCall || !rectFn || !entryCall || !entryFn || !overlay || !contain || !submit || !submitFn)
    {
        platform::Log(L"HUD layout: the overlay's code is not what the research read; the HUD keeps the game's layout");
        return false;
    }
    g_overlay      = *overlay;
    g_containScale = reinterpret_cast<ContainScaleFn>(*contain);
    if (!g_entryCall.Install(*entryCall, *entryFn, reinterpret_cast<std::uintptr_t>(&LayerEntry)))
    {
        platform::Log(L"HUD layout: the overlay's layer-entry call could not be redirected; the HUD keeps the game's layout");
        return false;
    }
    g_submitSite   = *submit;
    g_submitTarget = *submitFn;
    g_rectSite    = *rectCall;
    g_rectTarget  = *rectFn;
    g_rectPending = true;  // until the game thread installs it, the band is never applied
    g_installed   = true;

    using control::Outcome;
    using control::Register;
    using control::Shape;
    using control::Value;
    Register({.name  = L"hud-band",
              .shape = Shape::OnOff,
              .help  = L"lay the HUD out in the band each half shows, markers on their objects (on at start)",
              .set   = [](const Value& value) {
                  SetBandEnabled(std::get<bool>(value));
                  return Outcome{};
              },
              .get = [] { return Value{BandEnabled()}; }});
    Register({.name  = L"hud.layout",
              .shape = Shape::Text,
              .help  = L"the HUD layout's last decision: image, view, band, layout and marker host scales",
              .get   = [] { return Value{g_lastText}; }});
    platform::Log(L"HUD layout: installed (hud-band on); the rect call follows on the game thread");
    InstallFrameCode(image);
    InstallArrowCode(image);
    InstallBattleArrowCode(image);
    return true;
}

void Remove() noexcept
{
    if (!g_installed)
    {
        return;
    }
    g_bandOn         = false;
    g_splitShown     = false;
    g_pending.active = false;
    g_rectPending    = false;
    g_bandApplies    = 0;  // whatever code stays in place does the game's own thing
    if (g_frameCode.IsInstalled() && !g_frameCode.Restore())
    {
        platform::Log(L"HUD layout: the centred frame's code was changed after us; left in place (it acts only in a band)");
    }
    if (g_arrowCode.IsInstalled() && !g_arrowCode.Restore())
    {
        platform::Log(L"HUD layout: the arrow edge code was changed after us; left in place (it acts only in a band)");
    }
    if (g_battleArrowCode.IsInstalled() && !g_battleArrowCode.Restore())
    {
        platform::Log(L"HUD layout: the battle arrows' edge code was changed after us; left in place (it acts only in a band)");
    }
    // A call crossing two blocks is restored only from the game thread; elsewhere it stays and passes the engine's
    // rect through (the split is off).
    const bool onGame = GetCurrentThreadId() == g_gameThread;
    const bool rect   = !g_rectCall.IsInstalled() || (onGame && g_rectCall.Restore());
    const bool submitBack = !g_submitCall.IsInstalled() || (onGame && g_submitCall.Restore());
    const bool entry = g_entryCall.Restore();
    if (!rect || !entry || !submitBack)
    {
        platform::Log(L"HUD layout: {} left in place, passing the engine's values through",
                      !rect ? L"the rect call" : !entry ? L"the layer-entry call" : L"the submit call");
    }
    g_installed = !(rect && entry && submitBack);
}

void Tick(bool splitShown, const ViewShares& shares, int count) noexcept
{
    for (int column = 0; column < kMaxColumns; ++column)
    {
        g_shares[column] = column < count ? shares[column] : ViewShare{};
    }
    if (!g_installed)
    {
        return;
    }
    g_gameThread = GetCurrentThreadId();
    if (g_rectPending)
    {
        g_rectPending = false;  // the game thread is outside the overlay render here, as InstallFromOwnThread requires
        const bool ok = g_rectCall.InstallFromOwnThread(g_rectSite, g_rectTarget, reinterpret_cast<std::uintptr_t>(&LayoutRect));
        platform::Log(L"HUD layout: the overlay's rect call {}",
                      ok ? L"installed from the game thread" : L"could not be redirected (the HUD keeps the game's layout)");
        const bool submitOk =
            ok && g_submitCall.InstallFromOwnThread(g_submitSite, g_submitTarget, reinterpret_cast<std::uintptr_t>(&SubmitSlot));
        if (ok && !submitOk)
        {
            platform::Log(L"HUD layout: the overlay's submit call could not be redirected: widgets painted in 3D (the "
                          L"command menu) stay outside the band");
        }
    }
    g_splitShown = splitShown && g_rectCall.IsInstalled();
    if (!g_splitShown)
    {
        // Every view lays out again before anything is placed with its set (the window may have changed meanwhile).
        bool dropped = false;
        for (HostSet& set : g_hostSet)
        {
            dropped   = dropped || set.valid;
            set.valid = false;
        }
        if (dropped)
        {
            platform::LogDebug(L"HUD layout: the split is not shown; the columns' host sets dropped (each lays out again "
                               L"before anything is placed with it)");
        }
    }
}

HostSetScope::HostSetScope() noexcept
{
    const std::uintptr_t overlay = Overlay();
    hasSaved_ = overlay != 0 && GetCurrentThreadId() == g_gameThread &&
                platform::CopyGuarded(&saved_, reinterpret_cast<const void*>(overlay + facts::kOverlayLayoutMin), sizeof(saved_));
}

HostSetScope::~HostSetScope()
{
    Restore();
}

bool HostSetScope::Lend(int column) noexcept
{
    if (!hasSaved_ || column < 0 || column >= kMaxColumns || !g_hostSet[column].valid ||
        GetCurrentThreadId() != g_gameThread)
    {
        return false;
    }
    const std::uintptr_t overlay = Overlay();
    if (overlay == 0)
    {
        return false;
    }
    lent_ = true;  // before the write: a write that faults part-way is put back too
    return platform::WriteGuarded(reinterpret_cast<void*>(overlay + facts::kOverlayLayoutMin), &g_hostSet[column].values,
                                  sizeof(HostValues));
}

bool HostSetScope::Restore() noexcept
{
    if (!lent_)
    {
        return true;
    }
    const std::uintptr_t overlay = Overlay();
    if (overlay == 0 ||
        !platform::WriteGuarded(reinterpret_cast<void*>(overlay + facts::kOverlayLayoutMin), &saved_, sizeof(saved_)))
    {
        return false;
    }
    lent_ = false;
    return true;
}

bool BandApplies() noexcept
{
    return g_bandApplies != 0;
}

void SetBandEnabled(bool on) noexcept
{
    g_bandOn = on;
}

bool BandEnabled() noexcept
{
    return g_bandOn;
}

}  // namespace lss::hud::overlay_layout
