#include "hud/field_icons.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/player_two_camera.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace lss::hud::field_icons {

namespace {

using platform::Read;

struct Vector3
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};
struct Vector2
{
    float x = 0.0f, y = 0.0f;
};
/// A canvas slot's Offset attribute value: {x, y, width, height}.
using Offset = std::array<float, 4>;
static_assert(sizeof(Offset) == 16);

using PlaceFn    = void (*)(void* entry, void* canvas, float dt, void* controller, const Vector3* leader, std::int32_t unused,
                         const Vector3* icon);
using ProjectFn  = char (*)(void* controller, const Vector3* world, Vector2* out);
using ChildrenFn = void* (*)(void* canvas);
using CountFn    = std::int32_t (*)(void* children);

platform::EntryRedirect g_place;
std::uintptr_t          g_placeAt = 0;
ProjectFn               g_project = nullptr;
void**                  g_slot    = nullptr;  // &EndUISystem[51]
bool                    g_installed = false, g_pending = false;
bool                    g_on = true;

// All of the state below is the game thread's: the placement is recorded in the HUD tick, replayed in the overlay
// driver and forgotten at the frame's end, all on that thread (the driver guards its frame-end call by the thread).
void* g_member    = nullptr;  // player 2's character, set by Tick
bool  g_replaying = false;

/// One placement the tick made (the location pointers are the tick's stack temporaries: copied).
struct Call
{
    void*        entry      = nullptr;
    void*        canvas     = nullptr;
    void*        controller = nullptr;
    float        dt         = 0.0f;
    std::int32_t weak[2]    = {-1, 0};  // the entry's weak widget pair (entry+0): checked before a replay
    Vector3      leader;
    Vector3      icon;
};
/// What one view keeps for an icon between frames: its own alpha and the last offset its projection wrote.
struct View
{
    float  alpha  = 0.0f;  // a new icon starts at 0, as the game starts each entry
    Offset offset = {};
    bool   placed = false;
};
struct Known
{
    void*         entry   = nullptr;
    std::int32_t  weak[2] = {-1, 0};
    View          two, one;  // player 1's alpha is the entry's own; only its last offset is kept
    std::uint32_t seen = 0;  // the frame it was last recorded
};

std::vector<Call>        g_calls;
std::vector<Known>       g_known;
std::vector<const void*> g_listed;  // the icon map's live entries, walked once per replay
std::uint32_t            g_frame = 0;

/// What the readings show (`hud.field-icons`): counts since the last reading and one icon followed through the frame.
struct Seen
{
    std::uint32_t frames = 0, framesWithIcons = 0, recorded = 0;
    std::uint32_t replays[2] = {}, faults = 0;
    std::uint32_t canvasChanged = 0, entryGone = 0, widgetGone = 0, noSlot = 0;
    bool          memberKnown = false;
    void*         icon      = nullptr;
    float         tickAt[2] = {}, twoAt[2] = {}, oneAt[2] = {};
    bool          twoProjected = false, oneProjected = false;
    float         alphaTick = 0.0f, alphaTwo = 0.0f;
};
Seen          g_seen;
std::uint32_t g_run = 0, g_runRecorded = 0;  // consecutive frames with icons (an Info line when a run starts and ends)
std::uint32_t g_replayed[2] = {};
bool          g_firstReplayLogged = false;

void Fault(int kind, const wchar_t* what) noexcept
{
    static std::uint32_t told = 0;
    ++g_seen.faults;
    if ((told & (1u << kind)) == 0)
    {
        told |= 1u << kind;
        platform::Log(L"field icons: replaying {} faulted (the first; `hud.field-icons` counts them); that icon stays as the "
                      L"game placed it",
                      what);
    }
}

/// Last call per entry wins.
void Keep(const Call& call) noexcept
{
    for (auto& known : g_calls)
    {
        if (known.entry == call.entry)
        {
            known = call;
            return;
        }
    }
    try
    {
        g_calls.push_back(call);
    }
    catch (...)
    {
        // Out of memory: this icon is not replayed this frame.
    }
}

