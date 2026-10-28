#include "hud/minimap.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>

#include "control/registry.h"
#include "engine_facts.h"
#include "hud/image_code.h"
#include "hud/minimap_rules.h"
#include "hud/overlay_layout.h"
#include "hud/overlay_pass.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/scopes.h"
#include "players/seat_objects.h"
#include "players/seats.h"

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace lss::hud::minimap {

namespace {

using platform::Read;
using players::kSeatsBuilt;
using players::SeatBuilt;
using players::SeatName;

namespace f = facts;
using U64 = std::uint64_t;

constexpr int kMaxPlayers = players::scopes::kMaxPlayers;
static_assert(kSeatsBuilt < kMaxPlayers, "a record per built seat, player 1's beside them");

using TickFn        = void (*)(void* manager, float deltaTime);
using MgrFn         = U64 (*)(void* manager, U64 a2, U64 a3, U64 a4);  // the events' integer arguments, passed through
using AddIconFn     = U64 (*)(void* manager, U64 key, U64 a3, U64 a4, U64 a5, U64 a6);
using RemoveIconFn  = U64 (*)(void* manager, U64 key);
using FlagFn        = void (*)(void* manager, U64 value);
using EnabledFn     = void (*)(void* manager, U64 value, void* world);  // world 0: the game mode's (every game site)
using FirstPcFn     = void* (*)(void* world);
using AddToRootFn   = std::uint8_t (*)(void* widget, std::int32_t zOrder);
using FindOctagonFn = void* (*)(void* userWidget);
using AllocFn       = void* (*)(U64 size, std::uint32_t alignment);
using CtorFn        = void* (*)(void* memory);
using AddRawFn      = void* (*)(void* list, void* handleOut, void* object, const void* method);
using AcquireFn     = void* (*)(void* renderer, U64 pool);
using FindOrAddFn   = void* (*)(void* slot, U64 name, const void* rect);
using SubmitFn      = void (*)(void* renderer, U64 pool, void* slot);
/// FLinearColor as UUserWidget::SetColorAndOpacity reads it: `movaps xmm0, [rdx]` (0x1428F1C3B), so 16-byte aligned.
struct alignas(16) LinearColor
{
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
};
using SetColorFn    = void (*)(void* widget, const LinearColor* color);

struct Live
{
    TickFn        tick         = nullptr;
    MgrFn         setupMap     = nullptr;
    MgrFn         fieldStart   = nullptr;
    MgrFn         fieldEnd     = nullptr;
    MgrFn         reset        = nullptr;
    AddIconFn     addIcon      = nullptr;
    RemoveIconFn  removeIcon   = nullptr;
    FlagFn        setHidden    = nullptr;
    FlagFn        toggleVisible = nullptr;
    EnabledFn     setEnabled   = nullptr;
    FlagFn        setNaviMode  = nullptr;
    FlagFn        setCompassMode = nullptr;
    FirstPcFn     firstPc      = nullptr;
    AddToRootFn   addToRoot    = nullptr;
    FindOctagonFn findOctagon  = nullptr;
    AllocFn       alloc        = nullptr;
    CtorFn        ctor         = nullptr;
    AddRawFn      addRaw       = nullptr;
    AcquireFn     acquire      = nullptr;
    FindOrAddFn   findOrAdd    = nullptr;
    SubmitFn      submit       = nullptr;
    SetColorFn    setColor     = nullptr;
    std::uintptr_t bannerStart = 0, bannerEnd = 0;
    void**        slot29       = nullptr;  // &EndUISystem[29]
    void**        slot35       = nullptr;  // &EndUISystem[35] (the banner manager)
    std::uint8_t** partyManager = nullptr;
};
Live g;

platform::CallRedirect g_tickCall, g_firstPcCall, g_textureCall, g_entryCall, g_submitCall;
platform::CallRedirect g_setupCalls[2], g_startCall, g_endCall, g_resetCall;
platform::CallRedirect g_addCalls[4], g_removeCalls[5];
platform::CodeRedirect g_poolCode;
std::uintptr_t         g_removeLastSite = 0;  // crosses a block: installed from the game thread
bool                   g_removeLastPending = false;
bool                   g_installed = false;

// Read and written by the pool-0 code (hud/image_code.h): the paint interface of the octagon whose span is skipped in
// this pass, the last one that reached the site, the skips made.
image_code::NaviState g_navi;

// Game thread only.
bool  g_on       = true;
int   g_forSeat  = 0;  // the seat whose instance a game call runs on (0 none)
int   g_column   = overlay_pass::kSingle;  // the column whose pass runs (hud/overlay_pass.h)
void* g_renderer = nullptr;
void* g_heldSlot = nullptr;  // pool 0's slot, held from the frame's first further pass to column 0's
minimap_rules::PoolHandover g_pool;  // whether a slot is held, and by which column

/// One seat's minimap: its instance and what the module knows of its player.
struct Seat
{
    void*          manager     = nullptr;  // its navi map manager, made once and kept for the process
    void*          controller  = nullptr;  // its controller while its player is in
    bool           plays       = false;    // its player plays a member in the split
    bool           shown       = false;    // its instance is enabled for its player (follows player 1's flags)
    bool           faulted     = false;    // its tick faulted: its instance is left alone from then on
    void*          lastRoot    = nullptr;  // its root as last logged
    std::uintptr_t paint       = 0;        // its octagon's paint interface, as BeginPass resolved it
    std::uint32_t  ticks       = 0;        // its manager ticked, since the last reading
    std::uint32_t  scopeFailed = 0;        // its contexts could not be held (no tick then), since the last reading
    bool           notHeldLogged = false;  // the "not updated" line is out; the recovery line follows it
};
std::array<Seat, kMaxPlayers> g_seats;  // by seat; [0] holds player 1's octagon's paint interface only

/// What happened since the last `hud.minimap` reading, per pass (0 single, 1 a further column's, 2 player 1's).
struct Counts
{
    std::uint32_t acquiresBy[3][3] = {};  // [pass][octagon: player 1's, a seat's, another]
    std::uint32_t heldGiven[3]     = {};  // the held slot handed to an acquire
    std::uint32_t renamed          = 0;   // entries named with their column's number
    std::uint32_t submits[3]       = {};
    std::uint32_t submitsSkipped   = 0;
    std::uint32_t safetySubmits    = 0;
    std::uint32_t frames           = 0;
    std::uint64_t skipsAtRead      = 0;   // g_navi.skips at the last reading
};
Counts g_counts;

int PassIndex(int column) noexcept
{
    return column >= 1 ? 1 : column == 0 ? 2 : 0;
}

struct Faded
{
    void*       widget = nullptr;
    LinearColor color;
};
std::vector<Faded> g_faded;

std::uint8_t* At(void* object, std::uint32_t offset) noexcept
{
    return static_cast<std::uint8_t*>(object) + offset;
}

/// The manager is a seat's instance.
bool IsSeatManager(const void* manager) noexcept
{
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        if (manager != nullptr && manager == g_seats[seat].manager)
        {
            return true;
        }
    }
    return false;
}

void* One() noexcept
{
    void* manager = nullptr;
    Read(g.slot29, manager);
    return IsSeatManager(manager) ? nullptr : manager;  // never a seat's (it is in the slot during that seat's tick)
}

/// The column whose map an octagon is, by its paint interface: 0 player 1's, a seat's its column (hud/overlay_pass.h's
/// map), -1 another painter or a seat with no column.
int OwnerOf(std::uintptr_t paint) noexcept
{
    for (int seat = 0; paint != 0 && seat <= kSeatsBuilt; ++seat)
    {
        if (g_seats[seat].paint == paint)
        {
            return seat == 0 ? 0 : overlay_pass::ColumnOf(seat);
        }
    }
    return -1;
}

/// Names the octagon's span as skipped in this pass (the pool-0 code's slots, hud/image_code.h).
void SkipSpan(std::uintptr_t paint) noexcept
{
    if (paint == 0)
    {
        return;
    }
    std::uintptr_t* slots[image_code::kNaviSkips] = {&g_navi.skip, &g_navi.skipMore[0], &g_navi.skipMore[1]};
    for (std::uintptr_t* slot : slots)
    {
        if (*slot == paint)
        {
            return;
        }
        if (*slot == 0)
        {
            *slot = paint;
            return;
        }
    }
    static bool told = false;
    if (!told)
    {
        told = true;
        platform::LogWarning(L"HUD minimap: more octagons to skip in a pass than the pool-0 code holds ({}); one renders twice a frame",
                             image_code::kNaviSkips);
    }
}

void ClearSkips() noexcept
{
    g_navi.skip        = 0;
    g_navi.skipMore[0] = 0;
    g_navi.skipMore[1] = 0;
}

/// A weak pointer's object, if it is still that object and alive.
void* ResolveWeak(const void* weak) noexcept
{
    std::int32_t pair[2]{};
    if (!Read(weak, pair) || pair[0] < 0 || pair[1] == 0)
    {
        return nullptr;
    }
    try
    {
        auto* item = RC::Unreal::UObjectArray::IndexToObject(pair[0]);
        if (item == nullptr || item->GetSerialNumber() != pair[1] || item->IsUnreachable() || item->IsPendingKill())
        {
            return nullptr;
        }
        return item->GetUObject();
    }
    catch (...)
    {
        return nullptr;
    }
}

/// The octagon's paint interface (the manager's weak Nav_Map, its Slate object, +0x330): what the pool-0 code sees in rsi.
std::uintptr_t PaintInterfaceOf(void* manager) noexcept
{
    void* octagon = manager != nullptr ? ResolveWeak(At(manager, f::kNaviManagerOctagon)) : nullptr;
    std::uint8_t* slate = nullptr;
    if (octagon == nullptr || !Read(At(octagon, f::kOctagonSlate), slate) || slate == nullptr)
    {
        return 0;
    }
    return reinterpret_cast<std::uintptr_t>(slate + f::kOctagonPaintInterface);
}

/// The manager's roots shown on the overlay: its NaviMap_C and its compass's Compass_C.
void RootsOf(void* manager, void* (&roots)[2]) noexcept
{
    roots[0] = manager != nullptr ? ResolveWeak(At(manager, f::kNaviManagerRoot)) : nullptr;
    void* compass = nullptr;
    roots[1] = manager != nullptr && Read(At(manager, f::kNaviManagerCompass), compass) && compass != nullptr
                   ? ResolveWeak(compass)
                   : nullptr;
}

/// The manager's root is painted: alive and neither collapsed nor hidden (the tick collapses it when its Out ends).
bool RootShown(void* manager) noexcept
{
    void*        root       = manager != nullptr ? ResolveWeak(At(manager, f::kNaviManagerRoot)) : nullptr;
    std::uint8_t visibility = 1;
    return root != nullptr && Read(At(root, f::kWidgetVisibility), visibility) && visibility != 1 && visibility != 2;
}

// --- The seat's tick, as the seat --------------------------------------------------------------------------------------
/// While the seat's player plays: its primary and leader contexts (its camera, its member) and its column's host set.
/// While its map only closes after a leave (no member any more): player 1's, so nothing of a gone player is read. In
/// both: the seat's instance in the slot. Held only with every part in; put back in reverse.
class SeatScope
{
public:
    explicit SeatScope(int seat) noexcept
    {
        const Seat& s = g_seats[seat];
        if (!Read(g.slot29, slot_))
        {
            return;
        }
        // The controller the HUD tick was handed must be the registry's (bound at the join, liveness-checked): a
        // controller the engine replaced without a leave is not lent to the compass either.
        withPlayer_ = s.plays && s.controller != nullptr && s.controller == players::seat_objects::Controller(seat);
        if (withPlayer_)
        {
            contexts_.emplace(seat, players::scopes::Primary | players::scopes::Leader);
            if (!contexts_->Held())
            {
                contexts_.reset();
                return;
            }
            const int column = overlay_pass::ColumnOf(seat);  // the seat's column
            if (column >= 1)
            {
                hostSet_.emplace();
                hostSet_->Lend(column);  // refused: the compass reads player 1's set, as before the band
            }
        }
        platform::WriteGuarded(g.slot29, &s.manager, sizeof(void*));
        g_forSeat = seat;
        held_     = true;
    }
    ~SeatScope()
    {
        if (!held_)
        {
            return;
        }
        platform::WriteGuarded(g.slot29, &slot_, sizeof(void*));
        hostSet_.reset();  // the set that stood before, back
        g_forSeat = 0;
        // contexts_ closes after this body (a member's destructor): the reverse of the opening order.
    }
    SeatScope(const SeatScope&)            = delete;
    SeatScope& operator=(const SeatScope&) = delete;

    bool Held() const noexcept { return held_; }

private:
    void*                                 slot_       = nullptr;
    bool                                  withPlayer_ = false;
    bool                                  held_       = false;
    std::optional<players::scopes::Scope> contexts_;
    std::optional<overlay_layout::HostSetScope> hostSet_;  // the seat's column's host set lent (put back first)
};

// --- Redirected calls ------------------------------------------------------------------------------------------------
void Tick29(void* manager, float deltaTime)
{
    g.tick(manager, deltaTime);
    if (IsSeatManager(manager))
    {
        return;  // a seat's instance reached the game's site: never ticked twice
    }
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        Seat& s = g_seats[seat];
        if (s.manager == nullptr || s.faulted || !(s.shown || RootShown(s.manager)))
        {
            continue;
        }
        SeatScope scope(seat);
        ++(scope.Held() ? s.ticks : s.scopeFailed);
        if (scope.Held() == s.notHeldLogged)
        {
            // A state change, logged both ways: the player's map stops following them, then follows again.
            s.notHeldLogged = !scope.Held();
            if (s.notHeldLogged)
            {
                platform::Log(L"HUD minimap: {}'s minimap is not updated: their camera or character could not be lent to it",
                              SeatName(seat));
            }
            else
            {
                platform::Log(L"HUD minimap: {}'s minimap is updated again", SeatName(seat));
            }
        }
        if (scope.Held() && !platform::CallGuarded(g.tick, s.manager, deltaTime))
        {
            s.faulted = true;
            platform::Log(L"HUD minimap: {}'s navi map tick faulted; its minimap stops", SeatName(seat));
        }
    }
}

