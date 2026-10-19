#include "hud/minimap_two.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>

#include "control/registry.h"
#include "engine_facts.h"
#include "hud/image_code.h"
#include "hud/overlay_layout.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/player_two_camera.h"
#include "players/unreal_call.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace lss::hud::minimap_two {

namespace {

using platform::Read;

namespace f = facts;
using U64 = std::uint64_t;

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
    void* (*byType)(std::uint8_t type) = nullptr;  // EndUI_GetCharacterByPlayerType
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
bool                g_on       = true;
bool                g_plays    = false;  // player 2 plays in the split
void*               g_twoPc    = nullptr;
void*               g_two      = nullptr;  // player 2's manager (kept for the process)
bool                g_shown    = false;  // player 2's instance is enabled for player 2 (follows player 1's flags)
bool                g_forTwo   = false;  // a game call is running on player 2's instance
bool                g_faulted  = false;  // player 2's tick faulted: its instance is left alone from then on
std::int32_t        g_twoPcWeak[2] = {-1, 0};  // player 2's controller's weak pair (checked before each swap)
overlay_pass::Pass  g_pass     = overlay_pass::Pass::Single;
void*               g_renderer = nullptr;
void*               g_heldSlot = nullptr;  // pool 0's slot, held from player 2's pass to player 1's

/// What happened since the last `hud.minimap` reading, per pass (0 single, 1 player 2's, 2 player 1's).
struct Counts
{
    std::uint32_t acquiresBy[3][3] = {};  // [pass][octagon: player 1's, player 2's, another]
    std::uint32_t heldGiven[3]     = {};  // the held slot handed to an acquire
    std::uint32_t renamed          = 0;   // entries named with number 2
    std::uint32_t submits[3]       = {};
    std::uint32_t submitsSkipped   = 0;
    std::uint32_t safetySubmits    = 0;
    std::uint32_t ticksTwo         = 0;   // player 2's manager ticked
    std::uint32_t scopeFailed      = 0;   // its scopes could not be entered (no tick then)
    std::uint32_t frames           = 0;
    std::uint64_t skipsAtRead      = 0;   // g_navi.skips at the last reading
};
Counts         g_counts;
std::uintptr_t g_paintOne = 0, g_paintTwo = 0;  // each octagon's paint interface, as BeginPass resolved them

int PassIndex(overlay_pass::Pass pass) noexcept
{
    return pass == overlay_pass::Pass::PlayerTwo ? 1 : pass == overlay_pass::Pass::PlayerOne ? 2 : 0;
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

void* One() noexcept
{
    void* manager = nullptr;
    Read(g.slot29, manager);
    return manager == g_two ? nullptr : manager;  // never player 2's (it is in the slot during its tick)
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

// --- Scopes for player 2's tick ------------------------------------------------------------------------------------
std::uint8_t g_twoType = 0xFF;  // player 2's member type (the PartyManager's player type byte), 0xFF unknown

/// Player 2's member type: the one EndUI_GetCharacterByPlayerType resolves to player 2's pawn (the game's own lookup).
std::uint8_t TypeOfPlayerTwo() noexcept
{
    if (g.byType == nullptr)
    {
        return 0xFF;
    }
    static void*        lastPawn = nullptr;  // looked up again only when player 2's pawn changes
    static std::uint8_t lastType = 0xFF;
    void* pawn = nullptr;
    if (!Read(At(g_twoPc, f::kControllerPawn), pawn) || pawn == nullptr)
    {
        return 0xFF;
    }
    if (pawn == lastPawn)
    {
        return lastType;
    }
    lastPawn = pawn;
    lastType = 0xFF;
    for (std::uint8_t type = 0; type < 8; ++type)
    {
        void* character = nullptr;
        if (platform::CallGuarded(g.byType, character, type) && character == pawn)
        {
            platform::LogDebug(L"HUD minimap: player 2's member is player type {}", type);
            return lastType = type;
        }
    }
    return 0xFF;
}

struct TwoScope
{
    std::uint8_t* pm         = nullptr;
    void*         primary    = nullptr;
    std::uint8_t  leaderType = 0;
    void*         slot       = nullptr;
    bool          entered    = false;
    bool          withPlayer = false;

    void Enter() noexcept
    {
        if (!Read(g.partyManager, pm) || pm == nullptr || !Read(pm + f::kPartyPrimaryController, primary) ||
            !Read(pm + f::kPartyLeaderType, leaderType) || !Read(g.slot29, slot))
        {
            return;
        }
        // While player 2 plays: its controller (camera), member and host set. While its map only closes after a leave
        // (no controller any more): player 1's, so nothing of a gone player is read.
        withPlayer = g_plays && g_twoPc != nullptr && players::player_two_camera::ObjectOf(g_twoPcWeak) == g_twoPc;
        if (withPlayer)
        {
            platform::WriteGuarded(pm + f::kPartyPrimaryController, &g_twoPc, sizeof(void*));
            if (g_twoType != 0xFF)
            {
                platform::WriteGuarded(pm + f::kPartyLeaderType, &g_twoType, 1);
            }
            overlay_layout::WriteHostSet(2);
        }
        platform::WriteGuarded(g.slot29, &g_two, sizeof(void*));
        g_forTwo = entered = true;
    }
    void Leave() noexcept
    {
        if (!entered)
        {
            return;
        }
        if (withPlayer)
        {
            platform::WriteGuarded(pm + f::kPartyPrimaryController, &primary, sizeof(void*));
            platform::WriteGuarded(pm + f::kPartyLeaderType, &leaderType, 1);
            overlay_layout::WriteHostSet(1);
        }
        platform::WriteGuarded(g.slot29, &slot, sizeof(void*));
        g_forTwo = entered = false;
    }
};

// --- Redirected calls ------------------------------------------------------------------------------------------------
void Tick29(void* manager, float deltaTime)
{
    g.tick(manager, deltaTime);
    if (g_two == nullptr || g_faulted || manager == g_two || !(g_shown || RootShown(g_two)))
    {
        return;
    }
    TwoScope scope;
    scope.Enter();
    ++(scope.entered ? g_counts.ticksTwo : g_counts.scopeFailed);
    if (scope.entered && !platform::CallGuarded(g.tick, g_two, deltaTime))
    {
        g_faulted = true;
        platform::Log(L"HUD minimap: player 2's navi map tick faulted; player 2's minimap stops");
    }
    scope.Leave();
}

void* FirstPc(void* world)
{
    return g_forTwo && g_twoPc != nullptr ? g_twoPc : g.firstPc(world);
}

std::uint8_t AddToRoot(void* widget, std::int32_t zOrder)
{
    if (g_forTwo)
    {
        // Player 2's root: its octagon renders into a texture of its own (the game's Construct makes one when none is set).
        void* octagon = nullptr;
        if (platform::CallGuarded(g.findOctagon, octagon, widget) && octagon != nullptr)
        {
            void* none = nullptr;
            platform::WriteGuarded(At(octagon, f::kOctagonRenderTexture), &none, sizeof(none));
            platform::Log(L"HUD minimap: player 2's map widget made with a render target of its own");
        }
    }
    return g.addToRoot(widget, zOrder);
}

/// An event the game makes on EndUISystem[29] alone, made on player 2's instance too, for as long as it exists: it always
/// holds the current field's map and icons (shown or not; the pieces are shared, refcounted loads).
template <class... P, class... A>
void OnTwo(U64 (*fn)(void*, P...), A... args) noexcept
{
    if (g_two == nullptr || g_faulted || !g_on)
    {
        return;
    }
    g_forTwo = true;
    U64 ignored = 0;
    platform::CallGuarded(fn, ignored, g_two, args...);
    g_forTwo = false;
}

U64 SetupMap(void* m, U64 a2, U64 a3, U64 a4)
{
    const U64 r = g.setupMap(m, a2, a3, a4);
    if (m != g_two) OnTwo(g.setupMap, a2, a3, a4);
    return r;
}
U64 FieldStart(void* m, U64 a2, U64 a3, U64 a4)
{
    const U64 r = g.fieldStart(m, a2, a3, a4);
    if (m != g_two) OnTwo(g.fieldStart, a2, a3, a4);
    return r;
}
U64 FieldEnd(void* m, U64 a2, U64 a3, U64 a4)
{
    const U64 r = g.fieldEnd(m, a2, a3, a4);
    if (m != g_two) OnTwo(g.fieldEnd, a2, a3, a4);
    return r;
}
U64 Reset(void* m, U64 a2, U64 a3, U64 a4)
{
    const U64 r = g.reset(m, a2, a3, a4);
    if (m != g_two) OnTwo(g.reset, a2, a3, a4);
    return r;
}
U64 AddIcon(void* m, U64 key, U64 a3, U64 a4, U64 a5, U64 a6)
{
    const U64 r = g.addIcon(m, key, a3, a4, a5, a6);
    if (m != g_two) OnTwo(g.addIcon, key, a3, a4, a5, a6);
    return r;
}
U64 RemoveIcon(void* m, U64 key)
{
    const U64 r = g.removeIcon(m, key);
    if (m != g_two) OnTwo(g.removeIcon, key);
    return r;
}

/// The navi map's pool-0 acquire (reached from hud/image_code.h's code, made as the game's call).
void* AcquirePool0(void* renderer, U64 pool)
{
    const int pass = PassIndex(g_pass);
    ++g_counts.acquiresBy[pass][g_navi.seen != 0 && g_navi.seen == g_paintOne   ? 0
                                : g_navi.seen != 0 && g_navi.seen == g_paintTwo ? 1
                                                                                : 2];
    // Only player 2's octagon's slot is held, and only player 1's octagon is given it: any other painter reaching the
    // site (another map widget) gets the game's own acquire and submit, so no slot can be stranded.
    if (g_pass == overlay_pass::Pass::PlayerTwo && g_navi.seen != 0 && g_navi.seen == g_paintTwo)
    {
        if (g_heldSlot != nullptr)
        {
            g.submit(g_renderer, 0, g_heldSlot);  // a second hold in one frame: the first is sent, never stranded
            g_heldSlot = nullptr;
            ++g_counts.safetySubmits;
        }
        void* slot = g.acquire(renderer, pool);
        g_renderer = renderer;
        g_heldSlot = slot;  // player 1's octagon adds its entry to it; player 1's submit sends both
        return slot;
    }
    if (g_pass == overlay_pass::Pass::PlayerOne && g_heldSlot != nullptr && g_navi.seen != 0 && g_navi.seen == g_paintOne)
    {
        ++g_counts.heldGiven[pass];
        return g_heldSlot;
    }
    return g.acquire(renderer, pool);
}

void* FindOrAddPool0(void* slot, U64 name, const void* rect)
{
    // Player 2's octagon names its entry with number 2 (a distinct FName to the entry search; no registration).
    const bool two = g_pass == overlay_pass::Pass::PlayerTwo && slot == g_heldSlot && g_heldSlot != nullptr;
    if (two)
    {
        ++g_counts.renamed;
    }
    return g.findOrAdd(slot, two ? name + (2ull << 32) : name, rect);
}

void SubmitPool0(void* renderer, U64 pool, void* slot)
{
    if (g_pass == overlay_pass::Pass::PlayerTwo && slot == g_heldSlot && g_heldSlot != nullptr)
    {
        ++g_counts.submitsSkipped;
        return;  // held for player 1's pass
    }
    ++g_counts.submits[PassIndex(g_pass)];
    if (slot == g_heldSlot)
    {
        g_heldSlot = nullptr;
    }
    g.submit(renderer, pool, slot);
}

// --- Creation, mirror, leave -----------------------------------------------------------------------------------------
bool Create() noexcept
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
    g_two = memory;
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
        platform::CallGuarded(g.addRaw, out, static_cast<void*>(At(banners, 0x10)), static_cast<void*>(handle), g_two,
                              static_cast<const void*>(&start));
        platform::CallGuarded(g.addRaw, out, static_cast<void*>(At(banners, 0x80)), static_cast<void*>(handle), g_two,
                              static_cast<const void*>(&end));
    }
    platform::Log(L"HUD minimap: player 2's navi map manager made with the game's constructor ({})", g_two);
    return true;
}

/// Player 2's instance follows player 1's: the map, then the flags through the game's own setters.
void Follow(void* one) noexcept
{
    void* mapData = nullptr;
    Read(At(one, f::kNaviManagerMapData), mapData);
    void* twoMap = nullptr;
    Read(At(g_two, f::kNaviManagerMapData), twoMap);
    g_forTwo = true;
    if (mapData != nullptr && mapData != twoMap)
    {
        U64 r = 0;
        platform::CallGuarded(g.setupMap, r, g_two, reinterpret_cast<U64>(mapData), U64{0}, U64{0});
        platform::LogDebug(L"HUD minimap: player 2's map set up ({})", mapData);
    }
    std::uint8_t a[0x10]{}, b[0x10]{};
    if (Read(At(one, f::kNaviManagerFlags), a) && Read(At(g_two, f::kNaviManagerFlags), b))
    {
        // +0x180 hidden, +0x181 visible (a toggle), +0x182 enabled, +0x183 auto-rotate; +0x188 zoom.
        if (a[0] != b[0]) platform::CallGuarded(g.setHidden, g_two, U64{a[0]});
        if (a[1] != b[1]) platform::CallGuarded(g.toggleVisible, g_two, U64{0});
        if (a[2] != b[2]) platform::CallGuarded(g.setEnabled, g_two, U64{a[2]}, static_cast<void*>(nullptr));
        platform::WriteGuarded(At(g_two, f::kNaviManagerFlags + 3), &a[3], 1);
        platform::WriteGuarded(At(g_two, f::kNaviManagerFlags + 8), &a[8], 4);
    }
    std::uint8_t ma[3]{}, mb[3]{};
    if (Read(At(one, f::kNaviManagerModes), ma) && Read(At(g_two, f::kNaviManagerModes), mb))
    {
        if (ma[0] != mb[0]) platform::CallGuarded(g.setNaviMode, g_two, U64{ma[0]});
        if (ma[1] != mb[1]) platform::CallGuarded(g.setCompassMode, g_two, U64{ma[1]});
    }
    g_forTwo = false;
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
            platform::Log(L"HUD minimap: SetColorAndOpacity faulted on {}; the other player's minimap shows in this half",
                          NameOf(widget));
        }
    }
    g_hideCheck        = {};
    g_hideCheck.called = called;
    g_hideCheck.setter = reinterpret_cast<void*>(g.setColor);
    g_hideCheck.widget = widget;
    g_hideCheck.pass   = PassIndex(g_pass);
    ReadAlphas(widget, g_hideCheck.afterCall);
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
    if (void* icon = ResolveWeak(At(manager, 0x20)); icon != nullptr)
    {
        Read(At(icon, 0xA8), arrow);
    }
    Read(At(manager, f::kNaviManagerFlags), flags);
    Read(At(manager, f::kNaviManagerModes), modes);
    Read(At(manager, 0x18C), animation);
    Read(At(manager, 0x198), fade);
    Read(At(manager, 0x1A0), alpha);
    Read(At(manager, 0x1A4), rotation);
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
    void*         primary = nullptr;
    std::uint8_t  leader = 0xFF;
    if (Read(g.partyManager, pm) && pm != nullptr)
    {
        Read(pm + f::kPartyPrimaryController, primary);
        Read(pm + f::kPartyLeaderType, leader);
    }
    const Counts& c     = g_counts;
    const auto    skips = g_navi.skips - c.skipsAtRead;
    std::wstring  text  = std::format(
        L"one [{}] two [{}] shown {} plays {} | leader type {} two's type {} | {} frames: two ticked {} (scope failed {}); "
        L"acquires p2-pass (one {}, two {}, other {}) p1-pass (one {}, two {}, other {}) single (one {}, two {}, other {}); "
        L"held given {}; renamed {}; submits p1-pass {} single {} skipped {} safety {}; spans skipped {}",
        Describe(One()), Describe(g_two), g_shown, g_plays, leader, g_twoType, c.frames, c.ticksTwo, c.scopeFailed,
        c.acquiresBy[1][0], c.acquiresBy[1][1], c.acquiresBy[1][2], c.acquiresBy[2][0], c.acquiresBy[2][1],
        c.acquiresBy[2][2], c.acquiresBy[0][0], c.acquiresBy[0][1], c.acquiresBy[0][2],
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
    g.byType         = reinterpret_cast<void* (*)(std::uint8_t)>(at(f::kCharacterByPlayerType));
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
        platform::Log(L"HUD minimap: the navi map's code is not what the research read; player 2 sees player 1's minimap");
        return false;
    }
    // The pool-0 code: the other player's octagon skipped, the acquire through the handover above.
    if (!g_poolCode.Install(site, c::kNaviCall,
                            c::NaviCode(g_navi, back, reinterpret_cast<std::uintptr_t>(&AcquirePool0), skipTo), back))
    {
        platform::Log(L"HUD minimap: the navi map's acquire could not be redirected; player 2's image stays off");
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
        platform::Log(L"HUD minimap: the navi map's pool-0 calls could not be redirected; player 2's image stays off");
        return false;
    }
    g_installed = true;
    // Player 2's own instance: optional on top of the pool-0 code.
    bool two = call(g_tickCall, f::kNaviTickCall, f::kNaviTick, &Tick29) &&
               call(g_firstPcCall, f::kNaviCompassFirstPcCall, f::kGetFirstPlayerController, &FirstPc) &&
               call(g_textureCall, f::kNaviCreatorAddCall, f::kAddToRootOverlay, &AddToRoot) &&
               call(g_setupCalls[0], f::kNaviSetupCalls[0], f::kNaviSetupMap, &SetupMap) &&
               call(g_setupCalls[1], f::kNaviSetupCalls[1], f::kNaviSetupMap, &SetupMap) &&
               call(g_startCall, f::kNaviFieldStartCall, f::kNaviFieldStart, &FieldStart) &&
               call(g_endCall, f::kNaviFieldEndCall, f::kNaviFieldEnd, &FieldEnd) &&
               call(g_resetCall, f::kNaviResetCall, f::kNaviReset, &Reset);
    for (std::size_t i = 0; two && i < 4; ++i)
    {
        two = call(g_addCalls[i], f::kNaviAddIconCalls[i], f::kNaviAddIcon, &AddIcon);
    }
    for (std::size_t i = 0; two && i < 4; ++i)
    {
        two = call(g_removeCalls[i], f::kNaviRemoveIconCalls[i], f::kNaviRemoveIcon, &RemoveIcon);
    }
    if (!two)
    {
        for (auto* r : {&g_tickCall, &g_firstPcCall, &g_textureCall, &g_setupCalls[0], &g_setupCalls[1], &g_startCall, &g_endCall,
                        &g_resetCall, &g_addCalls[0], &g_addCalls[1], &g_addCalls[2], &g_addCalls[3], &g_removeCalls[0],
                        &g_removeCalls[1], &g_removeCalls[2], &g_removeCalls[3]})
        {
            r->Restore();
        }
        g_on = false;
        platform::Log(L"HUD minimap: a navi map event call is not what the research read; player 2's half shows no "
                      L"minimap (player 1's renders in player 1's pass only)");
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
              .help  = L"(read only) both navi map managers (root, octagon, texture property and target, state, rotation) "
                       L"and what the tick and pool-0 code did since the last reading",
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
    Register({.name  = L"hud-minimap-two",
              .shape = Shape::OnOff,
              .help  = L"player 2's own minimap and compass: a second navi map manager of the game's (on at start)",
              .set   = [](const Value& value) {
                  SetEnabled(std::get<bool>(value));
                  return Outcome{};
              },
              .get = [] { return Value{Enabled()}; }});
    platform::Log(L"HUD minimap: installed{}", two ? L" (player 2's own minimap on)" : L" (pool 0 only)");
    return true;
}