void Place(void* entry, void* canvas, float dt, void* controller, const Vector3* leader, std::int32_t unused, const Vector3* icon)
{
    if (!g_replaying && g_on && entry != nullptr && canvas != nullptr)
    {
        Call call{entry, canvas, controller, dt};
        if (platform::CopyGuarded(&call.leader, leader, sizeof(call.leader)) &&
            platform::CopyGuarded(&call.icon, icon, sizeof(call.icon)) && platform::CopyGuarded(call.weak, entry, sizeof(call.weak)))
        {
            Keep(call);
            ++g_seen.recorded;
        }
    }
    reinterpret_cast<PlaceFn>(g_place.Original())(entry, canvas, dt, controller, leader, unused, icon);
}

std::uint8_t* Manager() noexcept
{
    void* manager = nullptr;
    return g_slot != nullptr && Read(g_slot, manager) ? static_cast<std::uint8_t*>(manager) : nullptr;
}

/// The entries that are values in the manager's icon map now (an entry freed by the tick's removal would otherwise be
/// written): the map's sparse array walked by its allocation bits, as the tick walks it.
void ListEntries(const std::uint8_t* manager) noexcept
{
    g_listed.clear();
    const std::uint8_t*  data    = nullptr;
    const std::uint32_t* heap    = nullptr;
    std::int32_t         numBits = 0;
    if (!Read(manager + facts::kFieldIconMapData, data) || !Read(manager + facts::kFieldIconMapBitsHeap, heap) ||
        !Read(manager + facts::kFieldIconMapNumBits, numBits) || data == nullptr || numBits <= 0 || numBits > 4096)
    {
        return;
    }
    const std::uint32_t* bits =
        heap != nullptr ? heap : reinterpret_cast<const std::uint32_t*>(manager + facts::kFieldIconMapBitsInline);
    std::uint32_t word = 0;
    for (std::int32_t i = 0; i < numBits; ++i)
    {
        const void* value = nullptr;
        if (i % 32 == 0 && !Read(bits + i / 32, word))
        {
            return;
        }
        if ((word & (1u << (i % 32))) != 0 &&
            Read(data + static_cast<std::size_t>(i) * facts::kFieldIconMapElement + facts::kFieldIconMapElementEntry, value) &&
            value != nullptr)
        {
            try
            {
                g_listed.push_back(value);
            }
            catch (...)
            {
                return;
            }
        }
    }
}

bool Listed(const void* entry) noexcept
{
    for (const void* listed : g_listed)
    {
        if (listed == entry)
        {
            return true;
        }
    }
    return false;
}

/// The canvas slot holding `widget`, matched on the widget's Slate object as the placement matches it (the game's
/// GetCachedWidget: the GC widget at +0xD8, else the plain one at +0xC8; IDA 2026-10-22), or nullptr.
std::uint8_t* SlotOf(void* canvas, void* widget) noexcept
{
    void*      slate      = nullptr;
    void*      plain      = nullptr;
    void**     vtable     = nullptr;
    ChildrenFn childrenFn = nullptr;
    void*      children   = nullptr;
    const auto w          = static_cast<const std::uint8_t*>(widget);
    if (!Read(w + facts::kWidgetGcWidget, slate) || !Read(w + facts::kWidgetGcWidget - 0x10, plain) ||
        (slate == nullptr && plain == nullptr) || !Read(canvas, vtable) || vtable == nullptr ||
        !Read(reinterpret_cast<const std::uint8_t*>(vtable) + facts::kCanvasChildren, childrenFn) || childrenFn == nullptr ||
        !platform::CallGuarded(childrenFn, children, canvas) || children == nullptr)
    {
        return nullptr;
    }
    void**       childrenVtable = nullptr;
    CountFn      countFn        = nullptr;
    std::int32_t count          = 0;
    void**       slots          = nullptr;
    if (!Read(children, childrenVtable) || childrenVtable == nullptr || !Read(childrenVtable, countFn) || countFn == nullptr ||
        !platform::CallGuarded(countFn, count, children) || count <= 0 || count > 4096 ||
        !Read(static_cast<const std::uint8_t*>(children) + 8, slots) || slots == nullptr)
    {
        return nullptr;
    }
    for (std::int32_t i = 0; i < count; ++i)
    {
        std::uint8_t* slot   = nullptr;
        void*         holder = nullptr;
        if (Read(slots + i, slot) && slot != nullptr && Read(slot + facts::kCanvasSlotWidget, holder) && holder != nullptr &&
            (holder == slate || holder == plain))
        {
            return slot;
        }
    }
    return nullptr;
}

