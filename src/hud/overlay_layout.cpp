#include "hud/overlay_layout.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "hud/image_entries.h"
#include "hud/layout_code.h"
#include "hud/overlay_band.h"
#include "hud/overlay_pass.h"
#include "hud/root_placement.h"
#include "hud/second_image.h"
#include "hud/ui_effect_pass.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/game_thread.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
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
platform::CodeRedirect  g_scaleCode;           // the layout scale's contain rule (hud/layout_code.h): placed by Tick
std::uintptr_t          g_scaleSite   = 0;     // set at install when the render's code is what the research read
std::uintptr_t          g_scaleResume = 0;
alignas(8) std::uint8_t g_bandApplies = 0;

// Game thread only.
platform::GameThreadMark g_gameThread;
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
    IntRect        band{};
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

/// Each column's layout and band as its pass of this frame laid the overlay out (the band and its layout while the band
/// applied, else none), filled pass by pass (further columns first, column 0 last), read when column 0's pass, the
/// frame's last, records: each column then gets its UI particles' box against column 0's layout (hud/ui_effect_pass.h).
struct ColumnOverlay
{
    LayoutSize   layout = kSoloLayout;
    IntRect      band{};
    std::int32_t imageWidth  = 0;
    std::int32_t imageHeight = 0;
};
ColumnOverlay g_columnOverlay[kMaxColumns];
bool          g_columnSeen[kMaxColumns] = {};
bool          g_boxesPublished          = false;

// The UI effects' conversion reads the overlay's UI size twice; both reads answered (or neither).
using UiSizeFn = Size2D* (*)(Size2D* out);
platform::CallRedirect g_effectSizeCalls[2];

/// The entry the layer-entry call placed in this render, for the submit call at its end: the per-child matrices
/// the paint pushed are complete only then.
struct Submit
{
    bool           active = false;
    std::uintptr_t entry  = 0;
    ClipMap        clip{};
    IntRect        band{};
};
Submit       g_submit;
std::int32_t g_loggedMatrices = -1;
std::atomic<int> g_clipLogRenders{0};  // `hud.clip-log <n>`: the entry's clip states logged for that many band passes

// The area map's tiles in a band (`hud-band.map-tiles`, bits): 1 moves each tile quad's mask origin from the paint's pixels
// to the image's, where the pixel shader locates the mask under the pixel's position; 2 leaves the mask draws' own matrices
// out of the band map, since they draw into the mask target, not the image. Both on; the bits are for telling them apart.
constexpr int kMapTileOrigins  = 1;
constexpr int kMapTileMatrices = 2;
std::atomic<int> g_mapTiles{kMapTileOrigins | kMapTileMatrices};

/// What a walk of the entry's layers read, for the log.
struct WalkStats
{
    std::int32_t bitCount = 0;  // the further layers' allocation bits (0 for the overlay entry: its layer is the paint's)
    int          layers   = 0;
    int          elements = 0;
};

/// What the last tile placement of a column did, so each change is logged once (the element total is printed, not
/// compared: it moves with the HUD's own life).
struct TilesLogged
{
    int          quads    = -1;
    int          masks    = -1;
    int          shared   = -1;
    int          bits     = -1;
    int          layers   = -1;
    std::int32_t bitCount = -1;
    IntRect      band{};
    int          moved    = -1;  // the quads whose origin was written this pass
    bool         operator==(const TilesLogged&) const = default;
};
TilesLogged g_tilesLogged[kMaxColumns];

/// The per-child matrix list's bound (PlaceChildMatrices), and two sets over it rebuilt each band pass: the matrices
/// the pass leaves alone (the mask draws') and the matrices on-screen elements draw with, for noticing a shared index.
constexpr std::int32_t kMaxChildMatrices = 4096;
struct MatrixSet
{
    std::uint64_t words[kMaxChildMatrices / 64] = {};

