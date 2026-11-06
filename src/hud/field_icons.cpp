#include "hud/field_icons.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "hud/overlay_pass.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/sparse_array.h"
#include "players/weak_object.h"
#include "world/actor_place.h"

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
constexpr int kMaxColumns = overlay_pass::kMaxColumns;
bool          g_replaying = false;

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
    View          views[kMaxColumns];  // by column; column 0's alpha is the entry's own: only its last offset is kept
    std::uint32_t seen = 0;            // the frame it was last recorded
};

/// A live entry of the icon map and its key (the navi icon's FName, as the map holds it).
struct Listing
{
    const void*   entry = nullptr;
    std::uint64_t key   = 0;
};
/// An icon one seat owns: its diamond shows in that seat's view only (SetOwner).
struct Owned
{
    std::uint64_t key  = 0;
    int           seat = -1;  // -1: the record is free
};

std::vector<Call>        g_calls;
std::vector<Known>       g_known;
std::vector<Listing>     g_listed;  // the icon map's live entries, walked once per replay
std::array<Owned, 4>     g_owned;
std::uint32_t            g_frame = 0;

/// What the readings show (`hud.field-icons`): counts since the last reading and one icon followed through the frame
/// (the tick's placement, then each column's: index = column).
struct Seen
{
    std::uint32_t frames = 0, framesWithIcons = 0, recorded = 0;
    std::uint32_t replays[kMaxColumns] = {}, faults = 0;
    std::uint32_t canvasChanged = 0, entryGone = 0, widgetGone = 0, noSlot = 0;
    std::uint32_t ownedHidden = 0;  // a seat's own icon placed transparent (the tick's or another view's placement)
    bool          memberKnown = false;  // the further column's member location was read
    void*         icon      = nullptr;
    float         tickAt[2] = {}, at[kMaxColumns][2] = {};
    bool          projected[kMaxColumns] = {};
    float         alphaTick = 0.0f, alphaFurther = 0.0f;  // the entry's alpha as the tick left it, the further column's
};
Seen          g_seen;
std::uint32_t g_run = 0, g_runRecorded = 0;  // consecutive frames with icons (an Info line when a run starts and ends)
std::uint32_t g_replayed[kMaxColumns] = {};

/// Whose view a column is, for the log and the reading: the seat in it (player 3 in column 1, counted from 0, while
/// player 2 is out).
std::wstring PlayerOf(int column)
{
    const int seat = overlay_pass::SeatOf(column);
    return seat >= 0 ? std::format(L"player {}", seat + 1) : std::format(L"column {} (no player)", column + 1);
}
bool          g_firstReplayLogged = false;

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

std::uint8_t* Manager() noexcept
{
    void* manager = nullptr;
    return g_slot != nullptr && Read(g_slot, manager) ? static_cast<std::uint8_t*>(manager) : nullptr;
}

void ListEntries(const std::uint8_t* manager) noexcept;
bool Listed(const void* entry) noexcept;
int  OwnerOf(const void* entry) noexcept;
bool AnyOwned() noexcept;

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
        // A seat's own icon: the tick's placement (player 1's view, and the single view's) keeps it transparent, its alpha
        // held at 0 with no step (RE-lead-marker.md); the owner's column fades its own alpha in the replay.
        if (AnyOwned())
        {
            static std::uint32_t listedAt = ~0u;  // the frame the tick last walked the map
            if (std::uint8_t* manager = Manager())
            {
                if (listedAt != g_frame || !Listed(entry))  // once a frame, again for an entry added since
                {
                    ListEntries(manager);
                    listedAt = g_frame;
                }
                if (OwnerOf(entry) >= 1)
                {
                    constexpr float kHidden = 0.0f;
                    platform::WriteGuarded(static_cast<std::uint8_t*>(entry) + facts::kFieldIconEntryAlpha, &kHidden, sizeof(kHidden));
                    dt = 0.0f;
                    ++g_seen.ownedHidden;
                }
            }
        }
    }
    reinterpret_cast<PlaceFn>(g_place.Original())(entry, canvas, dt, controller, leader, unused, icon);
}

/// The entries that are values in the manager's icon map now (an entry freed by the tick's removal would otherwise be
/// written): the map's sparse array walked by its allocation bits, as the tick walks it.
void ListEntries(const std::uint8_t* manager) noexcept
try
{
    g_listed.clear();
    platform::ForEachSparseElement(reinterpret_cast<std::uintptr_t>(manager) + facts::kFieldIconMapData, facts::kFieldIconMapElement,
                                   platform::kSmallSetMaxBits, [](std::uintptr_t element) {
                                       Listing listing;
                                       if (Read(reinterpret_cast<const void*>(element + facts::kFieldIconMapElementEntry), listing.entry) &&
                                           listing.entry != nullptr)
                                       {
                                           Read(reinterpret_cast<const void*>(element + facts::kFieldIconMapElementKey), listing.key);
                                           g_listed.push_back(listing);
                                       }
                                       return true;
                                   });
}
catch (...)
{
    // An allocation failure: the entries listed so far stand, as before.
}