void* FirstPc(void* world)
{
    return g_forSeat != 0 && g_seats[g_forSeat].controller != nullptr ? g_seats[g_forSeat].controller : g.firstPc(world);
}

std::uint8_t AddToRoot(void* widget, std::int32_t zOrder)
{
    if (g_forSeat != 0)
    {
        // A seat's root: its octagon renders into a texture of its own (the game's Construct makes one when none is set).
        void* octagon = nullptr;
        if (platform::CallGuarded(g.findOctagon, octagon, widget) && octagon != nullptr)
        {
            void* none = nullptr;
            platform::WriteGuarded(At(octagon, f::kOctagonRenderTexture), &none, sizeof(none));
            platform::Log(L"HUD minimap: {}'s map widget made with a render target of its own", SeatName(g_forSeat));
        }
    }
    return g.addToRoot(widget, zOrder);
}

/// An event the game makes on EndUISystem[29] alone, made on every seat's instance too, for as long as it exists: each
/// always holds the current field's map and icons (shown or not; the pieces are shared, refcounted loads).
template <class... P, class... A>
void OnSeats(U64 (*fn)(void*, P...), A... args) noexcept
{
    if (!g_on)
    {
        return;
    }
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        const Seat& s = g_seats[seat];
        if (s.manager == nullptr || s.faulted)
        {
            continue;
        }
        g_forSeat   = seat;
        U64 ignored = 0;
        platform::CallGuarded(fn, ignored, s.manager, args...);
        g_forSeat = 0;
    }
}