    void Clear() noexcept { std::memset(words, 0, sizeof(words)); }
    bool Has(std::int32_t index) const noexcept
    {
        return index >= 0 && index < kMaxChildMatrices && ((words[index >> 6] >> (index & 63)) & 1u) != 0;
    }
    void Add(std::int32_t index) noexcept
    {
        if (index >= 0 && index < kMaxChildMatrices)
        {
            words[index >> 6] |= std::uint64_t{1} << (index & 63);
        }
    }
};
static_assert(sizeof(MatrixSet::words) * 8 == kMaxChildMatrices);
MatrixSet g_matrixSkip;
MatrixSet g_matrixOnScreen;

bool SkipsMatrix(std::int32_t index) noexcept
{
    return g_matrixSkip.Has(index);
}

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

/// Column 0's pass is the frame's last, so the overlay holds its layout at the widget tick that places the UI particles
/// (and the conversion's size reads answer it in the further passes, below): each column recorded this frame gets its
/// box against it; a column whose band did not apply gets none; one not recorded keeps its last.
void RecordColumn(int column, const LayoutSize& layout, const IntRect& band, std::int32_t imageWidth, std::int32_t imageHeight) noexcept
{
    g_columnOverlay[column] = {layout, band, imageWidth, imageHeight};
    g_columnSeen[column]    = true;
    if (column != 0)
    {
        return;
    }
    // Without column 0's band the conversion's size reads keep each pass's own size (below), so no column's box would
    // match: none then.
    const LayoutPoint anchor      = root_placement::GlowAnchor();
    const bool        columnZero = band.Height() > 0;
    for (int i = 0; i < kMaxColumns; ++i)
    {
        if (!g_columnSeen[i])
        {
            continue;
        }
        const ColumnOverlay& seen = g_columnOverlay[i];
        const auto           box  = columnZero && seen.band.Height() > 0
                                        ? EffectViewport(layout, seen.layout, seen.band, seen.imageWidth, seen.imageHeight, anchor)
                                        : std::nullopt;
        ui_effect_pass::PublishColumnViewport(i, box.value_or(EffectBox{}), box.has_value());
        g_columnSeen[i] = false;
    }
    g_boxesPublished = true;
}

/// No split shown: every column's box withdrawn, once, so the UI particles are drawn as the game draws them.
void DropBoxes() noexcept
{
    if (!g_boxesPublished)
    {
        return;
    }
    g_boxesPublished = false;
    for (int i = 0; i < kMaxColumns; ++i)
    {
        ui_effect_pass::PublishColumnViewport(i, EffectBox{}, false);
        g_columnSeen[i] = false;
    }
}

/// Replaces the UI effects' conversion's two reads of the overlay's UI size (same signature): player 1's column's
/// size during a further column's pass (its last band's), so every effect is converted in one layout, player 1's,
/// whichever pass placed it; the game's own read otherwise.
Size2D* UiSizeForEffects(Size2D* out)
{
    if (out != nullptr && g_gameThread.Here() && overlay_pass::Further() && g_hostSet[0].valid)
    {
        *out = {g_hostSet[0].values.ui[0], g_hostSet[0].values.ui[1]};
        return out;
    }
    return reinterpret_cast<UiSizeFn>(g_effectSizeCalls[0].Original())(out);
}