bool Listed(const void* entry) noexcept
{
    for (const Listing& listed : g_listed)
    {
        if (listed.entry == entry)
        {
            return true;
        }
    }
    return false;
}

bool AnyOwned() noexcept
{
    for (const Owned& owned : g_owned)
    {
        if (owned.seat >= 0)
        {
            return true;
        }
    }
    return false;
}

/// The seat owning the listed `entry`'s icon, or -1 (every view's).
int OwnerOf(const void* entry) noexcept
{
    for (const Listing& listed : g_listed)
    {
        if (listed.entry != entry)
        {
            continue;
        }
        for (const Owned& owned : g_owned)
        {
            if (owned.seat >= 0 && owned.key == listed.key)
            {
                return owned.seat;
            }
        }
        return -1;
    }
    return -1;
}

/// The canvas slot holding `widget`, matched on the widget's Slate object as the placement matches it (the game's
/// GetCachedWidget: the GC widget at +0xD8, else the plain one at +0xC8), or nullptr.
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

/// The character's root component's world location (the icons' range gate for that character's column).
bool MemberLocation(void* member, Vector3& out) noexcept
{
    float at[3]{};
    if (!world::actor_place::ActorLocation(member, at))
    {
        return false;
    }
    out = {at[0], at[1], at[2]};
    return true;
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

/// One icon placed again for a column's view: for a further column with that column's alpha swapped into the entry and
/// the tick's written back around the call; the slot's offset then read back (this view's projection wrote it) or this
/// view's last one written (it did not). `hidden` (a seat's own icon in another view): alpha 0 swapped in, no step, so
/// the placement leaves it transparent for this pass. Returns false when the icon stays as the game placed it.
bool PlaceFor(int column, const Call& call, void* widget, void* controller, const Vector3* leader, float dt, bool hidden, View& view,
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
    constexpr float kHidden = 0.0f;
    const bool      swap    = column != 0 || hidden;
    if (swap && !platform::WriteGuarded(entry + facts::kFieldIconEntryAlpha, hidden ? &kHidden : &view.alpha, sizeof(view.alpha)))
    {
        Fault(2, L"an icon's alpha for a further column");
        return false;
    }
    const bool placed = platform::CallGuarded(reinterpret_cast<PlaceFn>(g_place.Original()), call.entry, call.canvas,
                                              hidden ? 0.0f : dt, controller, leader, std::int32_t{0},
                                              static_cast<const Vector3*>(&call.icon));
    if (swap)
    {
        if (hidden)
        {
            view.alpha = 0.0f;  // fades in from 0 if this view becomes the owner's
            ++g_seen.ownedHidden;
        }
        else
        {
            Read(entry + facts::kFieldIconEntryAlpha, view.alpha);  // this column's step taken
        }
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

/// The first recorded icon followed through the frame for the reading: the tick's offset (read before the first further
/// placement, which overwrites it), then each column's offset after its placement.
void Sample(int column, const Call& call, const std::uint8_t* slot, bool projected, const View& view, float alphaTick) noexcept
{
    g_seen.icon = call.entry;
    if (column != 0)
    {
        g_seen.alphaTick    = alphaTick;
        g_seen.alphaFurther = view.alpha;
    }
    g_seen.projected[column] = projected;
    if (slot != nullptr)
    {
        platform::CopyGuarded(g_seen.at[column], slot + facts::kCanvasSlotOffset, sizeof(g_seen.at[column]));
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

void Tick() noexcept
{
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

void Replay(int column, void* controller, void* member) noexcept
{
    if (!g_on || g_calls.empty() || column < 0 || column >= kMaxColumns || (column != 0 && controller == nullptr))
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
    Vector3    memberAt;
    const bool memberKnown = column != 0 && MemberLocation(member, memberAt);
    if (column != 0)
    {
        g_seen.memberKnown = memberKnown;
    }
    g_replaying = true;
    for (const auto& call : g_calls)
    {
        std::int32_t liveWeak[2] = {-1, 0};
        void*        widget      = players::weak_object::ObjectOf(call.weak);
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
        View&          view   = known->views[column];
        void*          pc     = column != 0 ? controller : call.controller;  // column 0: the tick's own controller
        const Vector3* leader = column != 0 && memberKnown ? &memberAt : &call.leader;
        const float    dt     = column != 0 ? call.dt : 0.0f;  // column 0: the tick's step is already taken
        const bool     sample = &call == &g_calls.front();
        float          alphaTick = 0.0f;
        if (sample && column != 0)
        {
            Read(static_cast<const std::uint8_t*>(call.entry) + facts::kFieldIconEntryAlpha, alphaTick);
            if (const std::uint8_t* slot = SlotOf(call.canvas, widget))
            {
                platform::CopyGuarded(g_seen.tickAt, slot + facts::kCanvasSlotOffset, sizeof(g_seen.tickAt));
            }
        }
        // A seat's own icon shows in that seat's column only (a mirror's column has no seat: hidden there too).
        const int     owner     = OwnerOf(call.entry);
        const bool    hidden    = owner >= 0 && owner != overlay_pass::SeatOf(column);
        bool          projected = false;
        std::uint8_t* slot      = nullptr;
        if (!PlaceFor(column, call, widget, pc, leader, dt, hidden, view, projected, slot))
        {
            continue;
        }
        if (sample)
        {
            Sample(column, call, slot, projected, view, alphaTick);
        }
        ++g_seen.replays[column];
        ++g_replayed[column];
    }
    g_replaying = false;
    if (column == 0 && !g_firstReplayLogged && FurtherReplayed() != 0 && platform::Enabled(platform::Level::Debug))
    {
        g_firstReplayLogged = true;
        const std::wstring further = PlayerOf(1);  // the further pass that runs last before player 1's: column 1's
        platform::LogDebug(L"field icons: the first icon placed per view: tick ({:.0f},{:.0f}), {} ({:.0f},{:.0f}{}), "
                           L"player 1 ({:.0f},{:.0f}{}); alpha tick {:.2f}, {}'s {:.2f}; {}'s member {}",
                           g_seen.tickAt[0], g_seen.tickAt[1], further, g_seen.at[1][0], g_seen.at[1][1],
                           g_seen.projected[1] ? L"" : L" behind", g_seen.at[0][0], g_seen.at[0][1],
                           g_seen.projected[0] ? L"" : L" behind", g_seen.alphaTick, further, g_seen.alphaFurther, further,
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
            platform::Log(L"field icons: {} icon(s) placed this frame; replays for the further columns: {}", recorded,
                          FurtherReplayed() != 0 ? L"made" : L"none yet");
        }
        g_runRecorded += recorded;
    }
    else if (g_run != 0)
    {
        platform::Log(L"field icons: no icon this frame, after {} frames with {} placements ({} replayed for the further columns)",
                      g_run, g_runRecorded, FurtherReplayed());
        g_run = g_runRecorded = 0;
    }
    g_calls.clear();
    // Icons not recorded for ten seconds (gone, or the HUD tick paused) are forgotten.
    std::erase_if(g_known, [](const Known& known) { return g_frame - known.seen > 600; });
}

std::wstring Reading()
{
    const Seen&  s = g_seen;
    std::wstring replayed;
    for (int column = 0; column < kMaxColumns; ++column)
    {
        replayed += std::format(L"{}{} {}", column == 0 ? L"" : L", ", PlayerOf(column), s.replays[column]);
    }
    const std::wstring further = PlayerOf(1);
    std::wstring text = std::format(
        L"{} frames, {} with icons; recorded {}; replayed: {}; skipped: canvas changed {}, entry gone {}, widget gone "
        L"{}, slot not found {}; faults {}; {}'s member {}; a seat's own icon placed transparent {}",
        s.frames, s.framesWithIcons, s.recorded, replayed, s.canvasChanged, s.entryGone, s.widgetGone, s.noSlot, s.faults,
        further, s.memberKnown ? L"read" : L"not read", s.ownedHidden);
    if (s.icon != nullptr)
    {
        text += std::format(L" | icon {}: tick ({:.0f},{:.0f}) {} ({:.0f},{:.0f}{}) player 1 ({:.0f},{:.0f}{}); alpha tick {:.2f} "
                            L"{} {:.2f}",
                            s.icon, s.tickAt[0], s.tickAt[1], further, s.at[1][0], s.at[1][1], s.projected[1] ? L"" : L" behind",
                            s.at[0][0], s.at[0][1], s.projected[0] ? L"" : L" behind", s.alphaTick, further, s.alphaFurther);
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

bool SetOwner(std::uint64_t key, int seat) noexcept
{
    Owned* free = nullptr;
    for (Owned& owned : g_owned)
    {
        if (owned.seat >= 0 && owned.key == key)
        {
            owned.seat = seat;  // -1 frees it
            return true;
        }
        if (owned.seat < 0 && free == nullptr)
        {
            free = &owned;
        }
    }
    if (seat < 0)
    {
        return true;
    }
    if (free == nullptr)
    {
        return false;
    }
    *free = Owned{.key = key, .seat = seat};
    return true;
}

}  // namespace lss::hud::field_icons