U64 SetupMap(void* m, U64 a2, U64 a3, U64 a4)
{
    const U64 r = g.setupMap(m, a2, a3, a4);
    if (!IsSeatManager(m)) OnSeats(g.setupMap, a2, a3, a4);
    return r;
}
U64 FieldStart(void* m, U64 a2, U64 a3, U64 a4)
{
    const U64 r = g.fieldStart(m, a2, a3, a4);
    if (!IsSeatManager(m)) OnSeats(g.fieldStart, a2, a3, a4);
    return r;
}
U64 FieldEnd(void* m, U64 a2, U64 a3, U64 a4)
{
    const U64 r = g.fieldEnd(m, a2, a3, a4);
    if (!IsSeatManager(m)) OnSeats(g.fieldEnd, a2, a3, a4);
    return r;
}
U64 Reset(void* m, U64 a2, U64 a3, U64 a4)
{
    const U64 r = g.reset(m, a2, a3, a4);
    if (!IsSeatManager(m)) OnSeats(g.reset, a2, a3, a4);
    return r;
}
U64 AddIcon(void* m, U64 key, U64 a3, U64 a4, U64 a5, U64 a6)
{
    const U64 r = g.addIcon(m, key, a3, a4, a5, a6);
    if (!IsSeatManager(m)) OnSeats(g.addIcon, key, a3, a4, a5, a6);
    return r;
}
U64 RemoveIcon(void* m, U64 key)
{
    const U64 r = g.removeIcon(m, key);
    if (!IsSeatManager(m)) OnSeats(g.removeIcon, key);
    return r;
}