/// Replaces the render's call to EndUI__GetInGameUIRect (same signature).
IntRect* LayoutRect(IntRect* out)
{
    IntRect* result  = reinterpret_cast<GetRectFn>(g_rectCall.Original())(out);
    g_pending.active = false;
    g_submit.active  = false;
    g_bandApplies    = 0;
    if (!g_splitShown || !g_gameThread.Here())
    {
        if (g_gameThread.Here())
        {
            DropBoxes();
        }
        return result;  // one view (its sets dropped by Tick), or another thread: the engine's own result
    }
    // The single render (the mirror in, no further pass) lays out for column 0, player 1's, as its own pass does.
    const int            column  = std::max(overlay_pass::Column(), 0);
    const std::uintptr_t overlay = Overlay();
    if (out == nullptr || out->minX != 0 || out->minY != 0 || overlay == 0)
    {
        g_hostSet[column].valid = false;  // an unexpected rect or no overlay: no band in this pass either
        RecordColumn(column, kSoloLayout, IntRect{}, 0, 0);
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
                     BandClipMap(band, image.Width(), image.Height()), band};
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
    RecordColumn(column, applied ? LayoutSizeOf(band) : kSoloLayout, applied ? band : IntRect{}, image.Width(), image.Height());
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
    if (!g_pending.active || entry == nullptr || !g_gameThread.Here())
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
    g_submit = {true, e, g_pending.clip, g_pending.band};
    return entry;
}

constexpr std::int32_t kMaxLayerElements = 4096;  // a layer's element count beyond which it is not read
constexpr int          kMaxWalkedLayers  = 16;    // the layers one walk reads; more are left alone, never read twice

/// The layers a walk has visited, so a layer reachable twice (the paint's layer among the records) is read once:
/// the origin move must not be applied twice. Full, it admits no more.
struct VisitedLayers
{
    std::uintptr_t layers[kMaxWalkedLayers] = {};
    int            count                    = 0;

    bool Add(std::uintptr_t layer) noexcept
    {
        for (int i = 0; i < count; ++i)
        {
            if (layers[i] == layer)
            {
                return false;
            }
        }
        if (count >= kMaxWalkedLayers)
        {
            return false;
        }
        layers[count++] = layer;
        return true;
    }
};

/// Calls `fn(element)` for each draw element of one layer, read under guards and bounded; a layer that does not read as
/// one, or was walked already, is skipped.
template <typename Fn>
void ForEachLayerElement(std::uintptr_t layer, VisitedLayers& visited, WalkStats& stats, Fn&& fn) noexcept
{
    std::uintptr_t elements = 0;
    std::int32_t   count    = 0;
    if (layer == 0 || !visited.Add(layer) ||
        !platform::CopyGuarded(&elements, reinterpret_cast<const void*>(layer + facts::kLayerElements), sizeof(elements)) ||
        !platform::CopyGuarded(&count, reinterpret_cast<const void*>(layer + facts::kLayerElementCount), sizeof(count)) ||
        count <= 0 || count > kMaxLayerElements || elements == 0)
    {
        return;
    }
    ++stats.layers;
    stats.elements += count;
    for (std::int32_t k = 0; k < count; ++k)
    {
        fn(elements + static_cast<std::uintptr_t>(k) * facts::kElementSize);
    }
}

/// Calls `fn(element)` for each draw element of the entry: the layer the paint adds to, then the further layers (the
/// renderer's own walk).
template <typename Fn>
void ForEachElement(std::uintptr_t entry, WalkStats& stats, Fn&& fn) noexcept
{
    std::int32_t   bitCount = 0;
    std::uintptr_t records = 0, secondary = 0, current = 0;
    std::uint32_t  inlineBits[4] = {};
    VisitedLayers  visited;
    stats = {};
    if (platform::CopyGuarded(&current, reinterpret_cast<const void*>(entry + facts::kEntryCurrentLayer), sizeof(current)))
    {
        ForEachLayerElement(current, visited, stats, fn);
    }
    if (!platform::CopyGuarded(&bitCount, reinterpret_cast<const void*>(entry + facts::kEntryLayerBitCount), sizeof(bitCount)) ||
        !platform::CopyGuarded(&records, reinterpret_cast<const void*>(entry + facts::kEntryLayers), sizeof(records)) ||
        !platform::CopyGuarded(&secondary, reinterpret_cast<const void*>(entry + facts::kEntryLayerBitsSecondary), sizeof(secondary)) ||
        !platform::CopyGuarded(inlineBits, reinterpret_cast<const void*>(entry + facts::kEntryLayerBitsInline), sizeof(inlineBits)))
    {
        return;
    }
    stats.bitCount = bitCount;
    if (bitCount <= 0 || bitCount > 1024 || records == 0)
    {
        return;
    }
    std::uint32_t word = 0;
    for (std::int32_t i = 0; i < bitCount; ++i)
    {
        if ((i & 31) == 0)
        {
            word = 0;
            if (secondary != 0)
            {
                if (!platform::CopyGuarded(&word, reinterpret_cast<const void*>(secondary + static_cast<std::uintptr_t>(i >> 5) * 4), sizeof(word)))
                {
                    return;
                }
            }
            else if (i < 128)
            {
                word = inlineBits[i >> 5];
            }
        }
        if (((word >> (i & 31)) & 1u) == 0)
        {
            continue;  // a free record
        }
        std::uintptr_t layer = 0;
        if (platform::CopyGuarded(&layer, reinterpret_cast<const void*>(records + static_cast<std::uintptr_t>(i) * facts::kLayerRecordSize + facts::kLayerRecordLayer), sizeof(layer)))
        {
            ForEachLayerElement(layer, visited, stats, fn);
        }
    }
}