void Remove() noexcept
{
    if (!g_installed)
    {
        return;
    }
    g_on        = false;
    g_navi.skip = 0;
    for (auto* r : {&g_tickCall, &g_firstPcCall, &g_textureCall, &g_setupCalls[0], &g_setupCalls[1], &g_startCall, &g_endCall,
                    &g_resetCall, &g_addCalls[0], &g_addCalls[1], &g_addCalls[2], &g_addCalls[3], &g_removeCalls[0],
                    &g_removeCalls[1], &g_removeCalls[2], &g_removeCalls[3], &g_entryCall, &g_submitCall})
    {
        r->Restore();
    }
    g_poolCode.Restore();
    // The last remove call crosses a block; it stays (it passes through: the mirror stops with g_on).
    // Player 2's map off the screen, as the game's tick does at the end of its Out (its tick stops with the redirects).
    void* roots[2]{};
    RootsOf(g_two, roots);
    for (void* root : roots)
    {
        void** vtable = nullptr;
        void*  fn     = nullptr;
        if (root != nullptr && Read(root, vtable) && vtable != nullptr && Read(At(vtable, f::kWidgetSetVisibility), fn))
        {
            platform::CallGuarded(reinterpret_cast<void (*)(void*, std::uint8_t)>(fn), root, std::uint8_t{1});
        }
    }
    g_shown = false;
}