/// The navi map's pool-0 acquire (reached from hud/image_code.h's code, made as the game's call): the handover rule
/// (hud/minimap_rules.h) decides, by the pass and whose octagon paints.
void* AcquirePool0(void* renderer, U64 pool)
{
    using Take = minimap_rules::PoolHandover::Take;
    const int pass  = PassIndex(g_column);
    const int owner = OwnerOf(g_navi.seen);
    ++g_counts.acquiresBy[pass][owner == 0 ? 0 : owner >= 1 ? 1 : 2];
    switch (g_pool.OnAcquire(g_column, owner))
    {
    case Take::ResendThenAcquire:
        if (g_heldSlot != nullptr)
        {
            g.submit(g_renderer, 0, g_heldSlot);  // the column holds twice in one frame: the first is sent, never stranded
            ++g_counts.safetySubmits;
        }
        [[fallthrough]];
    case Take::Acquire:
        g_heldSlot = g.acquire(renderer, pool);  // the later passes' octagons add their entries; column 0's submit sends them
        g_renderer = renderer;
        if (g_heldSlot == nullptr)
        {
            g_pool.EndFrame();  // no slot to hold: the later passes acquire from the game, as before
        }
        return g_heldSlot;
    case Take::Held:
        if (g_heldSlot != nullptr)
        {
            ++g_counts.heldGiven[pass];
            return g_heldSlot;
        }
        break;
    case Take::Game:
        break;
    }
    return g.acquire(renderer, pool);
}

void* FindOrAddPool0(void* slot, U64 name, const void* rect)
{
    // A further column's octagon names its entry with its column's number (a distinct FName to the entry search).
    const bool held = g_column >= 1 && slot == g_heldSlot && g_heldSlot != nullptr;
    if (held)
    {
        ++g_counts.renamed;
    }
    return g.findOrAdd(slot, held ? minimap_rules::EntryName(name, g_column) : name, rect);
}

void SubmitPool0(void* renderer, U64 pool, void* slot)
{
    const bool ofHeld = slot == g_heldSlot && g_heldSlot != nullptr;
    if (g_pool.SkipSubmit(g_column, ofHeld))
    {
        ++g_counts.submitsSkipped;
        return;  // held for the later passes
    }
    ++g_counts.submits[PassIndex(g_column)];
    if (ofHeld)
    {
        g_heldSlot = nullptr;
    }
    g.submit(renderer, pool, slot);
}

// --- Creation, mirror, leave -----------------------------------------------------------------------------------------
bool Create(int seat) noexcept
{
    void* memory = nullptr;
    if (!platform::CallGuarded(g.alloc, memory, U64{0x268}, std::uint32_t{0}) || memory == nullptr)
    {
        return false;
    }
    void* made = nullptr;
    if (!platform::CallGuarded(g.ctor, made, memory))
    {
        return false;
    }
    g_seats[seat].manager = memory;
    // The banner fades, bound as EndUISystem's constructor binds the game's instance (0x1413B3367, 0x1413B33AC).
    void* banners = nullptr;
    if (Read(g.slot35, banners) && banners != nullptr)
    {
        alignas(16) std::uint8_t handle[16]{};
        struct Method
        {
            std::uintptr_t function;
            std::uint32_t  adjust;
        };
        alignas(16) Method start{g.bannerStart, 0}, end{g.bannerEnd, 0};
        void* out = nullptr;
        platform::CallGuarded(g.addRaw, out, static_cast<void*>(At(banners, 0x10)), static_cast<void*>(handle), memory,
                              static_cast<const void*>(&start));
        platform::CallGuarded(g.addRaw, out, static_cast<void*>(At(banners, 0x80)), static_cast<void*>(handle), memory,
                              static_cast<const void*>(&end));
    }
    platform::Log(L"HUD minimap: {}'s navi map manager made with the game's constructor ({})", SeatName(seat), memory);
    return true;
}

/// The seat's instance follows player 1's: the map, then the flags through the game's own setters.
void Follow(int seat, void* one) noexcept
{
    void* manager = g_seats[seat].manager;
    void* mapData = nullptr;
    Read(At(one, f::kNaviManagerMapData), mapData);
    void* seatMap = nullptr;
    Read(At(manager, f::kNaviManagerMapData), seatMap);
    g_forSeat = seat;
    if (mapData != nullptr && mapData != seatMap)
    {
        U64 r = 0;
        platform::CallGuarded(g.setupMap, r, manager, reinterpret_cast<U64>(mapData), U64{0}, U64{0});
        platform::LogDebug(L"HUD minimap: {}'s map set up ({})", SeatName(seat), mapData);
    }
    std::uint8_t a[0x10]{}, b[0x10]{};
    if (Read(At(one, f::kNaviManagerFlags), a) && Read(At(manager, f::kNaviManagerFlags), b))
    {
        // +0x180 hidden, +0x181 visible (a toggle), +0x182 enabled, +0x183 auto-rotate; +0x188 zoom.
        if (a[0] != b[0]) platform::CallGuarded(g.setHidden, manager, U64{a[0]});
        if (a[1] != b[1]) platform::CallGuarded(g.toggleVisible, manager, U64{0});
        if (a[2] != b[2]) platform::CallGuarded(g.setEnabled, manager, U64{a[2]}, static_cast<void*>(nullptr));
        platform::WriteGuarded(At(manager, f::kNaviManagerFlags + 3), &a[3], 1);
        platform::WriteGuarded(At(manager, f::kNaviManagerFlags + 8), &a[8], 4);
    }
    std::uint8_t ma[3]{}, mb[3]{};
    if (Read(At(one, f::kNaviManagerModes), ma) && Read(At(manager, f::kNaviManagerModes), mb))
    {
        if (ma[0] != mb[0]) platform::CallGuarded(g.setNaviMode, manager, U64{ma[0]});
        if (ma[1] != mb[1]) platform::CallGuarded(g.setCompassMode, manager, U64{ma[1]});
    }
    g_forSeat = 0;
}