/// The fields of a draw element the tile placement reads, in one guarded copy: texture mode and draw flags, the shader
/// vector, the type and the matrix index (facts::kElementTextureMode up to and including kElementMatrixIndex).
struct ElementFields
{
    std::int32_t textureMode = 0;
    std::int32_t drawFlags   = 0;
    std::uint8_t pad0[facts::kElementShaderVector - facts::kElementDrawFlags - 4] = {};
    float        vector[4]   = {};
    std::int32_t type        = 0;
    std::uint8_t pad1[facts::kElementMatrixIndex - facts::kElementType - 4] = {};
    std::int32_t matrixIndex = -1;
};
static_assert(offsetof(ElementFields, drawFlags) == facts::kElementDrawFlags - facts::kElementTextureMode);
static_assert(offsetof(ElementFields, vector) == facts::kElementShaderVector - facts::kElementTextureMode);
static_assert(offsetof(ElementFields, type) == facts::kElementType - facts::kElementTextureMode);
static_assert(offsetof(ElementFields, matrixIndex) == facts::kElementMatrixIndex - facts::kElementTextureMode);
static_assert(sizeof(ElementFields) == facts::kElementMatrixIndex + 4 - facts::kElementTextureMode);
static_assert(facts::kElementTextureMode + sizeof(ElementFields) <= facts::kElementSize);

