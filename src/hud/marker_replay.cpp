#include "hud/marker_replay.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "hud/overlay_pass.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/player_two_camera.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <intrin.h>

#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace lss::hud::marker_replay {

namespace {

struct Vector3
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};
struct Vector2
{
    float x = 0.0f, y = 0.0f;
};
static_assert(sizeof(Vector2) == 8);

/// TWeakPtr (thread-safe): the object and its reference controller (weak count at +0xC).
struct WeakKey
{
    void* object     = nullptr;
    void* controller = nullptr;
};

using PlaceUntargetedFn = void (*)(void* manager, void* widget, const Vector3* location);
using PlaceTargetedFn   = void (*)(void* manager, void* widget, WeakKey* key, const Vector3* location, std::int32_t scratch,
                                 float halfSize, std::uint8_t skipLeaderClamp);
/// TSharedPtr, NotThreadSafe (the battle text's Slate pointers: the game copies them with a plain `inc [ctrl+8]`).
struct SharedPtr
{
    void* object     = nullptr;
    void* controller = nullptr;
};

using PlaceTextFn        = void (*)(Vector2* out, void* controller, const SharedPtr* canvas, const SharedPtr* widget,
                                    const Vector3* location, Vector2 offset, std::uint8_t clamp, bool* projected);
using SharedReleaseFn    = void (*)(SharedPtr* pointer);  // the game's ~TSharedPtr
using AttributeFn        = void* (*)(void* attribute, const std::uint8_t* visibility);
using SlateVisibilityFn  = void (*)(void* widget, void* attribute);  // takes the TAttribute by value: it destroys it
using WidgetVisibilityFn = void (*)(void* widget, std::uint8_t visibility);
using CursorPlaceFn      = bool (*)(void* controller, void* widget, const Vector3* location);
using LockonPlaceFn      = void (*)(void* controller, void* widget, const Vector3* location, std::uint8_t show);
using ManagerTickFn      = void (*)(void* manager);
using SetPositionFn     = void (*)(void* widget, Vector2 position, bool removeDpiScale);
using ContextPcFn       = void* (*)(const void* context);

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

// Game thread only.
bool g_replaying = false;

struct BarCall
{
    void*        manager = nullptr;
    void*        widget  = nullptr;
    std::int32_t weak[2] = {-1, 0};  // the widget's weak pair: it is checked before the replay touches it
    bool         targeted = false;
    Vector3      location;
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
    Vector3      location;
    Vector2      offset;
    std::uint8_t clamp = 0;
    TextKind     kind  = TextKind::Other;
};
/// A target cursor or lock-on marker placement (both pure, the controller an argument).
struct PointCall
{
    void*        controller = nullptr;
    void*        widget     = nullptr;
    std::int32_t weak[2]    = {-1, 0};
    Vector3      location;
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
};
Seen          g_seen;
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
    return widget != nullptr && players::player_two_camera::ObjectOf(weak) == widget;
}

/// Last call per widget wins (the tick may place a bar twice: creation and frame).
template <class Call>
void Keep(std::vector<Call>& calls, const Call& call)
{
    for (auto& known : calls)
    {
        if (known.widget == call.widget)
        {
            known = call;
            return;
        }
    }
    calls.push_back(call);
}

void Untargeted(void* manager, void* widget, const Vector3* location)
{
    if (!g_replaying && g_on && location != nullptr)
    {
        BarCall call{manager, widget};
        call.location = *location;
        players::player_two_camera::WeakPairOf(widget, call.weak);
        Keep(g_bars, call);
        ++g_seen.untargeted;
    }
    reinterpret_cast<PlaceUntargetedFn>(g_untargeted.Original())(manager, widget, location);
}

void Targeted(void* manager, void* widget, WeakKey* key, const Vector3* location, std::int32_t scratch, float halfSize,
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
        players::player_two_camera::WeakPairOf(widget, call.weak);
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

void Text(Vector2* out, void* controller, const SharedPtr* canvas, const SharedPtr* widget, const Vector3* location,
          Vector2 offset, std::uint8_t clamp, bool* projected)
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
            g_texts.push_back(call);
        }
        ++g_seen.texts;
        ++g_seen.textKinds[static_cast<int>(kind)];
    }
    reinterpret_cast<PlaceTextFn>(g_text.Original())(out, controller, canvas, widget, location, offset, clamp, projected);
}

/// Last call per widget wins.
void KeepPoint(std::vector<PointCall>& calls, const PointCall& call)
{
    for (auto& known : calls)
    {
        if (known.widget == call.widget)
        {
            known = call;
            return;
        }
    }
    calls.push_back(call);
}

