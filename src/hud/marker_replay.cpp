#include "hud/marker_replay.h"

#include "battle/target_cursor.h"
#include "control/registry.h"
#include "engine_facts.h"
#include "hud/bar_class.h"
#include "hud/overlay_pass.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/game_vectors.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/game_globals.h"
#include "players/weak_object.h"
#include "players/scopes.h"
#include "players/seats.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <intrin.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace lss::hud::marker_replay {

namespace {

using platform::GameVector;
using platform::GameVector2D;

/// TWeakPtr (thread-safe): the object and its reference controller (weak count at +0xC).
struct WeakKey
{
    void* object     = nullptr;
    void* controller = nullptr;
};
static_assert(sizeof(WeakKey) == facts::kEnemyBarDisplayedTargetBytes, "the bar manager's key is one weak pair");

using PlaceUntargetedFn = void (*)(void* manager, void* widget, const GameVector* location);
using PlaceTargetedFn   = void (*)(void* manager, void* widget, WeakKey* key, const GameVector* location, std::int32_t scratch,
                                 float halfSize, std::uint8_t skipLeaderClamp);
/// TSharedPtr, NotThreadSafe (the battle text's Slate pointers: the game copies them with a plain `inc [ctrl+8]`).
struct SharedPtr
{
    void* object     = nullptr;
    void* controller = nullptr;
};

using PlaceTextFn        = void (*)(GameVector2D* out, void* controller, const SharedPtr* canvas, const SharedPtr* widget,
                                    const GameVector* location, GameVector2D offset, std::uint8_t clamp, bool* projected);
using SharedReleaseFn    = void (*)(SharedPtr* pointer);  // the game's ~TSharedPtr
using AttributeFn        = void* (*)(void* attribute, const std::uint8_t* visibility);
using SlateVisibilityFn  = void (*)(void* widget, void* attribute);  // takes the TAttribute by value: it destroys it
using WidgetVisibilityFn = void (*)(void* widget, std::uint8_t visibility);
using CursorPlaceFn      = bool (*)(void* controller, void* widget, const GameVector* location);
using LockonPlaceFn      = void (*)(void* controller, void* widget, const GameVector* location, std::uint8_t show);
using ManagerTickFn      = void (*)(void* manager);
using SetPositionFn     = void (*)(void* widget, GameVector2D position, bool removeDpiScale);
using ContextPcFn       = void* (*)(const void* context);
// The enemy bar's class per column (hud/bar_class.h; RE-enemy-bar-two-classification.md, -content.md).
using MainTargetFn      = WeakKey* (*)(WeakKey* out, const WeakKey* key);  // out holds one weak reference
using WeakReleaseFn     = void (*)(WeakKey* pair);
using DtAlphaFn         = float (*)(float scale);
using GetVisibilityFn   = std::uint8_t (*)(void* widget);
using IsTargetableFn    = std::uint8_t (*)(void* unit, std::int32_t one, std::int32_t zero);
using StatusIconsFn     = void (*)(void* block, void* character);
using WidgetFn          = void (*)(void* widget);
using BoolMethodFn      = std::uint8_t (*)(void* self);
using BoolMethod2Fn     = std::uint8_t (*)(void* self, std::int32_t zero);
using ObjectMethodFn    = void* (*)(void* self);
using FloatMethodFn     = float (*)(void* self);

platform::EntryRedirect g_untargeted;
platform::EntryRedirect g_targeted;
platform::EntryRedirect g_text;
platform::EntryRedirect g_cursor;
platform::EntryRedirect g_lockon;
platform::CallRedirect  g_labelsContext;   // crosses a block: installed from the game thread, outside the tick
platform::CallRedirect  g_labelsContext2;
platform::CallRedirect  g_weakContext;
std::uintptr_t          g_cursorAt = 0, g_lockonAt = 0;
std::uintptr_t          g_labelsSite = 0, g_labelsSite2 = 0, g_weakSite = 0, g_contextAt = 0;
ManagerTickFn           g_labelsTick = nullptr, g_weakTick = nullptr;
void**                  g_labelsSlot = nullptr;  // &EndUISystem[45]
void**                  g_weakSlot   = nullptr;  // &EndUISystem[58]
void*                   g_contextOverride = nullptr;  // what the ticks' controller lookups answer during a re-run
std::uintptr_t          g_untargetedAt = 0, g_targetedAt = 0, g_textAt = 0;
SetPositionFn           g_setPosition = nullptr;
ContextPcFn             g_contextPc   = nullptr;  // for the readings only
SharedReleaseFn         g_release     = nullptr;
AttributeFn             g_attribute   = nullptr;
const std::uint8_t*     g_shownVisibility  = nullptr;  // the game's EVisibility statics (set at its start-up)
const std::uint8_t*     g_hiddenVisibility = nullptr;
std::uintptr_t          g_umgReturn = 0, g_numberReturn = 0, g_textReturn = 0, g_wrapperReturn = 0;
bool                    g_installed = false, g_pending = false;
bool                    g_on        = true;
// The bars' class per column: set once at install (every check passed, or the further columns keep the tick's class).
void**                  g_barSlot        = nullptr;  // &EndUISystem[27]
MainTargetFn            g_mainTarget     = nullptr;
WeakReleaseFn           g_weakRelease    = nullptr;
DtAlphaFn               g_dtAlpha        = nullptr;
WidgetVisibilityFn      g_setVisibility  = nullptr;
GetVisibilityFn         g_getVisibility  = nullptr;
IsTargetableFn          g_isTargetable   = nullptr;
StatusIconsFn           g_statusIcons    = nullptr;
WidgetFn                g_renderTransform = nullptr;
bool                    g_ownClasses     = false;

// Game thread only.
bool g_replaying = false;

struct BarCall
{
    void*        manager = nullptr;
    void*        widget  = nullptr;
    std::int32_t weak[2] = {-1, 0};  // the widget's weak pair: it is checked before the replay touches it
    bool         targeted = false;
    GameVector   location;
    WeakKey      key;
    float        halfSize = 0.0f;
    std::uint8_t skip     = 0;
};
/// Which caller placed a text, so its replay sets the visibility as that caller does right after the placement.
enum class TextKind : std::uint8_t
{
    Other,  // the spawn paths with the first controller: position only (the tick places them again)
    Umg,    // the UMG list (and its spawn wrapper): UWidget::SetVisibility on the SObjectWidget's UUserWidget
    Slate,  // damage numbers and texts: SWidget::SetVisibility
};

struct TextCall
{
    void*        controller = nullptr;
    SharedPtr    canvas;  // pinned copies: the tick passes temporaries on its stack
    SharedPtr    widget;
    GameVector   location;
    GameVector2D offset;
    std::uint8_t clamp = 0;
    TextKind     kind  = TextKind::Other;
};
/// A target cursor or lock-on marker placement (both pure, the controller an argument).
struct PointCall
{
    void*        controller = nullptr;
    void*        widget     = nullptr;
    std::int32_t weak[2]    = {-1, 0};
    GameVector   location;
    std::uint8_t show = 0;  // lock-on markers only
};

std::vector<BarCall>   g_bars;
std::vector<TextCall>  g_texts;
std::vector<PointCall> g_cursors;
std::vector<PointCall> g_lockons;
constexpr int         kMaxColumns = overlay_pass::kMaxColumns;
std::uint32_t         g_replayed[kMaxColumns] = {};  // calls replayed per column since the last log line
std::uint32_t         g_frames      = 0;

/// The replays made for the further columns together.
std::uint32_t FurtherReplayed() noexcept
{
    std::uint32_t count = 0;
    for (int column = 1; column < kMaxColumns; ++column)
    {
        count += g_replayed[column];
    }
    return count;
}

/// What the readings show (`hud.markers`): counts since the last reading, and one bar and one text followed through
/// the frame (the tick's placement, then each column's replay: index = column, the further columns' first).
struct Seen
{
    std::uint32_t frames = 0, framesWithMarkers = 0;
    std::uint32_t untargeted = 0, targeted = 0, texts = 0;  // recorded
    std::uint32_t replays[kMaxColumns] = {}, faults = 0;
    std::uint32_t textKinds[3] = {};  // recorded per TextKind
    std::uint32_t visibilities = 0;   // visibility calls made by the replays
    std::uint32_t umgUnmatched = 0;   // UMG texts whose SObjectWidget did not lead back to its UUserWidget
    std::uint32_t cursors = 0, lockons = 0;            // recorded
    std::uint32_t labelRuns = 0, weakRuns = 0;         // ticks run again (both passes)
    std::uint32_t overridden = 0;                      // controller lookups answered by a re-run
    // A sampled cursor and lock-on marker: the tick's position ([0]), then each column's ([1 + column]).
    void*         cursor = nullptr, *lockon = nullptr;
    float         cursorAt[1 + kMaxColumns][2] = {}, lockonAt[1 + kMaxColumns][2] = {};
    // The sampled bar (the first recorded in the last frame with bars): the tick's position, then each column's.
    void*        bar          = nullptr;
    bool         barTargeted  = false;
    float        barTick[2]   = {}, barAt[kMaxColumns][2] = {};
    std::int32_t contextBefore[2] = {}, contextFurther[2] = {};  // the widget's player pair, and a further column's
    void*        pcUnderSwap  = nullptr;  // what the widget's context resolves to with the further column's pair in it
    void*        pcFurther    = nullptr;  // that column's controller
    // The sampled text: the position each column's replay computed.
    void*        text         = nullptr;
    float        textAt[kMaxColumns][2] = {};
    bool         textProjected[kMaxColumns] = {};
    // The bars' class per further column: bars classed otherwise than the tick (per column), looks set and put back,
    // status-icon refreshes made, and column 0's checks of the rule against the tick.
    std::uint32_t reclassed[kMaxColumns] = {}, looksSet = 0, looksRestored = 0, iconsRefreshed = 0, disagreements = 0;
    std::uint32_t keptBeforePlace = 0;   // bars a further column placed targeted, the tick's visibility kept first
    std::uint32_t putBackAfterPlace = 0;  // targeted placements of the tick's class whose visibility was put back at once
    // The sampled bar's class in the further column that last replayed it, and its own visibility before and after.
    bool         sampleTickTargeted = false, sampleOwnTargeted = false;
    std::uint8_t sampleBarBefore = 0xFF, sampleBarAfter = 0xFF;
    float        sampleEase = 0.0f;
};
Seen          g_seen;

/// A further column's own copy of a bar's y-ease (the tick keeps one, player 1's, in the entry: p+0x110).
struct OwnEase
{
    int   column = 0;
    void* source = nullptr;  // the entry's source object: the ease belongs to the enemy's bar
    float ease   = 0.0f;
    bool  seen   = false;
};
std::vector<OwnEase> g_eases;

/// A widget's visibility as the tick left it, changed in a further column's pass and put back before player 1's.
struct LookRestore
{
    void*        widget = nullptr;
    std::int32_t weak[2] = {-1, 0};
    std::uint8_t visibility = 0;
};
std::vector<LookRestore> g_looks;
std::vector<void*>       g_refreshed;  // the entries whose status icons were refreshed this frame (once a frame each)
std::uint32_t            g_reclassedLogged[kMaxColumns] = {};  // the last count logged per column
bool                     g_reclassedTold = false;              // the first re-class ever: an Info line
std::int32_t             g_disagreedLogged = -1;
std::uint32_t g_run = 0;  // consecutive frames with markers (an Info line when a run starts and ends)
std::uint32_t g_runRecorded = 0;

/// A replay call that faulted: counted, and the first of each kind logged.
void Fault(int kind, const wchar_t* what) noexcept
{
    static std::uint32_t told = 0;
    ++g_seen.faults;
    if ((told & (1u << kind)) == 0)
    {
        told |= 1u << kind;
        platform::Log(L"HUD markers: replaying {} faulted (the first; `hud.markers` counts them); that marker stays as "
                      L"the game placed it", what);
    }
}

/// A widget recorded this frame, if it is still that live object (a widget dropped by its tick may be collected
/// before the overlay render).
bool Alive(void* widget, const std::int32_t (&weak)[2]) noexcept
{
    return widget != nullptr && players::weak_object::ObjectOf(weak) == widget;
}

/// Last call per widget wins (the tick may place a bar twice: creation and frame). Runs inside the game's call: a record
/// that cannot be kept (no memory) leaves that widget at player 1's placement in the further views.
template <class Call>
void Keep(std::vector<Call>& calls, const Call& call) noexcept
{
    for (auto& known : calls)
    {
        if (known.widget == call.widget)
        {
            known = call;
            return;
        }
    }
    try
    {
        calls.push_back(call);
    }
    catch (...)
    {
    }
}

void Untargeted(void* manager, void* widget, const GameVector* location)
{
    if (!g_replaying && g_on && location != nullptr)
    {
        BarCall call{manager, widget};
        call.location = *location;
        players::weak_object::WeakPairOf(widget, call.weak);
        Keep(g_bars, call);
        ++g_seen.untargeted;
    }
    reinterpret_cast<PlaceUntargetedFn>(g_untargeted.Original())(manager, widget, location);
}

void Targeted(void* manager, void* widget, WeakKey* key, const GameVector* location, std::int32_t scratch, float halfSize,
              std::uint8_t skip)
{
    if (!g_replaying && g_on && location != nullptr && key != nullptr)
    {
        BarCall call{manager, widget};
        call.targeted = true;
        call.location = *location;
        call.key      = *key;
        call.halfSize = halfSize;
        call.skip     = skip;
        players::weak_object::WeakPairOf(widget, call.weak);
        Keep(g_bars, call);
        ++g_seen.targeted;
    }
    reinterpret_cast<PlaceTargetedFn>(g_targeted.Original())(manager, widget, key, location, scratch, halfSize, skip);
}

/// A copy holding one more shared reference, as the game's own copies (game thread only, as theirs).
SharedPtr Pin(const SharedPtr& from) noexcept
{
    if (from.controller != nullptr)
    {
        ++*reinterpret_cast<std::int32_t*>(static_cast<std::uint8_t*>(from.controller) + 8);
    }
    return from;
}

void Unpin(SharedPtr& pointer) noexcept
{
    if (pointer.controller != nullptr)
    {
        platform::CallGuarded(g_release, &pointer);
    }
    pointer = {};
}

void ClearTexts() noexcept
{
    for (auto& text : g_texts)
    {
        Unpin(text.canvas);
        Unpin(text.widget);
    }
    g_texts.clear();
}

TextKind KindOf(std::uintptr_t returnAddress) noexcept
{
    if (returnAddress == g_numberReturn || returnAddress == g_textReturn)
    {
        return TextKind::Slate;
    }
    if (returnAddress == g_umgReturn || returnAddress == g_wrapperReturn)
    {
        return TextKind::Umg;
    }
    return TextKind::Other;
}

void Text(GameVector2D* out, void* controller, const SharedPtr* canvas, const SharedPtr* widget, const GameVector* location,
          GameVector2D offset, std::uint8_t clamp, bool* projected)
{
    if (!g_replaying && g_on && location != nullptr && canvas != nullptr && widget != nullptr && canvas->object != nullptr &&
        widget->object != nullptr)
    {
        const TextKind kind = KindOf(reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
        TextCall       call{controller, Pin(*canvas), Pin(*widget), *location, offset, clamp, kind};
        bool           kept = false;
        for (auto& known : g_texts)  // last call per widget wins
        {
            if (known.widget.object == call.widget.object)
            {
                Unpin(known.canvas);
                Unpin(known.widget);
                known = call;
                kept  = true;
                break;
            }
        }
        if (!kept)
        {
            try
            {
                g_texts.push_back(call);
            }
            catch (...)  // no memory: the text stays at player 1's placement in the further views
            {
                Unpin(call.canvas);
                Unpin(call.widget);
            }
        }
        ++g_seen.texts;
        ++g_seen.textKinds[static_cast<int>(kind)];
    }
    reinterpret_cast<PlaceTextFn>(g_text.Original())(out, controller, canvas, widget, location, offset, clamp, projected);
}

/// Last call per widget wins (inside the game's call: as Keep).
void KeepPoint(std::vector<PointCall>& calls, const PointCall& call) noexcept
{
    for (auto& known : calls)
    {
        if (known.widget == call.widget)
        {
            known = call;
            return;
        }
    }
    try
    {
        calls.push_back(call);
    }
    catch (...)
    {
    }
}

bool Cursor(void* controller, void* widget, const GameVector* location)
{
    if (!g_replaying && g_on && location != nullptr && widget != nullptr)
    {
        PointCall call{controller, widget};
        call.location = *location;
        players::weak_object::WeakPairOf(widget, call.weak);
        KeepPoint(g_cursors, call);
        ++g_seen.cursors;
    }
    return reinterpret_cast<CursorPlaceFn>(g_cursor.Original())(controller, widget, location);
}

void Lockon(void* controller, void* widget, const GameVector* location, std::uint8_t show)
{
    if (!g_replaying && g_on && location != nullptr && widget != nullptr)
    {
        PointCall call{controller, widget};
        call.location = *location;
        call.show     = show;
        players::weak_object::WeakPairOf(widget, call.weak);
        KeepPoint(g_lockons, call);
        ++g_seen.lockons;
    }
    reinterpret_cast<LockonPlaceFn>(g_lockon.Original())(controller, widget, location, show);
}

/// The labels' and weak points' controller lookups (FLocalPlayerContext::GetPlayerController on the widget's context).
void* ContextController(const void* context)
{
    if (g_contextOverride != nullptr)
    {
        ++g_seen.overridden;
        return g_contextOverride;
    }
    return reinterpret_cast<ContextPcFn>(g_contextAt)(context);
}

/// The bar entry (p = element + 0x10) whose weak widget is `widget`, from the manager's set (RE-hud-battle-markers.md:
/// data +0x08, NumBits +0x30, 0x150 per element, the weak widget at p+0).
std::uint8_t* BarEntryOf(void* manager, void* widget) noexcept
{
    std::uint8_t* data  = nullptr;
    std::int32_t  count = 0, index = -1;
    const auto*   m     = static_cast<const std::uint8_t*>(manager);
    if (!platform::CopyGuarded(&data, m + 0x08, sizeof(data)) || !platform::CopyGuarded(&count, m + 0x30, sizeof(count)) ||
        !platform::CopyGuarded(&index, static_cast<const std::uint8_t*>(widget) + facts::kObjectIndex, sizeof(index)) ||
        data == nullptr || count <= 0 || count > 4096)
    {
        return nullptr;
    }
    for (std::int32_t i = 0; i < count; ++i)
    {
        std::uint8_t* p     = data + static_cast<std::size_t>(i) * 0x150 + 0x10;
        std::int32_t  weak  = -1;
        if (platform::CopyGuarded(&weak, p, sizeof(weak)) && weak == index)
        {
            return p;
        }
    }
    return nullptr;
}

/// One reference more on the key's weak count: the targeted placement releases one (the tick does the same,
/// `lock inc [ctrl+0xC]`).
void AddWeakReference(void* controller) noexcept
{
    if (controller != nullptr)
    {
        _InterlockedIncrement(reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(controller) + 0xC));
    }
}

/// A virtual method of `self` at vtable offset `slot`, read guarded.
template <class Fn>
bool Method(const void* self, std::uint32_t slot, Fn& fn) noexcept
{
    void* vtable = nullptr;
    fn           = nullptr;
    return platform::Read(self, vtable) && vtable != nullptr &&
           platform::Read(static_cast<const std::uint8_t*>(vtable) + slot, fn) && fn != nullptr;
}

/// A column's displayed target and its main target, as the tick's rule reads them: the main target is the key's owner
/// when the key is a part (the game's own lookup; its result holds one weak reference, given back).
bar_class::Keys KeysOf(const WeakKey& key) noexcept
{
    bar_class::Keys keys{key.object, nullptr};
    if (key.object == nullptr || key.controller == nullptr || g_mainTarget == nullptr)
    {
        return keys;
    }
    WeakKey  main{};
    WeakKey* result = nullptr;
    if (platform::CallGuarded(g_mainTarget, result, &main, static_cast<const WeakKey*>(&key)))
    {
        keys.main = main.object;
        if (main.controller != nullptr)
        {
            platform::CallGuarded(g_weakRelease, &main);
        }
    }
    return keys;
}

/// The key the tick classified with: the live displayed target (player 1's) on the bar manager.
WeakKey LiveKey(const void* manager) noexcept
{
    WeakKey key{};
    platform::CopyGuarded(&key, static_cast<const std::uint8_t*>(manager) + facts::kEnemyBarDisplayedTarget, sizeof(key));
    return key;
}

/// A column's own key: column 0's and a mirror's the live one (player 1's look); a seat's column its seat's copy (the
/// seat of the column from overlay_pass's map: with player 2 out, player 3 is in column 1). No copy, or one open: an
/// empty key, which classes every bar as untargeted there (the single-player look of a player with no target).
WeakKey ColumnKey(int column, const void* manager) noexcept
{
    const int seat = overlay_pass::SeatOf(column);
    if (seat < 1)
    {
        return LiveKey(manager);
    }
    WeakKey key{};
    battle::target_cursor::DisplayedTarget(seat, key.object, key.controller);
    return key;
}

/// What a bar's entry leads to: its source, the source's character and battle unit, and the "shown anyway" flag.
struct BarObjects
{
    void*        source     = nullptr;
    void*        controller = nullptr;
    void*        character  = nullptr;
    void*        unit       = nullptr;
    std::uint8_t show       = 0;  // p+0x12D: the bar's content shows whoever targets it (a burst began)
};

bool ObjectsOf(const std::uint8_t* p, BarObjects& out) noexcept
{
    const std::uint8_t* entry = p - facts::kBarEntryP;
    if (!platform::CopyGuarded(&out.source, entry, sizeof(out.source)) ||
        !platform::CopyGuarded(&out.controller, entry + 8, sizeof(out.controller)) || out.source == nullptr)
    {
        return false;
    }
    platform::CopyGuarded(&out.show, p + facts::kBarEntryShowFlag, sizeof(out.show));
    ObjectMethodFn character = nullptr;
    if (Method(out.source, facts::kSourceCharacterSlot, character))
    {
        platform::CallGuarded(character, out.character, out.source);
    }
    out.unit = platform::ReadPointer(out.character, facts::kCharacterUnit);
    return true;
}

/// The targeted clamp's half size for a bar the tick placed untargeted: the character's own answer, as the tick asks it.
float HalfSizeOf(const BarObjects& objects) noexcept
{
    float half = 0.0f;
    if (objects.character != nullptr)
    {
        auto*         object = static_cast<std::uint8_t*>(objects.character) + facts::kCharacterHalfSizeObject;
        FloatMethodFn fn     = nullptr;
        if (Method(object, facts::kHalfSizeSlot, fn))
        {
            platform::CallGuarded(fn, half, static_cast<void*>(object));
        }
    }
    return half;
}

/// The visibility the tick gives a bar and its parts for `targeted` (RE-enemy-bar-two-content.md): 0 Visible, 1
/// Collapsed. The stagger-bonus icon's applies only while the unit shows a bonus (the game sets it only then).
struct Look
{
    std::uint8_t bar = 1, name = 1, status = 1, linkage = 1;
    bool         bonus = false;  // the unit's stagger bonus is showing: the bonus icon's visibility applies
};

Look LookFor(const BarObjects& objects, bool targeted) noexcept
{
    Look          look;
    std::uint8_t  part = 0, downed = 0, bonus = 0, targetable = 1, keep = 0;
    BoolMethod2Fn partFn = nullptr;
    BoolMethodFn  fn     = nullptr;
    if (Method(objects.source, facts::kSourceIsPartSlot, partFn))
    {
        platform::CallGuarded(partFn, part, objects.source, std::int32_t{0});
    }
    if (objects.character != nullptr && Method(objects.character, facts::kCharacterIsDownedSlot, fn))
    {
        platform::CallGuarded(fn, downed, objects.character);
    }
    if (objects.unit != nullptr && Method(objects.unit, facts::kUnitBonusActiveSlot, fn))
    {
        platform::CallGuarded(fn, bonus, objects.unit);
    }
    look.bonus = bonus != 0;
    if (!targeted)
    {
        return look;  // every part collapsed, the bar too
    }
    if (objects.unit == nullptr)
    {
        look.bar = 1;
    }
    else
    {
        platform::CallGuarded(g_isTargetable, targetable, objects.unit, std::int32_t{1}, std::int32_t{0});
        platform::CopyGuarded(&keep, static_cast<const std::uint8_t*>(objects.unit) + facts::kUnitKeepBarFlag, sizeof(keep));
        look.bar = targetable == 0 && keep == 0 ? 1 : 0;  // untargetable and no script keeps its bar: collapsed
    }
    look.name    = 0;
    look.status  = part != 0 ? 1 : 0;
    look.linkage = part != 0 || downed != 0 ? 1 : 0;
    return look;
}

/// A widget's visibility as it stands, kept for the restore. False when it could not be read or kept.
bool KeepLook(void* widget, const std::int32_t (&weak)[2], std::uint8_t& now) noexcept
{
    now = 0xFF;
    if (widget == nullptr || !platform::CallGuarded(g_getVisibility, now, widget))
    {
        return false;
    }
    try
    {
        g_looks.push_back({widget, {weak[0], weak[1]}, now});
    }
    catch (...)
    {
        return false;
    }
    return true;
}

/// One widget's visibility set for this pass with the game's setter, the value before it kept for the restore.
void SetLook(void* widget, const std::int32_t (&weak)[2], std::uint8_t value) noexcept
{
    std::uint8_t now = 0xFF;
    if (!KeepLook(widget, weak, now))
    {
        return;  // the restore first: a look is never set without one
    }
    if (now != value && platform::CallGuarded(g_setVisibility, widget, value))
    {
        ++g_seen.looksSet;
    }
}

/// A sub-widget of the entry at its weak field.
void* SubWidget(const std::uint8_t* p, std::uint32_t field, std::int32_t (&weak)[2]) noexcept
{
    weak[0] = -1;
    weak[1] = 0;
    if (!platform::CopyGuarded(weak, p + field, sizeof(weak)))
    {
        return nullptr;
    }
    return players::weak_object::ObjectOf(weak);
}

/// The bar's look for this column when its class differs from the tick's: the bar, the name, the status panel and
/// the stagger-bonus icon through UWidget::SetVisibility (the tick's own setter), the tick's values kept.
Look ApplyLook(const std::uint8_t* p, void* barWidget, const std::int32_t (&barWeak)[2], const BarObjects& objects,
               bool targeted) noexcept
{
    const Look   look = LookFor(objects, targeted);
    std::int32_t weak[2]{};
    SetLook(barWidget, barWeak, look.bar);
    SetLook(SubWidget(p, facts::kBarEntryNameText, weak), weak, look.name);
    SetLook(SubWidget(p, facts::kBarEntryStatusPanel, weak), weak, look.status);
    if (look.bonus)  // the game sets the bonus icon's visibility only while the bonus shows
    {
        SetLook(SubWidget(p, facts::kBarEntryLinkage, weak), weak, look.linkage);
    }
    return look;
}

/// The status icons of a bar only this column targets, refreshed by the game's own once-per-frame call (the tick made
/// it only for the bars it placed targeted): the enemy's real status, no per-view state, nothing to put back.
void RefreshStatusIcons(const std::uint8_t* p, const BarObjects& objects, bool bonus) noexcept
{
    void* block = platform::ReadPointer(p, facts::kBarEntryStatusIcons);
    if (block == nullptr || objects.character == nullptr || g_statusIcons == nullptr)
    {
        return;
    }
    std::uint8_t* slots = nullptr;
    std::int32_t  count = 0;
    if (platform::CopyGuarded(&slots, block, sizeof(slots)) && platform::CopyGuarded(&count, static_cast<std::uint8_t*>(block) + 8, sizeof(count)) &&
        slots != nullptr && count > 0 && count <= 16)
    {
        const float y = bonus ? 30.0f : 0.0f;
        for (std::int32_t i = 0; i < count; ++i)
        {
            std::int32_t weak[2]{};
            void*        icon = SubWidget(slots + static_cast<std::size_t>(i) * facts::kBarStatusSlotSize, 0, weak);
            if (icon != nullptr && platform::WriteGuarded(static_cast<std::uint8_t*>(icon) + facts::kWidgetRenderTranslationY, &y, sizeof(y)))
            {
                platform::CallGuarded(g_renderTransform, icon);
            }
        }
    }
    if (platform::CallGuarded(g_statusIcons, block, objects.character))
    {
        ++g_seen.iconsRefreshed;
    }
    else
    {
        Fault(10, L"a bar's status icons");
    }
}

/// A further column's own y-ease for its displayed target's bar: the tick's arithmetic on the column's own copy
/// (the game has no function for the step: it is inline in the tick, RE-enemy-bar-two-classification.md; the
/// recreation is three lines with the game's constants and its own dt helper).
float& OwnEaseOf(int column, void* source) noexcept
{
    for (auto& own : g_eases)
    {
        if (own.column == column && own.source == source)
        {
            own.seen = true;
            return own.ease;
        }
    }
    try
    {
        g_eases.push_back({column, source, 0.0f, true});  // a new entry starts at 0, as the game's
        return g_eases.back().ease;
    }
    catch (...)
    {
        static float scratch = 0.0f;  // no room for a copy: this frame's step is lost, nothing else
        scratch = 0.0f;
        return scratch;
    }
}

void StepOwnEase(float& ease) noexcept
{
    float alpha = 0.0f;
    if (g_dtAlpha != nullptr && platform::CallGuarded(g_dtAlpha, alpha, 1.0f))
    {
        ease += (-60.0f - ease) * 3.0f * alpha;
    }
}

/// The looks the further columns' passes set, put back to the tick's values, the last set first (the columns run
/// highest first: each pass starts from the tick's looks, and player 1's pass renders them).
void RestoreLooks() noexcept
{
    for (auto look = g_looks.rbegin(); look != g_looks.rend(); ++look)
    {
        if (Alive(look->widget, look->weak) && platform::CallGuarded(g_setVisibility, look->widget, look->visibility))
        {
            ++g_seen.looksRestored;
        }
    }
    g_looks.clear();
}

/// The keys a column classes with (bar_class.h): player 1's live key (the tick's) and the column's own; a further
/// column's own key from its seat, column 0's the live one again.
struct ColumnKeys
{
    bar_class::Keys one, two;
};

ColumnKeys KeysForColumn(int column, bool ownColumn, const void* manager) noexcept
{
    const WeakKey live = LiveKey(manager);
    ColumnKeys    keys;
    keys.one = g_ownClasses ? KeysOf(live) : bar_class::Keys{live.object, nullptr};  // the ease needs the live key either way
    keys.two = ownColumn ? KeysOf(ColumnKey(column, manager)) : keys.one;
    return keys;
}

/// One bar placed for this column as the game places a bar of that class, then the displayed target's rise applied:
/// column 0 the tick's (applied, not stepped), a further column its own copy stepped once per frame as the tick
/// steps the game's.
void PlaceBar(int column, bool ownColumn, const BarCall& bar, std::uint8_t* p, const BarObjects& objects, bool known,
              bool targeted, bool caseC, const bar_class::Keys& two, bool sample) noexcept
{
    if (!targeted)
    {
        if (!platform::CallGuarded(reinterpret_cast<PlaceUntargetedFn>(g_untargeted.Original()), bar.manager, bar.widget,
                                   static_cast<const GameVector*>(&bar.location)))
        {
            Fault(1, L"an enemy bar");
        }
        return;
    }
    // The key is the entry's own pair; the clamp's half size the tick's when it placed this bar targeted, else the
    // character's answer; the leader clamp against this column's seat (its member), unless case (c) skips it.
    WeakKey            key      = bar.targeted ? bar.key : WeakKey{objects.source, objects.controller};
    const float        halfSize = bar.targeted ? bar.halfSize : HalfSizeOf(objects);
    const std::uint8_t skip     = ownColumn ? (caseC ? 1 : 0) : bar.skip;
    AddWeakReference(key.controller);  // the placement releases one
    bool      placed = false;
    const int seat   = overlay_pass::SeatOf(column);  // a mirror's column (no seat) clamps against player 1's leader, as the tick
    {
        std::optional<players::scopes::Scope> leader;  // the seat's own leader for the clamp, held through the call only
        if (ownColumn && skip == 0 && seat >= 1)
        {
            leader.emplace(seat, players::scopes::Leader);
            if (!leader->Held())
            {
                static bool told[players::scopes::kMaxPlayers]{};  // once per seat (seat >= 1 here)
                if (!told[seat])
                {
                    told[seat] = true;
                    platform::Log(L"HUD markers: {}'s leader scope is not bound; its target's bar is clamped away from player 1's leader",
                                  players::SeatName(seat));
                }
            }
        }
        placed = platform::CallGuarded(reinterpret_cast<PlaceTargetedFn>(g_targeted.Original()), bar.manager, bar.widget, &key,
                                       &bar.location, std::int32_t{0}, halfSize, skip);
    }
    if (!placed)
    {
        Fault(2, L"the targeted enemy bar");
        if (g_weakRelease != nullptr && key.controller != nullptr)
        {
            platform::CallGuarded(g_weakRelease, &key);  // the reference the placement did not get to release
        }
    }
    if (p == nullptr || !known || objects.source != two.own)
    {
        return;
    }
    float point[2]{}, ease = 0.0f;
    bool  have = platform::CopyGuarded(point, p + facts::kBarEntryPoint, sizeof(point));
    if (ownColumn)
    {
        float& mine = OwnEaseOf(column, objects.source);
        StepOwnEase(mine);
        ease = mine;
        if (sample)
        {
            g_seen.sampleEase = mine;
        }
    }
    else
    {
        have = have && platform::CopyGuarded(&ease, p + facts::kBarEntryEase, sizeof(ease));
    }
    if (have)
    {
        platform::CallGuarded(g_setPosition, bar.widget, GameVector2D{point[0], point[1] + ease}, false);
    }
}

/// The content's look follows `targeted || shown anyway`: set for this column where it differs from the tick's, and
/// the status icons refreshed for a bar only this column targets, once a frame (the tick's cadence) however many
/// further columns target it. True when this bar's class differed from the tick's.
bool LookAfter(const BarCall& bar, std::uint8_t* p, const BarObjects& objects, bool targeted, bool sample) noexcept
{
    const bool tickLook = bar.targeted || objects.show != 0;
    const bool ownLook  = targeted || objects.show != 0;
    if (tickLook == ownLook)
    {
        return targeted != bar.targeted;  // the placement differed, the look did not (shown anyway)
    }
    if (sample)
    {
        platform::CallGuarded(g_getVisibility, g_seen.sampleBarBefore, bar.widget);
    }
    const Look look = ApplyLook(p, bar.widget, bar.weak, objects, ownLook);
    if (sample)
    {
        platform::CallGuarded(g_getVisibility, g_seen.sampleBarAfter, bar.widget);
    }
    if (ownLook && !tickLook && std::find(g_refreshed.begin(), g_refreshed.end(), static_cast<void*>(p)) == g_refreshed.end())
    {
        try
        {
            g_refreshed.push_back(p);
        }
        catch (...)
        {
            return true;  // no room to remember it: the refresh is skipped rather than risked twice
        }
        RefreshStatusIcons(p, objects, look.bonus);
    }
    return true;
}

void ReplayBars(int column, const std::int32_t (&context)[2], void* controller) noexcept
{
    if (g_bars.empty())
    {
        return;
    }
    // The bar manager: the game's slot (the keys, the label scope); every recorded bar's manager must be it.
    void* manager = nullptr;
    if (g_barSlot == nullptr || !platform::CopyGuarded(&manager, g_barSlot, sizeof(manager)))
    {
        manager = nullptr;
    }
    const bool        ownColumn = g_ownClasses && column >= 1 && manager != nullptr;  // this column classes with its own key
    const ColumnKeys  keys      = KeysForColumn(column, ownColumn, manager != nullptr ? manager : g_bars.front().manager);
    std::uint32_t     reclassed = 0, disagreed = 0;
    for (auto& bar : g_bars)
    {
        if (!Alive(bar.widget, bar.weak))
        {
            continue;
        }
        std::uint8_t* const p = BarEntryOf(bar.manager, bar.widget);
        // A targeted placement consumes one weak reference on the key: replayed only while the manager's entry for this
        // widget still holds that key (its own reference keeps the controller alive).
        if (bar.targeted)
        {
            void* held = nullptr;
            if (p == nullptr || !platform::CopyGuarded(&held, p - facts::kBarEntryP + 8, sizeof(held)) || held != bar.key.controller)
            {
                continue;
            }
        }
        // This column's class: the tick's for column 0 (its inputs reproduce the tick), its own key's for a further one.
        BarObjects objects;
        const bool known    = p != nullptr && bar.manager == manager && ObjectsOf(p, objects);
        auto       verdict  = bar_class::Verdict{};
        bool       targeted = bar.targeted;
        if (g_ownClasses && known)
        {
            verdict = bar_class::Classify(objects.source, keys.one, keys.two, bar.targeted, bar.skip != 0);
            if (ownColumn)
            {
                // A targeted placement finds the entry by its key: an entry whose controller is gone would match
                // every empty key, so it keeps the untargeted placement.
                targeted = verdict.targeted && (bar.targeted || objects.controller != nullptr);
            }
            else if (!bar_class::PlayerOneAgrees(objects.source, keys.one, bar.targeted, bar.skip != 0))
            {
                ++disagreed;
            }
        }
        auto*        widget = static_cast<std::uint8_t*>(bar.widget);
        std::int32_t saved[2]{};
        const bool   sample = &bar == &g_bars.front();
        if (sample && column != 0)
        {
            g_seen.bar         = bar.widget;
            g_seen.barTargeted = bar.targeted;
            g_seen.pcFurther   = controller;
            platform::CopyGuarded(g_seen.barTick, widget + facts::kUserWidgetViewportOffsets, sizeof(g_seen.barTick));
            platform::CopyGuarded(g_seen.contextBefore, widget + facts::kUserWidgetPlayerWeak, sizeof(g_seen.contextBefore));
            std::memcpy(g_seen.contextFurther, context, sizeof(context));
            g_seen.sampleTickTargeted = bar.targeted;
            g_seen.sampleOwnTargeted  = targeted;
        }
        if (!platform::CopyGuarded(saved, widget + facts::kUserWidgetPlayerWeak, sizeof(saved)) ||
            !platform::WriteGuarded(widget + facts::kUserWidgetPlayerWeak, context, sizeof(context)))
        {
            Fault(0, L"an enemy bar's context");
            continue;
        }
        if (sample && column != 0 && g_contextPc != nullptr)
        {
            g_seen.pcUnderSwap = nullptr;
            platform::CallGuarded(g_contextPc, g_seen.pcUnderSwap, static_cast<const void*>(widget + facts::kUserWidgetPlayerWeak));
        }
        // The game's targeted placement makes the bar visible itself (SetVisibility 3, RE-enemy-bar-three-leaks.md (a)); the
        // tick's loop then gives it its final look. Every pass starts from the tick's looks (RestoreLooks), so the value found
        // here is the tick's: kept for the restore when this column's class differs (its own look follows in LookAfter;
        // RestoreLooks puts the earliest kept value back last), put back right after the placement when the class is the
        // tick's (column 0 always), so no pass draws a bar the tick hid. Not readable: the tick's class, never targeted
        // without a restore.
        std::uint8_t found   = 0xFF;
        bool         putBack = false;
        if (targeted && targeted != bar.targeted)
        {
            if (KeepLook(bar.widget, bar.weak, found))
            {
                ++g_seen.keptBeforePlace;
            }
            else
            {
                targeted = bar.targeted;
            }
        }
        else if (targeted)
        {
            putBack = platform::CallGuarded(g_getVisibility, found, bar.widget);
        }
        PlaceBar(column, ownColumn, bar, p, objects, known, targeted, verdict.caseC, keys.two, sample);
        if (putBack)
        {
            std::uint8_t now = 0xFF;
            if (platform::CallGuarded(g_getVisibility, now, bar.widget) && now != found &&
                platform::CallGuarded(g_setVisibility, bar.widget, found))
            {
                ++g_seen.putBackAfterPlace;
            }
        }
        platform::WriteGuarded(widget + facts::kUserWidgetPlayerWeak, saved, sizeof(saved));
        if (ownColumn && known && LookAfter(bar, p, objects, targeted, sample))
        {
            ++reclassed;
        }
        if (sample)
        {
            platform::CopyGuarded(g_seen.barAt[column], widget + facts::kUserWidgetViewportOffsets, sizeof(g_seen.barAt[column]));
        }
    }
    const bar_class::Keys& two = keys.two;
    if (ownColumn)
    {
        g_seen.reclassed[column] += reclassed;
        if (reclassed != g_reclassedLogged[column])
        {
            g_reclassedLogged[column] = reclassed;
            const wchar_t* who = players::SeatName(overlay_pass::SeatOf(column));  // "a player" for a mirror's column
            if (!g_reclassedTold && reclassed != 0)
            {
                g_reclassedTold = true;
                platform::Log(L"HUD markers: {}'s view (column {}) shows its own target's enemy bar for the first time ({} bar(s) "
                              L"classed by that player's target this frame, their look set for its pass and put back for player 1's)",
                              who, column + 1, reclassed);
            }
            else
            {
                platform::LogDebug(L"HUD markers: {}'s view (column {}): {} enemy bar(s) classed otherwise than the tick (own target {}, its main target {})",
                                   who, column + 1, reclassed, two.own, two.main);
            }
        }
    }
    else if (column == 0 && g_ownClasses)
    {
        g_seen.disagreements += disagreed;
        if (static_cast<std::int32_t>(disagreed) != g_disagreedLogged)
        {
            g_disagreedLogged = static_cast<std::int32_t>(disagreed);
            platform::LogDebug(L"HUD markers: player 1's view: the bar rule disagrees with the tick on {} bar(s)", disagreed);
        }
    }
}

/// The displayed-target key on the bar manager answered for a seat's column while its label tick runs again (the
/// ability label sits over that key's bar, RE-enemy-bar-two-classification.md; the tick only reads it). A seat with
/// no target gets the empty key: the label's bar lookup then finds nothing and the label stays at its own point, the
/// single-player look of a player with no target (RE-enemy-bar-three-leaks.md); left alone, the live key would put
/// it over player 1's target. A mirror's column keeps the live key (player 1's look).
class ColumnKeyScope
{
public:
    explicit ColumnKeyScope(int column) noexcept
    {
        void* manager = nullptr;
        if (column < 1 || column >= kMaxColumns || !g_ownClasses || g_barSlot == nullptr ||
            !platform::CopyGuarded(&manager, g_barSlot, sizeof(manager)) || manager == nullptr)
        {
            return;
        }
        const int seat = overlay_pass::SeatOf(column);
        if (seat < 1)
        {
            Note(column, seat, Choice::Live);
            return;
        }
        const WeakKey key = ColumnKey(column, manager);  // empty when the seat has no target
        Note(column, seat, key.object != nullptr ? Choice::Seat : Choice::Empty);
        slot_ = static_cast<std::uint8_t*>(manager) + facts::kEnemyBarDisplayedTarget;
        held_ = platform::CopyGuarded(&saved_, slot_, sizeof(saved_)) && platform::WriteGuarded(slot_, &key, sizeof(key));
    }
    ~ColumnKeyScope()
    {
        if (held_)
        {
            platform::WriteGuarded(slot_, &saved_, sizeof(saved_));
        }
    }
    ColumnKeyScope(const ColumnKeyScope&)            = delete;
    ColumnKeyScope& operator=(const ColumnKeyScope&) = delete;

private:
    enum class Choice : std::uint8_t
    {
        Unset,
        Live,   // a mirror's column: player 1's target
        Seat,   // the column's player's own target
        Empty,  // the column's player has none
    };
    /// Which key the column's labels follow, logged when it changes (an event: Debug).
    static void Note(int column, int seat, Choice choice) noexcept
    {
        static Choice s_last[kMaxColumns]{};
        if (s_last[column] == choice)
        {
            return;
        }
        s_last[column] = choice;
        platform::LogDebug(L"HUD markers: column {}'s ability labels follow {}{}", column + 1,
                           choice == Choice::Live ? L"player 1's target (a mirror's column)" : players::SeatName(seat),
                           choice == Choice::Seat ? L"'s target" : choice == Choice::Empty ? L" with no target: none" : L"");
    }