std::wstring NameOf(void* object);  // readings (below)

/// The last pass hide, read back (readings): the root's ColorAndOpacity.A and its SObjectWidget's tint alpha, right after
/// the call and again after the pass's render.
struct HideCheck
{
    void* widget = nullptr;
    int   pass   = 0;
    float afterCall[2]   = {-1.0f, -1.0f};
    float afterRender[2] = {-1.0f, -1.0f};
    bool  called = false;  // the setter returned (no fault)
    void* setter = nullptr;
};
HideCheck g_hideCheck;

void ReadAlphas(void* widget, float (&out)[2]) noexcept
{
    out[0] = out[1] = -1.0f;
    float color[4]{};
    if (Read(At(widget, f::kUserWidgetColor), color))
    {
        out[0] = color[3];
    }
    std::uint8_t* slate = nullptr;  // UWidget::MyGCWidget (TWeakPtr<SObjectWidget>, the object at +0xD8)
    if (Read(At(widget, f::kWidgetGcWidget), slate) && slate != nullptr && Read(slate + f::kSlateColorAndOpacity, color))
    {
        out[1] = color[3];
    }
}

void SetAlpha(void* widget, float alpha) noexcept
{
    LinearColor color;
    if (widget == nullptr || !Read(At(widget, f::kUserWidgetColor), color))
    {
        return;
    }
    g_faded.push_back({widget, color});
    LinearColor clear = color;
    clear.a           = alpha;
    const bool called = platform::CallGuarded(g.setColor, widget, static_cast<const LinearColor*>(&clear));
    if (!called)
    {
        static bool told = false;
        if (!told)
        {
            told = true;
            platform::Log(L"HUD minimap: SetColorAndOpacity faulted on {}; another player's minimap shows in this player's view",
                          NameOf(widget));
        }
    }
    g_hideCheck        = {};
    g_hideCheck.called = called;
    g_hideCheck.setter = reinterpret_cast<void*>(g.setColor);
    g_hideCheck.widget = widget;
    g_hideCheck.pass   = PassIndex(g_column);
    ReadAlphas(widget, g_hideCheck.afterCall);
}

/// The manager's two roots transparent for this pass (restored after it).
void HideRoots(void* manager) noexcept
{
    void* roots[2]{};
    RootsOf(manager, roots);
    SetAlpha(roots[0], 0.0f);
    SetAlpha(roots[1], 0.0f);
}

// --- Readings (`hud.minimap`, research §10's live checks) -------------------------------------------------------------
std::wstring NameOf(void* object)
{
    if (object == nullptr)
    {
        return L"none";
    }
    try
    {
        auto* o = static_cast<RC::Unreal::UObject*>(object);
        return std::format(L"{}:{}", o->GetClassPrivate()->GetName(), o->GetName());
    }
    catch (...)
    {
        return L"(unnamed)";
    }
}

/// One manager: its root, octagon and both texture fields, compass, state bytes and rotation.
std::wstring Describe(void* manager)
{
    if (manager == nullptr)
    {
        return L"none";
    }
    void* roots[2]{};
    RootsOf(manager, roots);
    void*         octagon = ResolveWeak(At(manager, f::kNaviManagerOctagon));
    void*         property = nullptr;  // UEndNaviMapOctagon::RenderTexture
    std::uint8_t* slate    = nullptr;
    void*         target   = nullptr;  // the Slate object's render target (+0x5C8): the one its entries draw into
    if (octagon != nullptr)
    {
        Read(At(octagon, f::kOctagonRenderTexture), property);
        Read(At(octagon, f::kOctagonSlate), slate);
    }
    if (slate != nullptr)
    {
        Read(slate + f::kOctagonSlateTarget, target);
    }
    float        color[4]{};
    std::uint8_t visibility = 0xFF;
    if (roots[0] != nullptr)
    {
        Read(At(roots[0], f::kUserWidgetColor), color);
        Read(At(roots[0], f::kWidgetVisibility), visibility);
    }
    std::uint8_t flags[4]{}, modes[2]{};
    std::int32_t animation = 0, fade = 0;
    float        alpha = 0.0f, rotation[2]{};
    void*        mapData = nullptr;
    float        arrow   = -999.0f;  // Icon_Player's RenderTransform.Angle: the leader's facing on this map
    if (void* icon = ResolveWeak(At(manager, f::kNaviManagerPlayerIcon)); icon != nullptr)
    {
        Read(At(icon, f::kWidgetRenderAngle), arrow);
    }
    Read(At(manager, f::kNaviManagerFlags), flags);
    Read(At(manager, f::kNaviManagerModes), modes);
    Read(At(manager, f::kNaviManagerAnimation), animation);
    Read(At(manager, f::kNaviManagerFade), fade);
    Read(At(manager, f::kNaviManagerAlpha), alpha);
    Read(At(manager, f::kNaviManagerRotation), rotation);
    Read(At(manager, f::kNaviManagerMapData), mapData);
    return std::format(L"{} root {} (a {:.2f} vis {}) compass {} octagon {} property {} target {} paint {:#x} | hidden {} "
                       L"visible {} enabled {} rotate {} anim {} fade {} alpha {:.2f} rot ({:.1f}, {:.1f}) arrow {:.1f} map {} modes {}/{}",
                       manager, NameOf(roots[0]), color[3], visibility, NameOf(roots[1]), NameOf(octagon), NameOf(property),
                       NameOf(target), PaintInterfaceOf(manager), flags[0], flags[1], flags[2], flags[3], animation, fade,
                       alpha, rotation[0], rotation[1], arrow, mapData, modes[0], modes[1]);
}