bool Cursor(void* controller, void* widget, const Vector3* location)
{
    if (!g_replaying && g_on && location != nullptr && widget != nullptr)
    {
        PointCall call{controller, widget};
        call.location = *location;
        players::player_two_camera::WeakPairOf(widget, call.weak);
        KeepPoint(g_cursors, call);
        ++g_seen.cursors;
    }
    return reinterpret_cast<CursorPlaceFn>(g_cursor.Original())(controller, widget, location);
}

void Lockon(void* controller, void* widget, const Vector3* location, std::uint8_t show)
{
    if (!g_replaying && g_on && location != nullptr && widget != nullptr)
    {
        PointCall call{controller, widget};
        call.location = *location;
        call.show     = show;
        players::player_two_camera::WeakPairOf(widget, call.weak);
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

/// The bar entry (p = element + 0x10) whose weak widget is `widget`, from the manager's set (RE-hud-battle-markers.md
/// §2.4: data +0x08, NumBits +0x30, 0x150 per element, the weak widget at p+0).
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
/// `lock inc [ctrl+0xC]` at 0x1413DF7B3).
void AddWeakReference(void* controller) noexcept
{
    if (controller != nullptr)
    {
        _InterlockedIncrement(reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(controller) + 0xC));
    }
}

void ReplayBars(int column, const std::int32_t (&context)[2], void* controller) noexcept
{
    for (auto& bar : g_bars)
    {
        if (!Alive(bar.widget, bar.weak))
        {
            continue;
        }
        // A targeted placement consumes one weak reference on the key: replayed only while the manager's entry for this
        // widget still holds that key (its own reference keeps the controller alive).
        if (bar.targeted)
        {
            std::uint8_t* entry = BarEntryOf(bar.manager, bar.widget);
            void* held = nullptr;
            if (entry == nullptr || !platform::CopyGuarded(&held, entry - 0x10 + 8, sizeof(held)) ||
                held != bar.key.controller)
            {
                continue;
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
        if (!bar.targeted)
        {
            if (!platform::CallGuarded(reinterpret_cast<PlaceUntargetedFn>(g_untargeted.Original()), bar.manager, bar.widget,
                                       static_cast<const Vector3*>(&bar.location)))
            {
                Fault(1, L"an enemy bar");
            }
        }
        else
        {
            WeakKey key = bar.key;
            AddWeakReference(key.controller);
            if (!platform::CallGuarded(reinterpret_cast<PlaceTargetedFn>(g_targeted.Original()), bar.manager, bar.widget, &key,
                                       static_cast<const Vector3*>(&bar.location), std::int32_t{0}, bar.halfSize, bar.skip))
            {
                Fault(2, L"the targeted enemy bar");
            }
            // The displayed target's bar rides its y-ease above the point (applied, not stepped).
            void* displayed = nullptr;
            std::uint8_t* p = nullptr;
            if (platform::CopyGuarded(&displayed, static_cast<const std::uint8_t*>(bar.manager) + 0xD8, sizeof(displayed)) &&
                displayed == bar.key.object && (p = BarEntryOf(bar.manager, bar.widget)) != nullptr)
            {
                float point[2]{}, ease = 0.0f;
                if (platform::CopyGuarded(point, p + 0xB4, sizeof(point)) && platform::CopyGuarded(&ease, p + 0x110, sizeof(ease)))
                {
                    platform::CallGuarded(g_setPosition, bar.widget, Vector2{point[0], point[1] + ease}, false);
                }
            }
        }
        platform::WriteGuarded(widget + facts::kUserWidgetPlayerWeak, saved, sizeof(saved));
        if (sample)
        {
            platform::CopyGuarded(g_seen.barAt[column], widget + facts::kUserWidgetViewportOffsets, sizeof(g_seen.barAt[column]));
        }
    }
}

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
        Vector2 out;
        bool    projected = false;
        if (!platform::CallGuarded(reinterpret_cast<PlaceTextFn>(g_text.Original()), &out,
                                   controller != nullptr && text.controller != nullptr ? controller : text.controller,
                                   static_cast<const SharedPtr*>(&text.canvas), static_cast<const SharedPtr*>(&text.widget),
                                   static_cast<const Vector3*>(&text.location), text.offset, text.clamp, &projected))
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
                                   static_cast<const Vector3*>(&call.location)))
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
                                   static_cast<const Vector3*>(&call.location), call.show))
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
    if (const auto contextPc = image.Rebase(facts::kPlayerContextController, 16);
        contextPc && image.BytesMatch(facts::kPlayerContextController, facts::kPlayerContextControllerFirst))
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
    const auto contextFn  = image.Rebase(facts::kPlayerContextController, 16);
    const auto slots      = image.Rebase(facts::kEndUISystemSlots, 64 * sizeof(void*));
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
    if (labelsTick && weakTick && labelsSite && labelsTwo && weakSite && contextFn && slots &&
        image.BytesMatch(facts::kLabelsTick, facts::kLabelsTickFirst) &&
        image.BytesMatch(facts::kWeakPointsTick, facts::kWeakPointsTickFirst) &&
        image.BytesMatch(facts::kPlayerContextController, facts::kPlayerContextControllerFirst))
    {
        g_labelsTick  = reinterpret_cast<ManagerTickFn>(*labelsTick);
        g_weakTick    = reinterpret_cast<ManagerTickFn>(*weakTick);
        g_labelsSite  = *labelsSite;
        g_labelsSite2 = *labelsTwo;
        g_weakSite    = *weakSite;
        g_contextAt   = *contextFn;
        g_labelsSlot  = reinterpret_cast<void**>(*slots) + facts::kLabelsSlot;
        g_weakSlot    = reinterpret_cast<void**>(*slots) + facts::kWeakPointsSlot;
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
        return;
    }
    void* pass = column != 0 ? controller : nullptr;  // column 0's pass: the game's own inputs, as its tick
    if (g_bars.empty() && g_texts.empty())
    {
        g_replaying = true;
        ReplayPoints(column, pass);
        RerunTicks(pass);
        g_replaying = false;
        return;
    }
    std::int32_t context[2]{};
    if (!players::player_two_camera::WeakPairOf(localPlayer, context))
    {
        return;
    }
    g_replaying = true;
    ReplayBars(column, context, controller);
    ReplayTexts(column, pass);  // column 0: each text's own stored controller, as the tick
    ReplayPoints(column, pass);
    RerunTicks(pass);           // after the bars: the labels read the displayed target's bar point
    g_replaying = false;
    g_replayed[column] += static_cast<std::uint32_t>(g_bars.size() + g_texts.size());
    g_seen.replays[column] += static_cast<std::uint32_t>(g_bars.size() + g_texts.size());
}