    std::uint8_t* slot_ = nullptr;
    WeakKey       saved_{};
    bool          held_ = false;
};

/// The visibility the tick sets right after a placement, for this replay's projection.
void ApplyVisibility(const TextCall& text, bool projected) noexcept
{
    if (text.kind == TextKind::Slate)
    {
        alignas(16) std::uint8_t attribute[0x60]{};  // TAttribute<EVisibility>: value, set, an unbound getter (+0x30..)
        void*                    made   = nullptr;
        void**                   vtable = nullptr;
        void*                    fn     = nullptr;
        if (platform::CallGuarded(g_attribute, made, static_cast<void*>(attribute),
                                  projected ? g_shownVisibility : g_hiddenVisibility) &&
            platform::CopyGuarded(&vtable, text.widget.object, sizeof(vtable)) && vtable != nullptr &&
            platform::CopyGuarded(&fn, reinterpret_cast<std::uint8_t*>(vtable) + facts::kSlateSetVisibility, sizeof(fn)) &&
            platform::CallGuarded(reinterpret_cast<SlateVisibilityFn>(fn), text.widget.object, static_cast<void*>(attribute)))
        {
            ++g_seen.visibilities;
            return;
        }
        Fault(3, L"a damage number's visibility");
    }
    else if (text.kind == TextKind::Umg)
    {
        // The core's widget is the UUserWidget's SObjectWidget; its WidgetObject leads back, checked against MyGCWidget.
        void*  user   = nullptr;
        void*  gc     = nullptr;
        void** vtable = nullptr;
        void*  fn     = nullptr;
        if (!platform::CopyGuarded(&user, static_cast<const std::uint8_t*>(text.widget.object) + facts::kObjectWidgetUser,
                                   sizeof(user)) ||
            user == nullptr ||
            !platform::CopyGuarded(&gc, static_cast<const std::uint8_t*>(user) + facts::kWidgetGcWidget, sizeof(gc)) ||
            gc != text.widget.object)
        {
            ++g_seen.umgUnmatched;
            return;
        }
        if (platform::CopyGuarded(&vtable, user, sizeof(vtable)) && vtable != nullptr &&
            platform::CopyGuarded(&fn, reinterpret_cast<std::uint8_t*>(vtable) + facts::kWidgetSetVisibility, sizeof(fn)) &&
            platform::CallGuarded(reinterpret_cast<WidgetVisibilityFn>(fn), user, static_cast<std::uint8_t>(projected ? 3 : 2)))
        {
            ++g_seen.visibilities;
            return;
        }
        Fault(4, L"a battle text's visibility");
    }
}

void ReplayTexts(int column, void* controller) noexcept
{
    for (auto& text : g_texts)
    {
        GameVector2D out;
        bool         projected = false;
        if (!platform::CallGuarded(reinterpret_cast<PlaceTextFn>(g_text.Original()), &out,
                                   controller != nullptr && text.controller != nullptr ? controller : text.controller,
                                   static_cast<const SharedPtr*>(&text.canvas), static_cast<const SharedPtr*>(&text.widget),
                                   static_cast<const GameVector*>(&text.location), text.offset, text.clamp, &projected))
        {
            Fault(5, L"a battle text");
            continue;
        }
        ApplyVisibility(text, projected);
        if (&text == &g_texts.front())
        {
            g_seen.text                  = text.widget.object;
            g_seen.textAt[column][0]     = out.x;
            g_seen.textAt[column][1]     = out.y;
            g_seen.textProjected[column] = projected;
        }
    }
}

void SamplePoint(void* widget, float (&into)[2]) noexcept
{
    platform::CopyGuarded(into, static_cast<const std::uint8_t*>(widget) + facts::kUserWidgetViewportOffsets, sizeof(into));
}

/// The cursors and lock-on markers placed again with this pass's controller (a call the game made without one stays so).
void ReplayPoints(int column, void* controller) noexcept
{
    const int slot = 1 + column;  // the sample's row for this column ([0] is the tick's)
    for (auto& call : g_cursors)
    {
        if (!Alive(call.widget, call.weak))
        {
            continue;
        }
        const bool sample = &call == &g_cursors.front();
        if (sample && column != 0)
        {
            g_seen.cursor = call.widget;
            SamplePoint(call.widget, g_seen.cursorAt[0]);
        }
        void* pc     = controller != nullptr && call.controller != nullptr ? controller : call.controller;
        bool  placed = false;
        if (!platform::CallGuarded(reinterpret_cast<CursorPlaceFn>(g_cursor.Original()), placed, pc, call.widget,
                                   static_cast<const GameVector*>(&call.location)))
        {
            Fault(6, L"the target cursor");
        }
        if (sample)
        {
            SamplePoint(call.widget, g_seen.cursorAt[slot]);
        }
    }
    for (auto& call : g_lockons)
    {
        if (!Alive(call.widget, call.weak))
        {
            continue;
        }
        const bool sample = &call == &g_lockons.front();
        if (sample && column != 0)
        {
            g_seen.lockon = call.widget;
            SamplePoint(call.widget, g_seen.lockonAt[0]);
        }
        void* pc = controller != nullptr && call.controller != nullptr ? controller : call.controller;
        if (!platform::CallGuarded(reinterpret_cast<LockonPlaceFn>(g_lockon.Original()), pc, call.widget,
                                   static_cast<const GameVector*>(&call.location), call.show))
        {
            Fault(7, L"a lock-on marker");
        }
        if (sample)
        {
            SamplePoint(call.widget, g_seen.lockonAt[slot]);
        }
    }
}

/// A manager whose TSet holds elements (its tick is not run again for an empty list).
void* ManagerWithElements(void** slot, std::uint32_t numBitsOffset) noexcept
{
    void*        manager = nullptr;
    std::int32_t count   = 0;
    if (slot == nullptr || !platform::CopyGuarded(&manager, slot, sizeof(manager)) || manager == nullptr ||
        !platform::CopyGuarded(&count, static_cast<const std::uint8_t*>(manager) + numBitsOffset, sizeof(count)) || count <= 0)
    {
        return nullptr;
    }
    return manager;
}

/// The labels' and weak points' ticks again, their controller lookups answering `controller` (nullptr: the widgets' own,
/// player 1's, as the game's tick). Labels after the bars (they read the displayed target's bar point); weak points with
/// the facing gate at 0, so only their placement runs.
void RerunTicks(void* controller) noexcept
{
    if (!g_labelsContext.IsInstalled() || !g_labelsContext2.IsInstalled() || !g_weakContext.IsInstalled())
    {
        return;
    }
    g_contextOverride = controller;
    if (void* labels = ManagerWithElements(g_labelsSlot, facts::kLabelsNumBits))
    {
        if (platform::CallGuarded(g_labelsTick, labels))
        {
            ++g_seen.labelRuns;
        }
        else
        {
            Fault(8, L"the ability labels' tick");
        }
    }
    if (void* weak = ManagerWithElements(g_weakSlot, facts::kWeakPointsNumBits))
    {
        std::uint8_t gate = 0;
        const std::uint8_t off = 0;
        if (platform::CopyGuarded(&gate, weak, 1) && platform::WriteGuarded(weak, &off, 1))
        {
            if (platform::CallGuarded(g_weakTick, weak))
            {
                ++g_seen.weakRuns;
            }
            else
            {
                Fault(9, L"the weak points' tick");
            }
            platform::WriteGuarded(weak, &gate, 1);
        }
    }
    g_contextOverride = nullptr;
}

}  // namespace