std::wstring Reading()
{
    std::uint8_t* pm = nullptr;
    std::uint8_t  leader = 0xFF;
    if (Read(g.partyManager, pm) && pm != nullptr)
    {
        Read(pm + f::kPartyLeaderType, leader);
    }
    std::wstring text = std::format(L"{} [{}]", SeatName(0), Describe(One()));
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        Seat& s = g_seats[seat];
        text += std::format(L"; {} [{}] shown {} plays {} member type {} ticked {} (contexts not held {})", SeatName(seat),
                            Describe(s.manager), s.shown, s.plays, players::scopes::MemberType(seat), s.ticks, s.scopeFailed);
        s.ticks = s.scopeFailed = 0;
    }
    const Counts& c     = g_counts;
    const auto    skips = g_navi.skips - c.skipsAtRead;
    text += std::format(
        L" | leader type {} | {} frames: acquires further-pass (player 1's {}, a further player's {}, other {}) player 1's pass "
        L"(player 1's {}, a further player's {}, other {}) single (player 1's {}, a further player's {}, other {}); held given {}; renamed {}; "
        L"submits player 1's pass {} single {} skipped {} safety {}; spans skipped {}",
        leader, c.frames, c.acquiresBy[1][0], c.acquiresBy[1][1], c.acquiresBy[1][2], c.acquiresBy[2][0],
        c.acquiresBy[2][1], c.acquiresBy[2][2], c.acquiresBy[0][0], c.acquiresBy[0][1], c.acquiresBy[0][2],
        c.heldGiven[0] + c.heldGiven[1] + c.heldGiven[2], c.renamed, c.submits[2], c.submits[0], c.submitsSkipped,
        c.safetySubmits, skips);
    text += std::format(L" | last hide {} in pass {}: alpha/tint after the call {:.2f}/{:.2f}, after the render {:.2f}/{:.2f} (setter {} {})",
                        NameOf(g_hideCheck.widget), g_hideCheck.pass, g_hideCheck.afterCall[0], g_hideCheck.afterCall[1],
                        g_hideCheck.afterRender[0], g_hideCheck.afterRender[1], g_hideCheck.setter,
                        g_hideCheck.called ? L"returned" : L"FAULTED");
    g_counts             = {};
    g_counts.skipsAtRead = g_navi.skips;
    return text;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    namespace c = image_code;
    auto at = [&image](std::uintptr_t address, std::size_t length = 1) { return image.Rebase(address, length).value_or(0); };
    g.tick           = reinterpret_cast<TickFn>(at(f::kNaviTick));
    g.setupMap       = reinterpret_cast<MgrFn>(at(f::kNaviSetupMap));
    g.fieldStart     = reinterpret_cast<MgrFn>(at(f::kNaviFieldStart));
    g.fieldEnd       = reinterpret_cast<MgrFn>(at(f::kNaviFieldEnd));
    g.reset          = reinterpret_cast<MgrFn>(at(f::kNaviReset));
    g.addIcon        = reinterpret_cast<AddIconFn>(at(f::kNaviAddIcon));
    g.removeIcon     = reinterpret_cast<RemoveIconFn>(at(f::kNaviRemoveIcon));
    g.setHidden      = reinterpret_cast<FlagFn>(at(f::kNaviSetHidden));
    g.toggleVisible  = reinterpret_cast<FlagFn>(at(f::kNaviToggleVisible));
    g.setEnabled     = reinterpret_cast<EnabledFn>(at(f::kNaviSetEnabled));
    g.setNaviMode    = reinterpret_cast<FlagFn>(at(f::kNaviSetNaviMode));
    g.setCompassMode = reinterpret_cast<FlagFn>(at(f::kNaviSetCompassMode));
    g.firstPc        = reinterpret_cast<FirstPcFn>(at(f::kGetFirstPlayerController));
    g.addToRoot      = reinterpret_cast<AddToRootFn>(at(f::kAddToRootOverlay));
    g.findOctagon    = reinterpret_cast<FindOctagonFn>(at(f::kNaviFindOctagon));
    g.alloc          = reinterpret_cast<AllocFn>(at(f::kGameMalloc));
    g.ctor           = reinterpret_cast<CtorFn>(at(f::kNaviManagerCtor));
    g.addRaw         = reinterpret_cast<AddRawFn>(at(f::kMulticastAddRaw));
    g.acquire        = reinterpret_cast<AcquireFn>(at(c::kAcquireLayer));
    g.findOrAdd      = reinterpret_cast<FindOrAddFn>(at(f::kFindOrAddLayerEntry));
    g.submit         = reinterpret_cast<SubmitFn>(at(f::kSubmitLayerSlot));
    g.setColor       = reinterpret_cast<SetColorFn>(at(f::kSetColorAndOpacity));
    g.bannerStart    = at(f::kNaviBannerStart);
    g.bannerEnd      = at(f::kNaviBannerEnd);
    g.slot29         = reinterpret_cast<void**>(at(f::kNaviSlotGlobal, sizeof(void*)));
    g.slot35         = reinterpret_cast<void**>(at(f::kBannerSlotGlobal, sizeof(void*)));
    g.partyManager   = reinterpret_cast<std::uint8_t**>(at(f::kPartyManagerGlobal, sizeof(void*)));
    const auto site     = at(c::kNaviSite, sizeof(c::kNaviCall));
    const auto back     = at(c::kNaviReturn);
    const auto skipTo   = at(c::kNaviSkipTo);
    if (!g.tick || !g.setupMap || !g.addIcon || !g.removeIcon || !g.firstPc || !g.addToRoot || !g.ctor || !g.acquire ||
        !g.slot29 || !g.slot35 || !g.partyManager || !site || !back || !skipTo ||
        !image.BytesMatch(c::kNaviCheckStart, c::kNaviBytes) || !image.BytesMatch(c::kNaviSubmitStart, c::kNaviSubmitBytes))
    {
        platform::Log(L"HUD minimap: the navi map's code is not what the research read; every player sees player 1's minimap");
        return false;
    }
    // The pool-0 code: the other player's octagon skipped, the acquire through the handover above.
    if (!g_poolCode.Install(site, c::kNaviCall,
                            c::NaviCode(g_navi, back, reinterpret_cast<std::uintptr_t>(&AcquirePool0), skipTo), back))
    {
        platform::Log(L"HUD minimap: the navi map's acquire could not be redirected; players 2 and up get no HUD image of their own");
        return false;
    }
    const auto call = [&at](platform::CallRedirect& redirect, std::uintptr_t siteAddress, std::uintptr_t target,
                            auto* replacement) {
        return redirect.Install(at(siteAddress, 5), at(target), reinterpret_cast<std::uintptr_t>(replacement));
    };
    bool ok = call(g_entryCall, f::kNaviEntryCall, f::kFindOrAddLayerEntry, &FindOrAddPool0) &&
              call(g_submitCall, f::kNaviSubmitCall, f::kSubmitLayerSlot, &SubmitPool0);
    if (!ok)
    {
        g_entryCall.Restore();
        g_poolCode.Restore();
        platform::Log(L"HUD minimap: the navi map's pool-0 calls could not be redirected; players 2 and up get no HUD image of their own");
        return false;
    }
    g_installed = true;
    g_faded.reserve(2 * (kSeatsBuilt + 1));  // two roots per player hidden in a pass, at most
    // The seats' own instances: optional on top of the pool-0 code.
    bool seats = call(g_tickCall, f::kNaviTickCall, f::kNaviTick, &Tick29) &&
                 call(g_firstPcCall, f::kNaviCompassFirstPcCall, f::kGetFirstPlayerController, &FirstPc) &&
                 call(g_textureCall, f::kNaviCreatorAddCall, f::kAddToRootOverlay, &AddToRoot) &&
                 call(g_setupCalls[0], f::kNaviSetupCalls[0], f::kNaviSetupMap, &SetupMap) &&
                 call(g_setupCalls[1], f::kNaviSetupCalls[1], f::kNaviSetupMap, &SetupMap) &&
                 call(g_startCall, f::kNaviFieldStartCall, f::kNaviFieldStart, &FieldStart) &&
                 call(g_endCall, f::kNaviFieldEndCall, f::kNaviFieldEnd, &FieldEnd) &&
                 call(g_resetCall, f::kNaviResetCall, f::kNaviReset, &Reset);
    for (std::size_t i = 0; seats && i < 4; ++i)
    {
        seats = call(g_addCalls[i], f::kNaviAddIconCalls[i], f::kNaviAddIcon, &AddIcon);
    }
    for (std::size_t i = 0; seats && i < 4; ++i)
    {
        seats = call(g_removeCalls[i], f::kNaviRemoveIconCalls[i], f::kNaviRemoveIcon, &RemoveIcon);
    }
    if (!seats)
    {
        for (auto* r : {&g_tickCall, &g_firstPcCall, &g_textureCall, &g_setupCalls[0], &g_setupCalls[1], &g_startCall, &g_endCall,
                        &g_resetCall, &g_addCalls[0], &g_addCalls[1], &g_addCalls[2], &g_addCalls[3], &g_removeCalls[0],
                        &g_removeCalls[1], &g_removeCalls[2], &g_removeCalls[3]})
        {
            r->Restore();
        }
        g_on = false;
        platform::Log(L"HUD minimap: a navi map event call is not what the research read; players 2 and up see no minimap "
                      L"of their own (player 1's renders in player 1's view only)");
    }
    else
    {
        g_removeLastSite    = at(f::kNaviRemoveIconCalls[4], 5);
        g_removeLastPending = true;
    }

    using control::Outcome;
    using control::Register;
    using control::Shape;
    using control::Value;
    Register({.name  = L"hud.minimap",
              .shape = Shape::Text,
              .help  = L"(read only) each player's navi map manager, player 1's first (root, octagon, texture property "
                       L"and target, state, rotation), and what the ticks and the pool-0 code did since the last reading",
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
    Register({.name  = L"hud-minimap",
              .shape = Shape::OnOff,
              .help  = L"each further player's own minimap and compass: a navi map manager of the game's per player (on at start)",
              .set   = [](const Value& value) {
                  SetEnabled(std::get<bool>(value));
                  return Outcome{};
              },
              .get = [] { return Value{Enabled()}; }});
    platform::Log(L"HUD minimap: installed{}", seats ? L" (each further player's own minimap on)" : L" (pool 0 only)");
    return true;
}

void Remove() noexcept
{
    if (!g_installed)
    {
        return;
    }
    g_on = false;
    ClearSkips();
    for (auto* r : {&g_tickCall, &g_firstPcCall, &g_textureCall, &g_setupCalls[0], &g_setupCalls[1], &g_startCall, &g_endCall,
                    &g_resetCall, &g_addCalls[0], &g_addCalls[1], &g_addCalls[2], &g_addCalls[3], &g_removeCalls[0],
                    &g_removeCalls[1], &g_removeCalls[2], &g_removeCalls[3], &g_entryCall, &g_submitCall})
    {
        r->Restore();
    }
    g_poolCode.Restore();
    // The last remove call crosses a block; it stays (it passes through: the mirror stops with g_on).
    // Each seat's map off the screen, as the game's tick does at the end of its Out (the ticks stop with the redirects).
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        void* roots[2]{};
        RootsOf(g_seats[seat].manager, roots);
        for (void* root : roots)
        {
            void** vtable = nullptr;
            void*  fn     = nullptr;
            if (root != nullptr && Read(root, vtable) && vtable != nullptr && Read(At(vtable, f::kWidgetSetVisibility), fn))
            {
                platform::CallGuarded(reinterpret_cast<void (*)(void*, std::uint8_t)>(fn), root, std::uint8_t{1});
            }
        }
        g_seats[seat].shown = false;
    }
}