/// The area map's tiles in a band (research/RE-hud-band-clip.md): each tile quad's shader vector holds the tile's origin in
/// the paint's pixels, which the pixel shader compares with the pixel's position on the image, so in a band the mask is
/// sampled offset by the band's Min and wraps within the tile (a blank strip across the map); the origin is moved by the
/// band's Min. The mask draws' own matrices draw into the mask target, where the band does not exist: they are left out of
/// the band map, whatever the element's type. Fills the skip set for PlaceChildMatrices.
void PlaceMapTiles(const Submit& submit) noexcept
{
    g_matrixSkip.Clear();
    g_matrixOnScreen.Clear();
    const int bits = g_mapTiles.load(std::memory_order_relaxed);
    int   quads = 0, masks = 0, moved = 0;
    float first[4] = {};
    bool  firstRead = false;
    WalkStats stats;
    ForEachElement(submit.entry, stats, [&](std::uintptr_t element) noexcept {
        ElementFields e;
        if (!platform::CopyGuarded(&e, reinterpret_cast<const void*>(element + facts::kElementTextureMode), sizeof(e)))
        {
            return;
        }
        if ((e.drawFlags & facts::kElementDrawToTarget) != 0)
        {
            ++masks;
            if ((bits & kMapTileMatrices) != 0)
            {
                g_matrixSkip.Add(e.matrixIndex);
            }
            return;
        }
        g_matrixOnScreen.Add(e.matrixIndex);
        if (e.type != facts::kElementTypeCustomVerts || e.textureMode != facts::kElementTextureModeMask)
        {
            return;
        }
        ++quads;
        if (!firstRead)
        {
            firstRead = true;
            std::memcpy(first, e.vector, sizeof(first));
        }
        if ((bits & kMapTileOrigins) != 0 && (submit.band.minX != 0 || submit.band.minY != 0))
        {
            const float origin[2] = {e.vector[0] + static_cast<float>(submit.band.minX), e.vector[1] + static_cast<float>(submit.band.minY)};
            if (platform::WriteGuarded(reinterpret_cast<void*>(element + facts::kElementShaderVector), origin, sizeof(origin)))
            {
                ++moved;
            }
        }
    });
    int shared = 0;  // matrices a mask draw and an on-screen element both use: such a one would be left off the band
    for (std::size_t w = 0; w < std::size(g_matrixSkip.words); ++w)
    {
        shared += std::popcount(g_matrixSkip.words[w] & g_matrixOnScreen.words[w]);
    }
    const int         column = std::clamp(overlay_pass::Column(), 0, kMaxColumns - 1);
    const TilesLogged now{quads, masks, shared, bits, stats.layers, stats.bitCount, submit.band, moved};
    if (!(now == g_tilesLogged[column]))
    {
        g_tilesLogged[column] = now;
        platform::LogDebug(L"HUD layout: column {}: {} map tile quad(s), {} moved by the band's ({},{}), {} mask draw(s) {} the band map "
                           L"(hud-band.map-tiles {}), {} matrix index(es) shared with on-screen elements; the first quad's origin "
                           L"({:.1f},{:.1f}), 1/tile {:.6f}; the walk: {} layer(s) ({} further layer bit(s)), {} element(s)",
                           column, quads, moved, submit.band.minX, submit.band.minY, masks,
                           (bits & kMapTileMatrices) != 0 ? L"kept out of" : L"left in", bits, shared, first[0], first[1], first[2],
                           stats.layers, stats.bitCount, stats.elements);
    }
}