bool PoolCodeInstalled() noexcept
{
    return g_installed && g_poolCode.IsInstalled();
}

void Tick(bool playerTwoPlays, void* playerTwoController) noexcept
try
{
    if (!g_installed)
    {
        return;
    }
    if (g_removeLastPending)
    {
        g_removeLastPending = false;  // outside the function that holds it: the game thread runs it
        if (!g_removeCalls[4].InstallFromOwnThread(g_removeLastSite, reinterpret_cast<std::uintptr_t>(g.removeIcon),
                                                   reinterpret_cast<std::uintptr_t>(&RemoveIcon)))
        {
            platform::Log(L"HUD minimap: one icon-remove call could not be redirected; player 2's map may keep that icon");
        }
    }
    g_plays = playerTwoPlays && g_on;
    if (playerTwoController != g_twoPc)
    {
        g_twoPcWeak[0] = -1;
        g_twoPcWeak[1] = 0;
        if (playerTwoController != nullptr)
        {
            players::player_two_camera::WeakPairOf(playerTwoController, g_twoPcWeak);
        }
    }
    g_twoPc = playerTwoController;
    void* one = One();
    if (g_plays && one != nullptr)
    {
        void* mapData = nullptr;
        if (g_two == nullptr && Read(At(one, f::kNaviManagerMapData), mapData) && mapData != nullptr && !Create())
        {
            g_on = false;
            platform::Log(L"HUD minimap: player 2's navi map manager could not be made; player 2's half shows no minimap");
            return;
        }
        if (g_two != nullptr)
        {
            g_twoType = TypeOfPlayerTwo();
            g_shown   = true;
            if (!g_faulted)
            {
                Follow(one);
            }
            static void* lastRoot = nullptr;
            if (void* root = ResolveWeak(At(g_two, f::kNaviManagerRoot)); root != lastRoot)
            {
                lastRoot = root;
                platform::Log(L"HUD minimap: player 2's root changed: two [{}]; one [{}]", Describe(g_two), Describe(one));
            }
        }
    }
    else if (g_shown && g_two != nullptr)
    {
        // Player 2 left: disabled with the game's setter (its Out animation; the manager's tick collapses the root when it
        // ends). Not `hidden`: the game's field end clears that flag and shows the map again.
        if (!g_faulted)
        {
            g_forTwo = true;
            platform::CallGuarded(g.setEnabled, g_two, U64{0}, static_cast<void*>(nullptr));
            g_forTwo = false;
        }
        g_shown  = false;
        platform::Log(L"HUD minimap: player 2's minimap disabled (the game's Out; its root collapses at the end)");
    }
}
catch (...)
{
    platform::Log(L"HUD minimap: tick failed (an exception)");
}