bool PoolCodeInstalled() noexcept
{
    return g_installed && g_poolCode.IsInstalled();
}

void Tick() noexcept
{
    if (!g_installed || !g_removeLastPending)
    {
        return;
    }
    g_removeLastPending = false;  // outside the function that holds it: the game thread runs it
    if (!g_removeCalls[4].InstallFromOwnThread(g_removeLastSite, reinterpret_cast<std::uintptr_t>(g.removeIcon),
                                               reinterpret_cast<std::uintptr_t>(&RemoveIcon)))
    {
        platform::Log(L"HUD minimap: one icon-remove call could not be redirected; another player's map may keep that icon");
    }
}

void TickSeat(int seat, bool seatPlays, void* seatController) noexcept
try
{
    if (!g_installed || !SeatBuilt(seat))
    {
        return;
    }
    Seat& s      = g_seats[seat];
    s.plays      = seatPlays && g_on;
    s.controller = seatController;
    void* one    = One();
    if (s.plays && one != nullptr)
    {
        void* mapData = nullptr;
        if (s.manager == nullptr && Read(At(one, f::kNaviManagerMapData), mapData) && mapData != nullptr && !Create(seat))
        {
            g_on = false;
            platform::Log(L"HUD minimap: {}'s navi map manager could not be made; players 2 and up see no minimap of their own",
                          SeatName(seat));
            return;
        }
        if (s.manager != nullptr)
        {
            s.shown = true;
            if (!s.faulted)
            {
                Follow(seat, one);
            }
            if (void* root = ResolveWeak(At(s.manager, f::kNaviManagerRoot)); root != s.lastRoot)
            {
                s.lastRoot = root;
                platform::Log(L"HUD minimap: {}'s root changed: {} [{}]; player 1 [{}]", SeatName(seat), SeatName(seat),
                              Describe(s.manager), Describe(one));
            }
        }
    }
    else if (s.shown && s.manager != nullptr)
    {
        // The seat's player left: disabled with the game's setter (its Out animation; the manager's tick collapses the
        // root when it ends). Not `hidden`: the game's field end clears that flag and shows the map again.
        if (!s.faulted)
        {
            g_forSeat = seat;
            platform::CallGuarded(g.setEnabled, s.manager, U64{0}, static_cast<void*>(nullptr));
            g_forSeat = 0;
        }
        s.shown = false;
        platform::Log(L"HUD minimap: {}'s minimap disabled (the game's Out; its root collapses at the end)", SeatName(seat));
    }
}
catch (...)
{
    platform::Log(L"HUD minimap: {}'s tick failed (an exception)", SeatName(seat));
}