/// Moves the per-child view-projections onto the band (the equivalent of a viewport the draw lacks).
void PlaceChildMatrices(const Submit& submit) noexcept
{
    std::int32_t count = 0;
    float*       data  = nullptr;
    if (!platform::CopyGuarded(&count, reinterpret_cast<const void*>(submit.entry + facts::kEntryChildMatrixCount), sizeof(count)) ||
        !platform::CopyGuarded(&data, reinterpret_cast<const void*>(submit.entry + facts::kEntryChildMatrices), sizeof(data)) ||
        count < 0 || count > kMaxChildMatrices || (count > 0 && data == nullptr))
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
        if (SkipsMatrix(i))
        {
            continue;  // a mask draw's matrix: it draws into the mask target, not the image
        }
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

/// Logs the entry's clip states for this band pass: each zone's rect in the paint's absolute pixels (the renderer adds
/// the entry rect's Min to it for the scissor), for comparing a cut's rows with the zones (hud.clip-log).
void LogClipStates(const Submit& submit) noexcept
{
    std::int32_t count = 0;
    std::uintptr_t data = 0;
    if (!platform::CopyGuarded(&count, reinterpret_cast<const void*>(submit.entry + facts::kEntryClipStateCount), sizeof(count)) ||
        !platform::CopyGuarded(&data, reinterpret_cast<const void*>(submit.entry + facts::kEntryClipStates), sizeof(data)) ||
        count < 0 || count > 1024 || (count > 0 && data == 0))
    {
        platform::Log(L"HUD clips: column {}: the entry's clip states do not look like a list (count {})", overlay_pass::Column(), count);
        return;
    }
    std::wstring text;
    for (std::int32_t i = 0; i < count; ++i)
    {
        std::uint8_t state[facts::kClipStateSize] = {};
        if (!platform::CopyGuarded(state, reinterpret_cast<const void*>(data + static_cast<std::uintptr_t>(i) * facts::kClipStateSize),
                                   sizeof(state)))
        {
            text += std::format(L" [{}] unreadable;", i);
            continue;
        }
        float tl[2] = {}, br[2] = {};
        std::memcpy(tl, state + facts::kClipStateTopLeft, sizeof(tl));
        std::memcpy(br, state + facts::kClipStateBottomRight, sizeof(br));
        const std::uint8_t flags = state[facts::kClipStateFlags];
        const std::uint8_t own   = state[facts::kClipStateOwnRect];
        std::int32_t   stackCount = 0;
        std::uintptr_t stack      = 0;
        std::memcpy(&stackCount, state + facts::kClipStateStackCount, sizeof(stackCount));
        std::memcpy(&stack, state + facts::kClipStateStack, sizeof(stack));
        text += std::format(L" [{}] ({:.0f},{:.0f})-({:.0f},{:.0f}) flags {} own {} stack {}", i, tl[0], tl[1], br[0], br[1], flags, own, stackCount);
        if (own == 0 && stackCount > 0 && stackCount <= 64 && stack != 0)
        {
            std::uint8_t zone[facts::kClipZoneSize] = {};
            if (platform::CopyGuarded(zone, reinterpret_cast<const void*>(stack + static_cast<std::uintptr_t>(stackCount - 1) * sizeof(zone)), sizeof(zone)))
            {
                float ztl[2] = {}, zbr[2] = {};
                std::memcpy(ztl, zone + facts::kClipZoneTopLeft, sizeof(ztl));
                std::memcpy(zbr, zone + facts::kClipZoneBottomRight, sizeof(zbr));
                text += std::format(L" last zone ({:.0f},{:.0f})-({:.0f},{:.0f}) flags {}", ztl[0], ztl[1], zbr[0], zbr[1],
                                    zone[facts::kClipZoneFlags]);
            }
        }
        text += L";";
    }
    platform::Log(L"HUD clips: column {}, band ({},{})-({},{}): {} clip state(s):{}", overlay_pass::Column(),
                  g_logged[g_loggedColumn].band.minX, g_logged[g_loggedColumn].band.minY, g_logged[g_loggedColumn].band.maxX,
                  g_logged[g_loggedColumn].band.maxY, count, text);
}

/// Replaces the render's call to FEndMenuRenderer__SubmitLayerSlot (same register arguments; no result).
void SubmitSlot(void* renderer, std::uintptr_t pool, void* slot)
{
    if (g_submit.active && g_gameThread.Here())
    {
        g_submit.active = false;
        PlaceMapTiles(g_submit);  // first: it names the matrices the band map leaves alone
        PlaceChildMatrices(g_submit);
        if (g_clipLogRenders.load(std::memory_order_relaxed) > 0)
        {
            g_clipLogRenders.fetch_sub(1, std::memory_order_relaxed);
            LogClipStates(g_submit);
        }
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
    Register({.name  = L"hud-band.map-tiles",
              .shape = Shape::Integer,
              .min   = 0,
              .max   = 3,
              .help  = L"the area map's tiles in a band, bits: 1 each tile quad's mask origin moved to the image's pixels, 2 the mask "
                       L"draws' matrices kept out of the band map (3 at start; 0 shows the game's strips)",
              .set   = [](const Value& value) {
                  g_mapTiles.store(static_cast<int>(std::clamp<std::int64_t>(std::get<std::int64_t>(value), 0, kMapTileOrigins | kMapTileMatrices)),
                                   std::memory_order_relaxed);
                  return Outcome{};
              },
              .get = [] { return Value{static_cast<std::int64_t>(g_mapTiles.load(std::memory_order_relaxed))}; }});
    Register({.name  = L"hud.clip-log",
              .shape = Shape::Integer,
              .min   = 0,
              .max   = 600,
              .help  = L"log the overlay entry's clip states (each zone's rect in the paint's pixels) for that many band passes",
              .set   = [](const Value& value) {
                  g_clipLogRenders.store(static_cast<int>(std::clamp<std::int64_t>(std::get<std::int64_t>(value), 0, 600)), std::memory_order_relaxed);
                  return Outcome{};
              },
              .get = [] { return Value{static_cast<std::int64_t>(g_clipLogRenders.load(std::memory_order_relaxed))}; }});
    platform::Log(L"HUD layout: installed (hud-band on); the rect call follows on the game thread");
    // The UI effects' conversion in player 1's layout in every pass (hud/ui_effect_pass.h): both of its size reads, or neither.
    const auto sizeOne = image.Rebase(facts::kEffectUiSizeCallOne, 5);
    const auto sizeTwo = image.Rebase(facts::kEffectUiSizeCallTwo, 5);
    const auto sizeFn  = platform::FindFunction(image, facts::kOverlayUiSizeGetter);
    bool       effects = sizeOne && sizeTwo && sizeFn &&
                   g_effectSizeCalls[0].Install(*sizeOne, *sizeFn, reinterpret_cast<std::uintptr_t>(&UiSizeForEffects));
    if (effects && !g_effectSizeCalls[1].Install(*sizeTwo, *sizeFn, reinterpret_cast<std::uintptr_t>(&UiSizeForEffects)))
    {
        g_effectSizeCalls[0].Restore();
        effects = false;
    }
    platform::Log(L"HUD layout: the UI effects' layout conversion {}",
                  effects ? L"reads player 1's column's size in every pass" : L"keeps each pass's own size (its calls are not as the research read)");
    InstallFrameCode(image);
    InstallArrowCode(image);
    InstallBattleArrowCode(image);
    {
        namespace c       = layout_code;
        const auto site   = image.Rebase(c::kScaleSite, sizeof(c::kScaleReplaced));
        const auto resume = image.Rebase(c::kScaleResume);
        if (site && resume && image.BytesMatch(c::kScaleCheckStart, c::kScaleBytes))
        {
            g_scaleSite   = *site;
            g_scaleResume = *resume;
        }
        else
        {
            platform::LogWarning(L"HUD layout: the overlay's layout scale differs from the research; a view narrower than 16:9 keeps the "
                                 L"game's scale, its markers off their objects");
        }
    }
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
    if (g_scaleCode.IsInstalled() && !g_scaleCode.Restore())
    {
        platform::Log(L"HUD layout: the layout scale's code was changed after us; left in place (it acts only in a band narrower than 16:9)");
    }
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
    for (platform::CallRedirect& call : g_effectSizeCalls)
    {
        if (call.IsInstalled() && !call.Restore())
        {
            platform::Log(L"HUD layout: a UI effects' size call was changed after us; left in place (it passes the game's size through)");
        }
    }
    // A call crossing two blocks is restored only from the game thread; elsewhere it stays and passes the engine's
    // rect through (the split is off).
    const bool onGame = g_gameThread.Here();
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
    g_gameThread.Set();
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
        // Two instructions are replaced, so this too needs the only thread that runs them outside the render.
        const bool scaled = ok && g_scaleSite != 0 &&
                            g_scaleCode.Install(g_scaleSite, layout_code::kScaleReplaced, layout_code::kScaleCode, g_scaleResume);
        platform::Log(L"HUD layout: the overlay's layout scale {}",
                      scaled ? L"is the contain rule (a view narrower than 16:9 lays out at its width)" : L"stays the game's");
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
    hasSaved_ = overlay != 0 && g_gameThread.Here() &&
                platform::CopyGuarded(&saved_, reinterpret_cast<const void*>(overlay + facts::kOverlayLayoutMin), sizeof(saved_));
}

HostSetScope::~HostSetScope()
{
    Restore();
}

bool HostSetScope::Lend(int column) noexcept
{
    if (!hasSaved_ || column < 0 || column >= kMaxColumns || !g_hostSet[column].valid ||
        !g_gameThread.Here())
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