void BeginPass(overlay_pass::Pass pass) noexcept
{
    g_pass = pass;
    g_faded.clear();
    g_navi.skip = 0;
    if (!g_installed)
    {
        return;
    }
    void* one = One();
    g_paintOne = PaintInterfaceOf(one);
    g_paintTwo = g_two != nullptr ? PaintInterfaceOf(g_two) : 0;
    void* roots[2]{};
    if (pass == overlay_pass::Pass::PlayerTwo)
    {
        // Player 1's minimap and compass out of player 2's image; its octagon renders in player 1's pass only.
        g_navi.skip = g_paintOne;
        RootsOf(one, roots);
        SetAlpha(roots[0], 0.0f);
        SetAlpha(roots[1], 0.0f);
    }
    else if (g_two != nullptr && (pass == overlay_pass::Pass::PlayerOne || RootShown(g_two)))
    {
        // Player 2's out of player 1's image and, while it still shows after a leave, out of the single image. Painted at
        // alpha 0: the only pass with a real DeltaTime, so its animations (In, Out) advance; its octagon does not render.
        g_navi.skip = g_paintTwo;
        RootsOf(g_two, roots);
        SetAlpha(roots[0], 0.0f);
        SetAlpha(roots[1], 0.0f);
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
    g_pass      = overlay_pass::Pass::Single;
    g_navi.skip = 0;
    ++g_counts.frames;
    if (g_heldSlot != nullptr && g_renderer != nullptr)
    {
        // Player 1's octagon did not take the slot: submitted on its own, or the next acquire landing on it would spin.
        g.submit(g_renderer, 0, g_heldSlot);
        g_heldSlot = nullptr;
        ++g_counts.safetySubmits;
        platform::LogDebug(L"HUD minimap: pool 0's slot submitted after player 1's render (player 1's map not painted)");
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

}  // namespace lss::hud::minimap_two