std::wstring Reading();  // hud.markers (below)

bool Install(const platform::GameImage& image) noexcept
{
    const auto untargeted = image.Rebase(facts::kBarPlaceUntargeted, 16);
    const auto targeted   = image.Rebase(facts::kBarPlaceTargeted, 16);
    const auto text       = image.Rebase(facts::kBattleTextPlace, 16);
    const auto position   = image.Rebase(facts::kSetPositionInViewport);
    if (!untargeted || !targeted || !text || !position ||
        !image.BytesMatch(facts::kBarPlaceUntargeted, facts::kBarPlaceUntargetedFirst) ||
        !image.BytesMatch(facts::kBarPlaceTargeted, facts::kBarPlaceTargetedFirst) ||
        !image.BytesMatch(facts::kBattleTextPlace, facts::kBattleTextPlaceFirst))
    {
        platform::Log(L"HUD markers: the battle marker placements are not what the research read; markers stay player 1's");
        return false;
    }
    g_untargetedAt = *untargeted;
    g_targetedAt   = *targeted;
    g_textAt       = *text;
    g_setPosition  = reinterpret_cast<SetPositionFn>(*position);
    if (const auto contextPc = platform::FindFunction(image, facts::kPlayerContextController))
    {
        g_contextPc = reinterpret_cast<ContextPcFn>(*contextPc);
    }
    const auto release   = image.Rebase(facts::kSharedPtrRelease, 16);
    const auto attribute = image.Rebase(facts::kVisibilityAttribute, 16);
    const auto shown     = image.Rebase(facts::kTextVisibilityShown, 1);
    const auto hidden    = image.Rebase(facts::kTextVisibilityHidden, 1);
    const auto umg       = image.Rebase(facts::kTextTickUmgReturn);
    const auto number    = image.Rebase(facts::kTextTickNumberReturn);
    const auto textSite  = image.Rebase(facts::kTextTickTextReturn);
    const auto wrapper   = image.Rebase(facts::kTextUmgWrapperReturn);
    if (!release || !attribute || !shown || !hidden || !umg || !number || !textSite || !wrapper ||
        !image.BytesMatch(facts::kSharedPtrRelease, facts::kSharedPtrReleaseFirst) ||
        !image.BytesMatch(facts::kVisibilityAttribute, facts::kVisibilityAttributeFirst))
    {
        platform::Log(L"HUD markers: the battle text's pointer release or visibility is not what the research read; markers "
                      L"stay player 1's");
        return false;
    }
    g_release          = reinterpret_cast<SharedReleaseFn>(*release);
    g_attribute        = reinterpret_cast<AttributeFn>(*attribute);
    // The bars' class per column: every function the further columns' classes call, or they keep the tick's class.
    {
        const auto barSlot   = players::game_globals::UiSlotAddress(facts::kEnemyBarSlot);
        const auto main      = platform::FindFunction(image, facts::kMainTargetOfLockOn);
        const auto weakFree  = platform::FindFunction(image, facts::kWeakPtrRelease);
        const auto dtAlpha   = platform::FindFunction(image, facts::kGameStateDtAlpha);
        const auto setVis    = platform::FindFunction(image, facts::kWidgetSetVisibilityFn);
        const auto getVis    = platform::FindFunction(image, facts::kWidgetGetVisibilityFn);
        const auto targetable = platform::FindFunction(image, facts::kBattleUnitIsTargetable);
        const auto icons     = platform::FindFunction(image, facts::kBarStatusIconsUpdate);
        const auto transform = platform::FindFunction(image, facts::kWidgetUpdateRenderTransform);
        g_ownClasses = barSlot && main && weakFree && dtAlpha && setVis && getVis && targetable && icons && transform;
        if (g_ownClasses)
        {
            try
            {
                g_looks.reserve(64);  // four looks per bar re-classed; the pushes stay guarded for the rest
                g_eases.reserve(16);
                g_refreshed.reserve(16);
            }
            catch (...)
            {
            }
            g_barSlot         = reinterpret_cast<void**>(barSlot);
            g_mainTarget      = reinterpret_cast<MainTargetFn>(*main);
            g_weakRelease     = reinterpret_cast<WeakReleaseFn>(*weakFree);
            g_dtAlpha         = reinterpret_cast<DtAlphaFn>(*dtAlpha);
            g_setVisibility   = reinterpret_cast<WidgetVisibilityFn>(*setVis);
            g_getVisibility   = reinterpret_cast<GetVisibilityFn>(*getVis);
            g_isTargetable    = reinterpret_cast<IsTargetableFn>(*targetable);
            g_statusIcons     = reinterpret_cast<StatusIconsFn>(*icons);
            g_renderTransform = reinterpret_cast<WidgetFn>(*transform);
        }
        platform::Log(L"HUD markers: the enemy bars' class per view {}",
                      g_ownClasses ? L"follows each player's own target (the look set with the game's setters per pass)"
                                   : L"stays player 1's (a function check failed)");
    }
    g_shownVisibility  = reinterpret_cast<const std::uint8_t*>(*shown);
    g_hiddenVisibility = reinterpret_cast<const std::uint8_t*>(*hidden);
    g_umgReturn        = *umg;
    g_numberReturn     = *number;
    g_textReturn       = *textSite;
    g_wrapperReturn    = *wrapper;
    const auto cursor     = image.Rebase(facts::kCursorPlace, 16);
    const auto lockon     = image.Rebase(facts::kLockonPlace, 16);
    const auto labelsTick = image.Rebase(facts::kLabelsTick);
    const auto weakTick   = image.Rebase(facts::kWeakPointsTick);
    const auto labelsSite = image.Rebase(facts::kLabelsContextCall, 5);
    const auto labelsTwo  = image.Rebase(facts::kLabelsContextCall2, 5);
    const auto weakSite   = image.Rebase(facts::kWeakPointsContextCall, 5);
    const auto contextFn  = platform::FindFunction(image, facts::kPlayerContextController);
    const auto labelsSlot = players::game_globals::UiSlotAddress(facts::kLabelsSlot);
    const auto weakSlot   = players::game_globals::UiSlotAddress(facts::kWeakPointsSlot);
    if (cursor && lockon && image.BytesMatch(facts::kCursorPlace, facts::kCursorPlaceFirst) &&
        image.BytesMatch(facts::kLockonPlace, facts::kLockonPlaceFirst))
    {
        g_cursorAt = *cursor;
        g_lockonAt = *lockon;
    }
    else
    {
        platform::Log(L"HUD markers: the target cursor or lock-on placement is not what the research read; they stay "
                      L"player 1's");
    }
    if (labelsTick && weakTick && labelsSite && labelsTwo && weakSite && contextFn && labelsSlot && weakSlot &&
        image.BytesMatch(facts::kLabelsTick, facts::kLabelsTickFirst) &&
        image.BytesMatch(facts::kWeakPointsTick, facts::kWeakPointsTickFirst))
    {
        g_labelsTick  = reinterpret_cast<ManagerTickFn>(*labelsTick);
        g_weakTick    = reinterpret_cast<ManagerTickFn>(*weakTick);
        g_labelsSite  = *labelsSite;
        g_labelsSite2 = *labelsTwo;
        g_weakSite    = *weakSite;
        g_contextAt   = *contextFn;
        g_labelsSlot  = reinterpret_cast<void**>(labelsSlot);
        g_weakSlot    = reinterpret_cast<void**>(weakSlot);
    }
    else
    {
        platform::Log(L"HUD markers: the ability labels' or weak points' tick is not what the research read; they stay "
                      L"player 1's");
    }
    g_pending      = true;  // installed by the game thread's tick, outside these functions
    g_installed    = true;

    using control::Outcome;
    using control::Register;
    using control::Shape;
    using control::Value;
    Register({.name  = L"hud.markers",
              .shape = Shape::Text,
              .help  = L"(read only) battle markers since the last reading: placements recorded and replayed, one bar's "
                       L"position from the tick and each replay, the controller its swapped context resolves to, one text",
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
    Register({.name  = L"hud-markers",
              .shape = Shape::OnOff,
              .help  = L"battle markers (enemy bars, damage numbers) placed through each player's camera (on at start)",
              .set   = [](const Value& value) {
                  SetEnabled(std::get<bool>(value));
                  return Outcome{};
              },
              .get = [] { return Value{Enabled()}; }});
    return true;
}

void Remove() noexcept
{
    if (!g_installed)
    {
        return;
    }
    g_on = false;
    RestoreLooks();  // the tick's looks back, then nothing of the columns' kept
    g_eases.clear();
    g_refreshed.clear();
    g_bars.clear();
    ClearTexts();
    g_cursors.clear();
    g_lockons.clear();
    const bool ok = g_untargeted.Restore() & g_targeted.Restore() & g_text.Restore() & g_cursor.Restore() &
                    g_lockon.Restore() & g_labelsContext.Restore() & g_labelsContext2.Restore() & g_weakContext.Restore();
    if (!ok)
    {
        platform::Log(L"HUD markers: a placement entry was changed after us; left in place (it only records)");
    }
}

void Tick() noexcept
{
    // Before the world tick: a frame whose overlay render did not run (no EndFrame) leaves nothing for the next.
    g_bars.clear();
    ClearTexts();
    g_cursors.clear();
    g_lockons.clear();
    if (!g_pending)
    {
        return;
    }
    g_pending = false;
    const bool ok =
        g_untargeted.Install(g_untargetedAt, facts::kBarPlaceUntargetedFirst, reinterpret_cast<std::uintptr_t>(&Untargeted)) &&
        g_targeted.Install(g_targetedAt, facts::kBarPlaceTargetedFirst, reinterpret_cast<std::uintptr_t>(&Targeted)) &&
        g_text.Install(g_textAt, facts::kBattleTextPlaceFirst, reinterpret_cast<std::uintptr_t>(&Text));
    if (!ok)
    {
        g_untargeted.Restore();
        g_targeted.Restore();
        g_text.Restore();
    }
    platform::Log(L"HUD markers: {}", ok ? L"enemy bars and battle text recorded for each player's replay (hud-markers on)"
                                         : L"the placement entries could not be redirected; markers stay player 1's");
    if (!ok)
    {
        return;
    }
    // The cursor and lock-on placements, and the labels' and weak points' controller lookups. From the game thread here,
    // outside every one of these functions (one of the calls crosses a block).
    bool points = false;
    if (g_cursorAt != 0 && g_lockonAt != 0)
    {
        points = g_cursor.Install(g_cursorAt, facts::kCursorPlaceFirst, reinterpret_cast<std::uintptr_t>(&Cursor)) &&
                 g_lockon.Install(g_lockonAt, facts::kLockonPlaceFirst, reinterpret_cast<std::uintptr_t>(&Lockon));
        if (!points)
        {
            g_cursor.Restore();
            g_lockon.Restore();
        }
    }
    bool reruns = false;
    if (g_labelsTick != nullptr)
    {
        const auto replacement = reinterpret_cast<std::uintptr_t>(&ContextController);
        reruns = g_labelsContext.InstallFromOwnThread(g_labelsSite, g_contextAt, replacement) &&
                 g_labelsContext2.Install(g_labelsSite2, g_contextAt, replacement) &&
                 g_weakContext.Install(g_weakSite, g_contextAt, replacement);
        if (!reruns)
        {
            g_labelsContext.Restore();
            g_labelsContext2.Restore();
            g_weakContext.Restore();
        }
    }
    platform::Log(L"HUD markers: target cursor and lock-on markers {}; ability labels and weak points {}",
                  points ? L"recorded for each player's replay" : L"stay player 1's (not installed)",
                  reruns ? L"placed again for each player" : L"stay player 1's (not installed)");
}

void Replay(int column, void* localPlayer, void* controller) noexcept
{
    if (!g_on || localPlayer == nullptr || column < 0 || column >= kMaxColumns)
    {
        RestoreLooks();  // player 1's pass without a replay still renders the tick's looks
        return;
    }
    RestoreLooks();  // every pass starts from the tick's looks (the further columns' passes set their own)
    void* pass = column != 0 ? controller : nullptr;  // column 0's pass: the game's own inputs, as its tick
    if (g_bars.empty() && g_texts.empty())
    {
        g_replaying = true;
        ReplayPoints(column, pass);
        {
            const ColumnKeyScope key(column);
            RerunTicks(pass);
        }
        g_replaying = false;
        return;
    }
    std::int32_t context[2]{};
    if (!players::weak_object::WeakPairOf(localPlayer, context))
    {
        return;  // the looks are the tick's already (above)
    }
    g_replaying = true;
    ReplayBars(column, context, controller);
    ReplayTexts(column, pass);  // column 0: each text's own stored controller, as the tick
    ReplayPoints(column, pass);
    {
        const ColumnKeyScope key(column);  // the label over this column's own target's bar
        RerunTicks(pass);                  // after the bars: the labels read the displayed target's bar point
    }
    g_replaying = false;
    g_replayed[column] += static_cast<std::uint32_t>(g_bars.size() + g_texts.size());
    g_seen.replays[column] += static_cast<std::uint32_t>(g_bars.size() + g_texts.size());
}

void EndFrame() noexcept
{
    RestoreLooks();  // if player 1's pass did not come to put the tick's looks back
    g_refreshed.clear();
    std::erase_if(g_eases, [](const OwnEase& own) { return !own.seen; });  // a bar not replayed this frame: its ease gone
    for (auto& own : g_eases)
    {
        own.seen = false;
    }
    ++g_seen.frames;
    const auto recorded = static_cast<std::uint32_t>(g_bars.size() + g_texts.size());
    if (recorded != 0)
    {
        ++g_seen.framesWithMarkers;
        if (g_run++ == 0)
        {
            platform::Log(L"HUD markers: placements recorded ({} bars, {} texts this frame); replays for the further columns: {}",
                          g_bars.size(), g_texts.size(), FurtherReplayed() != 0 ? L"made" : L"none yet");
        }
        g_runRecorded += recorded;
    }
    else if (g_run != 0)
    {
        platform::Log(L"HUD markers: no placement this frame, after {} frames with {} ({} replayed for the further columns)",
                      g_run, g_runRecorded, FurtherReplayed());
        g_run = g_runRecorded = 0;
    }
    if (!g_bars.empty() || !g_texts.empty())
    {
        if (++g_frames >= 300)
        {
            platform::LogDebug(L"HUD markers: over 300 frames, {} placements replayed for player 1, {} for the further columns",
                               g_replayed[0], FurtherReplayed());
            g_frames = 0;
            std::memset(g_replayed, 0, sizeof(g_replayed));
        }
    }
    g_bars.clear();
    ClearTexts();
    g_cursors.clear();
    g_lockons.clear();
}

std::wstring Reading()
{
    const Seen&  s = g_seen;
    std::wstring text = std::format(
        L"{} frames, {} with markers; recorded {} untargeted, {} targeted, {} texts (UMG {}, Slate {}, other {}); replayed "
        L"column 3 {} column 2 {} column 1 {}; visibilities {}; UMG unmatched {}; faults {}",
        s.frames, s.framesWithMarkers, s.untargeted, s.targeted, s.texts, s.textKinds[1], s.textKinds[2], s.textKinds[0],
        s.replays[2], s.replays[1], s.replays[0], s.visibilities, s.umgUnmatched, s.faults);
    if (s.bar != nullptr)
    {
        text += std::format(L" | bar {} ({}): tick ({:.0f},{:.0f}) column 2 ({:.0f},{:.0f}) column 1 ({:.0f},{:.0f}); context {}/{} -> "
                            L"a further column's {}/{}: resolves {} (that column's {}: {})",
                            s.bar, s.barTargeted ? L"targeted" : L"untargeted", s.barTick[0], s.barTick[1], s.barAt[1][0],
                            s.barAt[1][1], s.barAt[0][0], s.barAt[0][1], s.contextBefore[0], s.contextBefore[1],
                            s.contextFurther[0], s.contextFurther[1], s.pcUnderSwap, s.pcFurther,
                            s.pcUnderSwap == s.pcFurther ? L"same" : L"DIFFERENT");
    }
    text += std::format(L" | classes: bars classed by the column's own target: column 2 {}, column 3 {} ({} looks set, {} "
                        L"kept before a targeted placement, {} put back after one, {} restored, {} status-icon refreshes), "
                        L"p1 rule disagreements {}; sample bar: tick {}, further {}, its visibility {} -> {}, further ease {:.1f}",
                        s.reclassed[1], s.reclassed[2], s.looksSet, s.keptBeforePlace, s.putBackAfterPlace, s.looksRestored,
                        s.iconsRefreshed, s.disagreements,
                        s.sampleTickTargeted ? L"targeted" : L"untargeted", s.sampleOwnTargeted ? L"targeted" : L"untargeted",
                        s.sampleBarBefore, s.sampleBarAfter, s.sampleEase);
    text += std::format(L" | cursors {}, lock-on markers {} recorded; labels run {}, weak points run {}, lookups answered {}",
                        s.cursors, s.lockons, s.labelRuns, s.weakRuns, s.overridden);
    if (s.cursor != nullptr)
    {
        text += std::format(L" | cursor {}: tick ({:.0f},{:.0f}) column 2 ({:.0f},{:.0f}) column 1 ({:.0f},{:.0f})", s.cursor,
                            s.cursorAt[0][0], s.cursorAt[0][1], s.cursorAt[2][0], s.cursorAt[2][1], s.cursorAt[1][0],
                            s.cursorAt[1][1]);
    }
    if (s.lockon != nullptr)
    {
        text += std::format(L" | lock-on {}: tick ({:.0f},{:.0f}) column 2 ({:.0f},{:.0f}) column 1 ({:.0f},{:.0f})", s.lockon,
                            s.lockonAt[0][0], s.lockonAt[0][1], s.lockonAt[2][0], s.lockonAt[2][1], s.lockonAt[1][0],
                            s.lockonAt[1][1]);
    }
    if (s.text != nullptr)
    {
        text += std::format(L" | text {}: column 2 ({:.0f},{:.0f}{}) column 1 ({:.0f},{:.0f}{})", s.text, s.textAt[1][0], s.textAt[1][1],
                            s.textProjected[1] ? L"" : L" off", s.textAt[0][0], s.textAt[0][1], s.textProjected[0] ? L"" : L" off");
    }
    g_seen = {};
    return text;
}

void SetEnabled(bool on) noexcept
{
    g_on = on;
}

bool Enabled() noexcept
{
    return g_on && g_installed;
}

}  // namespace lss::hud::marker_replay