bool MemberLocation(Vector3& out) noexcept
{
    void* root = platform::ReadPointer(g_member, facts::kActorRootComponent);
    return root != nullptr && Read(static_cast<const std::uint8_t*>(root) + facts::kComponentWorldLocation, out);
}

Known* KnownOf(const Call& call) noexcept
{
    for (auto& known : g_known)
    {
        if (known.entry == call.entry)
        {
            if (known.weak[0] != call.weak[0] || known.weak[1] != call.weak[1])
            {
                known = Known{};  // the entry's memory reused for another icon: it starts over
                known.entry = call.entry;
                std::memcpy(known.weak, call.weak, sizeof(known.weak));
            }
            known.seen = g_frame;
            return &known;
        }
    }
    Known made;
    made.entry = call.entry;
    std::memcpy(made.weak, call.weak, sizeof(made.weak));
    made.seen = g_frame;
    try
    {
        g_known.push_back(made);
    }
    catch (...)
    {
        return nullptr;
    }
    return &g_known.back();
}

/// One icon placed again for a view: for player 2 with player 2's alpha swapped into the entry and the tick's written
/// back around the call; the slot's offset then read back (this view's projection wrote it) or this view's last one
/// written (it did not). Returns false when the icon stays as the game placed it.
bool PlaceFor(int player, const Call& call, void* widget, void* controller, const Vector3* leader, float dt, View& view,
              bool& projected, std::uint8_t*& slot) noexcept
{
    auto*   entry = static_cast<std::uint8_t*>(call.entry);
    Vector2 point;
    char    projects = 0;
    if (!platform::CallGuarded(g_project, projects, controller, static_cast<const Vector3*>(&call.icon), &point))
    {
        Fault(0, L"an icon's projection");
        return false;
    }
    projected = projects != 0;
    float alphaTick = 0.0f;
    if (!Read(entry + facts::kFieldIconEntryAlpha, alphaTick))
    {
        Fault(1, L"an icon's alpha");
        return false;
    }
    if (player == 2 && !platform::WriteGuarded(entry + facts::kFieldIconEntryAlpha, &view.alpha, sizeof(view.alpha)))
    {
        Fault(2, L"an icon's alpha for player 2");
        return false;
    }
    const bool placed = platform::CallGuarded(reinterpret_cast<PlaceFn>(g_place.Original()), call.entry, call.canvas, dt,
                                              controller, leader, std::int32_t{0}, static_cast<const Vector3*>(&call.icon));
    if (player == 2)
    {
        Read(entry + facts::kFieldIconEntryAlpha, view.alpha);  // player 2's step taken
        platform::WriteGuarded(entry + facts::kFieldIconEntryAlpha, &alphaTick, sizeof(alphaTick));
    }
    if (!placed)
    {
        Fault(3, L"an icon");
        return false;
    }
    // The placement writes the slot only for an entry with a slot (entry+28) whose point projects; the view's offset is
    // kept from those writes and put back on the frames the point is behind this view's camera.
    std::uint8_t hasSlot = 0;
    slot                 = SlotOf(call.canvas, widget);
    if (slot == nullptr)
    {
        ++g_seen.noSlot;
    }
    else if (Read(entry + facts::kFieldIconEntryHasSlot, hasSlot) && hasSlot != 0)
    {
        if (projected)
        {
            view.placed = Read(slot + facts::kCanvasSlotOffset, view.offset);
        }
        else if (view.placed)
        {
            platform::WriteGuarded(slot + facts::kCanvasSlotOffset, view.offset.data(), sizeof(view.offset));
        }
    }
    return true;
}

/// The first recorded icon followed through the frame for the reading: the tick's offset (read before player 2's
/// placement, which overwrites it), then each view's offset after its placement.
void Sample(int player, const Call& call, const std::uint8_t* slot, bool projected, const View& view, float alphaTick) noexcept
{
    g_seen.icon = call.entry;
    if (player == 2)
    {
        g_seen.alphaTick    = alphaTick;
        g_seen.alphaTwo     = view.alpha;
        g_seen.twoProjected = projected;
    }
    else
    {
        g_seen.oneProjected = projected;
    }
    if (slot != nullptr)
    {
        float* into = player == 2 ? g_seen.twoAt : g_seen.oneAt;
        platform::CopyGuarded(into, slot + facts::kCanvasSlotOffset, sizeof(g_seen.twoAt));
    }
}

}  // namespace