void BeginPass(int column) noexcept
{
    g_column = column;
    g_faded.clear();
    ClearSkips();
    if (!g_installed)
    {
        return;
    }
    void* one        = One();
    g_seats[0].paint = PaintInterfaceOf(one);
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        g_seats[seat].paint = g_seats[seat].manager != nullptr ? PaintInterfaceOf(g_seats[seat].manager) : 0;
    }
    if (column >= 1)
    {
        // The column's seat's map alone in its image: player 1's minimap and compass out of it, every other seat's
        // too; each octagon renders in its own player's pass only (every other octagon's span skipped in this one).
        const int own = overlay_pass::SeatOf(column);
        SkipSpan(g_seats[0].paint);
        HideRoots(one);
        for (int seat = 1; seat <= kSeatsBuilt; ++seat)
        {
            if (seat != own && g_seats[seat].manager != nullptr)
            {
                SkipSpan(g_seats[seat].paint);
                HideRoots(g_seats[seat].manager);
            }
        }
    }
    else
    {
        // Player 1's pass, or the single render while a seat's map still shows after a leave: every seat's map out of
        // the image. Painted at alpha 0: the only pass with a real DeltaTime, so its animations (In, Out) advance; no
        // seat's octagon renders here (its span skipped).
        for (int seat = 1; seat <= kSeatsBuilt; ++seat)
        {
            const Seat& s = g_seats[seat];
            if (s.manager != nullptr && (column == 0 || RootShown(s.manager)))
            {
                SkipSpan(s.paint);
                HideRoots(s.manager);
            }
        }
    }
}

void EndPass() noexcept
{
    if (g_hideCheck.widget != nullptr && !g_faded.empty() && g_faded.back().widget == g_hideCheck.widget)
    {
        ReadAlphas(g_hideCheck.widget, g_hideCheck.afterRender);
    }
    for (const auto& faded : g_faded)
    {
        platform::CallGuarded(g.setColor, faded.widget, static_cast<const LinearColor*>(&faded.color));
    }
    g_faded.clear();
}

void EndFrame() noexcept
{
    g_column = overlay_pass::kSingle;
    ClearSkips();
    ++g_counts.frames;
    if (g_pool.EndFrame())
    {
        // Player 1's octagon did not take the slot: submitted on its own, or the next acquire landing on it would spin.
        if (g_heldSlot != nullptr && g_renderer != nullptr)
        {
            g.submit(g_renderer, 0, g_heldSlot);
            ++g_counts.safetySubmits;
            platform::LogDebug(L"HUD minimap: pool 0's slot submitted after player 1's render (player 1's map not painted)");
        }
        g_heldSlot = nullptr;
    }
}

void SetEnabled(bool on) noexcept
{
    g_on = on;
}

bool Enabled() noexcept
{
    return g_on && g_installed;
}

}  // namespace lss::hud::minimap