void EndFrame() noexcept
{
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
        L"p2 {} p1 {}; visibilities {}; UMG unmatched {}; faults {}",
        s.frames, s.framesWithMarkers, s.untargeted, s.targeted, s.texts, s.textKinds[1], s.textKinds[2], s.textKinds[0],
        s.replays[1], s.replays[0], s.visibilities, s.umgUnmatched, s.faults);
    if (s.bar != nullptr)
    {
        text += std::format(L" | bar {} ({}): tick ({:.0f},{:.0f}) p2 ({:.0f},{:.0f}) p1 ({:.0f},{:.0f}); context {}/{} -> "
                            L"p2 {}/{}: resolves {} (p2's {}: {})",
                            s.bar, s.barTargeted ? L"targeted" : L"untargeted", s.barTick[0], s.barTick[1], s.barAt[1][0],
                            s.barAt[1][1], s.barAt[0][0], s.barAt[0][1], s.contextBefore[0], s.contextBefore[1],
                            s.contextFurther[0], s.contextFurther[1], s.pcUnderSwap, s.pcFurther,
                            s.pcUnderSwap == s.pcFurther ? L"same" : L"DIFFERENT");
    }
    text += std::format(L" | cursors {}, lock-on markers {} recorded; labels run {}, weak points run {}, lookups answered {}",
                        s.cursors, s.lockons, s.labelRuns, s.weakRuns, s.overridden);
    if (s.cursor != nullptr)
    {
        text += std::format(L" | cursor {}: tick ({:.0f},{:.0f}) p2 ({:.0f},{:.0f}) p1 ({:.0f},{:.0f})", s.cursor,
                            s.cursorAt[0][0], s.cursorAt[0][1], s.cursorAt[2][0], s.cursorAt[2][1], s.cursorAt[1][0],
                            s.cursorAt[1][1]);
    }
    if (s.lockon != nullptr)
    {
        text += std::format(L" | lock-on {}: tick ({:.0f},{:.0f}) p2 ({:.0f},{:.0f}) p1 ({:.0f},{:.0f})", s.lockon,
                            s.lockonAt[0][0], s.lockonAt[0][1], s.lockonAt[2][0], s.lockonAt[2][1], s.lockonAt[1][0],
                            s.lockonAt[1][1]);
    }
    if (s.text != nullptr)
    {
        text += std::format(L" | text {}: p2 ({:.0f},{:.0f}{}) p1 ({:.0f},{:.0f}{})", s.text, s.textAt[1][0], s.textAt[1][1],
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