std::wstring Reading();  // hud.field-icons (below; outside the anonymous namespace for the registry's lambda)

bool Install(const platform::GameImage& image) noexcept
{
    const auto place   = image.Rebase(facts::kFieldIconPlace, 16);
    const auto project = image.Rebase(facts::kOverlayProject, 16);
    const auto slots   = image.Rebase(facts::kEndUISystemSlots, 64 * sizeof(void*));
    if (!place || !project || !slots || !image.BytesMatch(facts::kFieldIconPlace, facts::kFieldIconPlaceFirst) ||
        !image.BytesMatch(facts::kOverlayProject, facts::kOverlayProjectFirst))
    {
        platform::Log(L"field icons: the icon placement or the overlay projection is not what the research read; the field "
                      L"icons stay placed through player 1's camera");
        return false;
    }
    g_placeAt   = *place;
    g_project   = reinterpret_cast<ProjectFn>(*project);
    g_slot      = reinterpret_cast<void**>(*slots) + facts::kFieldIconSlot;
    g_pending   = true;  // installed by the game thread's tick, outside the function
    g_installed = true;

    using control::Outcome;
    using control::Register;
    using control::Shape;
    using control::Value;
    Register({.name  = L"hud.field-icons",
              .shape = Shape::Text,
              .help  = L"(read only) field objective icons since the last reading: placements recorded and replayed per "
                       L"player, one icon's position from the tick and each replay, its alphas",
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
    Register({.name  = L"hud-field-icons",
              .shape = Shape::OnOff,
              .help  = L"field objective icons placed through each player's camera (on at start)",
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
    g_calls.clear();
    g_known.clear();
    if (!g_place.Restore())
    {
        platform::Log(L"field icons: the placement entry was changed after us; left in place (it only records)");
    }
}

void Tick(void* memberTwo) noexcept
{
    g_member = memberTwo;
    g_calls.clear();  // a frame whose overlay render did not run (no EndFrame) leaves nothing for the next
    if (!g_pending)
    {
        return;
    }
    g_pending     = false;
    const bool ok = g_place.Install(g_placeAt, facts::kFieldIconPlaceFirst, reinterpret_cast<std::uintptr_t>(&Place));
    platform::Log(L"field icons: {}", ok ? L"the objective icons' placements recorded for each player's replay (hud-field-icons on)"
                                         : L"the placement entry could not be redirected; the icons stay player 1's");
}

void Replay(int player, void* controller) noexcept
{
    if (!g_on || g_calls.empty() || (player == 2 && controller == nullptr))
    {
        return;
    }
    std::uint8_t* manager   = Manager();
    void*         canvasNow = nullptr;
    if (manager == nullptr || !Read(manager, canvasNow) || canvasNow == nullptr)
    {
        g_seen.canvasChanged += static_cast<std::uint32_t>(g_calls.size());
        return;
    }
    ListEntries(manager);
    Vector3    member;
    const bool memberKnown = player == 2 && MemberLocation(member);
    if (player == 2)
    {
        g_seen.memberKnown = memberKnown;
    }
    g_replaying = true;
    for (const auto& call : g_calls)
    {
        std::int32_t liveWeak[2] = {-1, 0};
        void*        widget      = players::player_two_camera::ObjectOf(call.weak);
        if (call.canvas != canvasNow)
        {
            ++g_seen.canvasChanged;
            continue;
        }
        if (widget == nullptr)
        {
            ++g_seen.widgetGone;
            continue;
        }
        // Still the entry the tick placed: listed in the map, and naming the same widget (not its memory reused).
        if (!Listed(call.entry) || !platform::CopyGuarded(liveWeak, call.entry, sizeof(liveWeak)) ||
            liveWeak[0] != call.weak[0] || liveWeak[1] != call.weak[1])
        {
            ++g_seen.entryGone;
            continue;
        }
        Known* known = KnownOf(call);
        if (known == nullptr)
        {
            continue;
        }
        View&          view   = player == 2 ? known->two : known->one;
        void*          pc     = player == 2 ? controller : call.controller;  // player 1: the tick's own controller
        const Vector3* leader = player == 2 && memberKnown ? &member : &call.leader;
        const float    dt     = player == 2 ? call.dt : 0.0f;  // player 1: the tick's step is already taken
        const bool     sample = &call == &g_calls.front();
        float          alphaTick = 0.0f;
        if (sample && player == 2)
        {
            Read(static_cast<const std::uint8_t*>(call.entry) + facts::kFieldIconEntryAlpha, alphaTick);
            if (const std::uint8_t* slot = SlotOf(call.canvas, widget))
            {
                platform::CopyGuarded(g_seen.tickAt, slot + facts::kCanvasSlotOffset, sizeof(g_seen.tickAt));
            }
        }
        bool          projected = false;
        std::uint8_t* slot      = nullptr;
        if (!PlaceFor(player, call, widget, pc, leader, dt, view, projected, slot))
        {
            continue;
        }
        if (sample)
        {
            Sample(player, call, slot, projected, view, alphaTick);
        }
        ++g_seen.replays[player == 2 ? 1 : 0];
        ++g_replayed[player == 2 ? 1 : 0];
    }
    g_replaying = false;
    if (player == 1 && !g_firstReplayLogged && g_replayed[1] != 0)
    {
        g_firstReplayLogged = true;
        platform::LogDebug(L"field icons: the first icon placed per view: tick ({:.0f},{:.0f}), player 2 ({:.0f},{:.0f}{}), "
                           L"player 1 ({:.0f},{:.0f}{}); alpha tick {:.2f}, player 2's {:.2f}; player 2's member {}",
                           g_seen.tickAt[0], g_seen.tickAt[1], g_seen.twoAt[0], g_seen.twoAt[1],
                           g_seen.twoProjected ? L"" : L" behind", g_seen.oneAt[0], g_seen.oneAt[1],
                           g_seen.oneProjected ? L"" : L" behind", g_seen.alphaTick, g_seen.alphaTwo,
                           g_seen.memberKnown ? L"read" : L"not read (the tick's leader location used)");
    }
}

void EndFrame() noexcept
{
    ++g_frame;
    ++g_seen.frames;
    const auto recorded = static_cast<std::uint32_t>(g_calls.size());
    if (recorded != 0)
    {
        ++g_seen.framesWithIcons;
        if (g_run++ == 0)
        {
            platform::Log(L"field icons: {} icon(s) placed this frame; replays for player 2: {}", recorded,
                          g_replayed[1] != 0 ? L"made" : L"none yet");
        }
        g_runRecorded += recorded;
    }
    else if (g_run != 0)
    {
        platform::Log(L"field icons: no icon this frame, after {} frames with {} placements ({} replayed for player 2)", g_run,
                      g_runRecorded, g_replayed[1]);
        g_run = g_runRecorded = 0;
    }
    g_calls.clear();
    // Icons not recorded for ten seconds (gone, or the HUD tick paused) are forgotten.
    std::erase_if(g_known, [](const Known& known) { return g_frame - known.seen > 600; });
}

std::wstring Reading()
{
    const Seen&  s = g_seen;
    std::wstring text = std::format(
        L"{} frames, {} with icons; recorded {}; replayed p2 {} p1 {}; skipped: canvas changed {}, entry gone {}, widget gone "
        L"{}, slot not found {}; faults {}; player 2's member {}",
        s.frames, s.framesWithIcons, s.recorded, s.replays[1], s.replays[0], s.canvasChanged, s.entryGone, s.widgetGone,
        s.noSlot, s.faults, s.memberKnown ? L"read" : L"not read");
    if (s.icon != nullptr)
    {
        text += std::format(L" | icon {}: tick ({:.0f},{:.0f}) p2 ({:.0f},{:.0f}{}) p1 ({:.0f},{:.0f}{}); alpha tick {:.2f} p2 {:.2f}",
                            s.icon, s.tickAt[0], s.tickAt[1], s.twoAt[0], s.twoAt[1], s.twoProjected ? L"" : L" behind",
                            s.oneAt[0], s.oneAt[1], s.oneProjected ? L"" : L" behind", s.alphaTick, s.alphaTwo);
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

}  // namespace lss::hud::field_icons
