#include "world/target_icons.h"

#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/scopes.h"
#include "players/seats.h"
#include "players/weak_object.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace lss::world::target_icons {

namespace {

using platform::Read;
using platform::ReadPointer;
using players::SeatName;
using players::weak_object::Weak;

using ShowFn       = void* (*)(void* ui15, void* world, std::uint64_t name, const void* location, std::uint8_t state, std::uint8_t type);
using FirstLocalFn = void* (*)(void* world);
using DestroyFn    = void (*)(void* ui15, void* widget);
using IconTickFn   = void (*)(void* ui15);
using RemoveFn     = void (*)(void* mgr, const std::uint64_t* name);
using FindEntryFn  = void* (*)(void* mgr, const std::uint64_t* name);

constexpr int           kSeats         = players::scopes::kMaxPlayers;
constexpr std::uint32_t kMaxMismatches = 8;  // icons made with the wrong owner before the seats' icons stop
static_assert(kItems == facts::kTargetsItems);

/// A seat's icons: one widget per pool item, the object each was made as, and what the seat's pick consumes.
struct Seat
{
    std::array<void*, kItems>        widgets{};  // the seat's widget per item (the item's +0x78 while exchanged in)
    std::array<void*, kItems>        known{};    // the widget `ids` was made for
    std::array<Weak, kItems>         ids{};
    std::array<std::uint8_t, kItems> dirty{};    // location refreshes posted that the seat's pick has not consumed
    float                            gauge = 0.0f;  // the icon manager's last gauge ratio, the seat's
    std::uint32_t                    held  = 0;     // widgets now
    std::uint32_t made = 0, gone = 0, removed = 0, targetGone = 0, forgotten = 0, dropped = 0;
};

// Set at install, then only read.
std::uintptr_t g_ui15Slot = 0, g_gameMode = 0;
std::uintptr_t g_showSite = 0, g_show = 0, g_ownerSite = 0, g_firstLocal = 0, g_removeAt = 0;
DestroyFn      g_destroy   = nullptr;
IconTickFn     g_iconTick  = nullptr;
FindEntryFn    g_findEntry = nullptr;
bool           g_installed = false;

platform::CallRedirect  g_showCall, g_ownerCall;
platform::EntryRedirect g_remove;
bool                    g_placeTried = false;
bool                    g_placed     = false;

// Game thread.
std::array<Seat, kSeats> g_seats{};
int                      g_swapSeat   = 0;        // the seat whose arrays are exchanged in now
void*                    g_swapPlayer = nullptr;  // its local player
std::vector<std::pair<const void*, int>> g_owned;  // the seats' widgets, for OwnerSeat
bool                     g_ownedStale = true;
bool                     g_passFaulted = false;
std::uint32_t            g_mismatched = 0, g_passes = 0, g_answered = 0, g_playerOneFreed = 0, g_offThreadSeen = 0;

// Any thread.
std::atomic<DWORD>  g_gameThread{0};
std::atomic<void*>  g_creatingPlayer{nullptr};  // the owner the widget creation's call answers, on g_creatingThread
std::atomic<DWORD>  g_creatingThread{0};
std::atomic<bool>   g_swapHeld{false};   // a seat's arrays are exchanged in (the items hold the seat's widgets)
std::atomic<bool>   g_anyHeld{false};
std::atomic<std::uint32_t> g_offThreadFrees{0};
std::array<std::atomic<std::uint64_t>, kItems / 64> g_freedElsewhere{};  // items freed off the game thread

bool OnGameThreadNow() noexcept
{
    return GetCurrentThreadId() == g_gameThread.load(std::memory_order_relaxed);
}

void* Ui15() noexcept
{
    return ReadPointer(reinterpret_cast<const void*>(g_ui15Slot), 0);
}

std::uint8_t* ItemAt(void* mgr, std::size_t index) noexcept
{
    return static_cast<std::uint8_t*>(mgr) + facts::kTargetsPool + index * facts::kTargetsItemSize;
}

/// The pool index of an item (or of its entry when `offset` is the entry's), or kItems if it is not one.
std::size_t IndexOf(const void* mgr, const void* at, std::uint32_t offset) noexcept
{
    const auto first = reinterpret_cast<std::uintptr_t>(mgr) + facts::kTargetsPool + offset;
    const auto here  = reinterpret_cast<std::uintptr_t>(at);
    if (at == nullptr || here < first || (here - first) % facts::kTargetsItemSize != 0)
    {
        return kItems;
    }
    const std::size_t index = (here - first) / facts::kTargetsItemSize;
    return index < kItems ? index : kItems;
}

/// The first `count` items' widget pointers (and location-dirty bytes, if `dirty`) exchanged with the arrays: twice puts
/// both back. Returns how many were exchanged (fewer than `count` if one faulted).
std::size_t ExchangeItems(void* mgr, void** widgets, std::uint8_t* dirty, std::size_t count) noexcept
{
    std::size_t done = 0;
    __try
    {
        for (; done < count; ++done)
        {
            std::uint8_t* item   = ItemAt(mgr, done);
            auto**        widget = reinterpret_cast<void**>(item + facts::kTargetsItemWidget);
            void*         live   = *widget;
            *widget              = widgets[done];
            widgets[done]        = live;
            if (dirty != nullptr)
            {
                const std::uint8_t posted      = item[facts::kTargetsItemDirty];
                item[facts::kTargetsItemDirty] = dirty[done];
                dirty[done]                    = posted;
            }
        }
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
    }
    return done;
}

/// The items' posted location refreshes OR'ed into a seat's own (false if one faulted).
bool KeepDirty(void* mgr, std::uint8_t* dirty) noexcept
{
    __try
    {
        for (std::size_t i = 0; i < kItems; ++i)
        {
            dirty[i] |= ItemAt(mgr, i)[facts::kTargetsItemDirty];
        }
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

void Recount(Seat& seat) noexcept
{
    std::uint32_t held = 0;
    for (const void* widget : seat.widgets)
    {
        held += widget != nullptr ? 1 : 0;
    }
    seat.held    = held;
    g_ownedStale = true;
    bool any     = false;
    for (int s = 1; s <= players::kSeatsBuilt; ++s)
    {
        any = any || g_seats[s].held != 0;
    }
    g_anyHeld.store(any, std::memory_order_relaxed);
}

/// Every seat recounted but the one whose arrays are exchanged in now (it is recounted when they come back).
void RecountAll() noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (seat != g_swapSeat)
        {
            Recount(g_seats[seat]);
        }
    }
}

/// The game's destroy of one widget (game thread), if it is still the object it was made as.
bool DestroyWidget(void* widget, bool alive) noexcept
{
    void* ui15 = Ui15();
    return alive && ui15 != nullptr && platform::CallGuarded(g_destroy, ui15, widget);
}

enum class Why
{
    Removed,     // its target was freed
    TargetGone,  // its target is no longer active (the per-tick check)
    Dropped,     // the seat left, changed member or local player, or the passes stopped
};

/// The seat's widget of item `index` destroyed and forgotten. `exchanged`: the arrays are swapped in now, so the slot
/// holds player 1's widget of that item (its id is not this slot's: liveness is read from the object array; the seat's
/// own, in the item, is destroyed by the game's free and forgotten when the arrays come back).
void DestroyAt(int seatIndex, std::size_t index, Why why, bool exchanged) noexcept
{
    Seat& seat   = g_seats[seatIndex];
    void* widget = seat.widgets[index];
    if (widget == nullptr)
    {
        return;
    }
    const bool alive = exchanged ? players::weak_object::Live(widget) : players::weak_object::Resolve(seat.ids[index]) == widget;
    DestroyWidget(widget, alive);
    seat.widgets[index] = nullptr;
    if (exchanged)
    {
        ++g_playerOneFreed;
        return;
    }
    seat.known[index] = nullptr;
    seat.ids[index]   = {};
    (why == Why::Removed ? seat.removed : why == Why::TargetGone ? seat.targetGone : seat.dropped) += 1;
}

/// After the seat's run: the widgets it made get their ids, the ones its run destroyed (the far sweep, a freed target)
/// are forgotten.
void Note(int seatIndex) noexcept
{
    Seat&         seat = g_seats[seatIndex];
    std::uint32_t made = 0, gone = 0;
    for (std::size_t i = 0; i < kItems; ++i)
    {
        void* widget = seat.widgets[i];
        if (widget == seat.known[i])
        {
            continue;
        }
        if (widget == nullptr)
        {
            ++gone;
            seat.ids[i] = {};
        }
        else
        {
            ++made;
            seat.ids[i] = players::weak_object::MakeWeak(widget);
        }
        seat.known[i] = widget;
    }
    if (made + gone == 0)
    {
        return;
    }
    seat.made += made;
    seat.gone += gone;
    Recount(seat);
    platform::LogDebug(L"target icons: {}'s pick made {} icon(s) and dropped {}; {} now", SeatName(seatIndex), made, gone, seat.held);
}

/// The widget's owner (its player context's local player, +0x1E8) is `localPlayer`.
bool OwnedBy(const void* widget, const void* localPlayer) noexcept
{
    std::int32_t owner = -1, index = -2;
    return Read(static_cast<const std::uint8_t*>(widget) + facts::kWidgetOwnerPlayer, owner) &&
           Read(static_cast<const std::uint8_t*>(localPlayer) + facts::kObjectIndex, index) && owner == index;
}

void ClearCreating() noexcept
{
    g_creatingThread.store(0, std::memory_order_relaxed);
    g_creatingPlayer.store(nullptr, std::memory_order_relaxed);
}

// --- The redirects -----------------------------------------------------------------------------------------------------

/// The icon setter's Show: inside a seat's pick, the widget it makes is owned by the seat's local player.
void* Show(void* ui15, void* world, std::uint64_t name, const void* location, std::uint8_t state, std::uint8_t type)
{
    const auto original = reinterpret_cast<ShowFn>(g_showCall.Original());
    if (!OnGameThreadNow() || g_swapSeat == 0 || g_swapPlayer == nullptr)
    {
        return original(ui15, world, name, location, state, type);
    }
    if (g_mismatched >= kMaxMismatches)
    {
        return nullptr;  // the owner answer does not reach this game's creation: the seats show no icons (logged)
    }
    // A CreatingAs (world/search_arrows) is never held here: it is opened only around a hook area's call, which makes no
    // target icon; if one ever were, this clear would end its answer early and its own owner check would catch it.
    g_creatingPlayer.store(g_swapPlayer, std::memory_order_relaxed);
    g_creatingThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    void* widget = original(ui15, world, name, location, state, type);
    ClearCreating();  // also in ~SeatSwap: a fault in Show skips this line
    if (widget != nullptr && !OwnedBy(widget, g_swapPlayer))
    {
        // Player 1's (a creation path the owner call did not reach): it would show in player 1's view.
        ++g_mismatched;
        platform::LogWarning(L"target icons: an icon made in {}'s pick is not that player's; destroyed{}", SeatName(g_swapSeat),
                             g_mismatched >= kMaxMismatches ? L" (no new icons for further players from here on; those shown stay until swept)"
                                                             : L"");
        platform::CallGuarded(g_destroy, ui15, widget);
        return nullptr;
    }
    return widget;
}

/// The widget creation's "first local player" call: the seat's player while its pick's Show runs on this thread.
void* FirstLocalPlayer(void* world)
{
    void* player = g_creatingPlayer.load(std::memory_order_relaxed);
    if (player != nullptr && g_creatingThread.load(std::memory_order_relaxed) == GetCurrentThreadId())
    {
        ++g_answered;
        return player;
    }
    return reinterpret_cast<FirstLocalFn>(g_firstLocal)(world);
}

/// The one function that frees a target: the seats' widgets of it are destroyed first (the game's own call then destroys
/// the live one). Inside a seat's run the seat's array holds player 1's widgets, so the free is handled then even while
/// no seat holds an icon (else player 1's widget of the freed target would come back into the freed item). A free on
/// another thread is queued for the next game tick.
void RemoveTarget(void* mgr, const std::uint64_t* name)
{
    const auto original = reinterpret_cast<RemoveFn>(g_remove.Original());
    if (mgr != nullptr && name != nullptr && (g_swapHeld.load(std::memory_order_relaxed) || g_anyHeld.load(std::memory_order_relaxed)))
    {
        void* entry = nullptr;
        if (platform::CallGuarded(g_findEntry, entry, mgr, name))
        {
            if (const std::size_t index = IndexOf(mgr, entry, facts::kTargetsItemEntry); index != kItems)
            {
                if (OnGameThreadNow())
                {
                    std::uint32_t destroyed = 0;
                    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
                    {
                        destroyed += g_seats[seat].widgets[index] != nullptr ? 1 : 0;
                        DestroyAt(seat, index, Why::Removed, seat == g_swapSeat);
                    }
                    RecountAll();
                    if (destroyed != 0)
                    {
                        platform::LogDebug(L"target icons: a target was freed; {} icon(s) of it destroyed with it (a seat's, or player 1's "
                                           L"inside a seat's run)", destroyed);
                    }
                }
                else
                {
                    g_freedElsewhere[index / 64].fetch_or(std::uint64_t{1} << (index % 64), std::memory_order_relaxed);
                    g_offThreadFrees.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
    original(mgr, name);
}

void RemoveRedirects() noexcept
{
    g_remove.Restore();
    g_showCall.Restore();
    if (OnGameThreadNow())
    {
        g_ownerCall.Restore();  // crosses a block: only from the game thread, outside the creation
    }
}

/// The frees queued off the game thread: the seats' widgets of those items destroyed, and a widget left in a freed item
/// (the free came during a seat's run: player 1's came back into it) destroyed with the game's call.
bool DestroyFreedElsewhere(void* mgr, bool listed, const std::bitset<kItems>& active) noexcept
{
    bool changed = false;
    for (std::size_t word = 0; word < g_freedElsewhere.size(); ++word)
    {
        std::uint64_t bits = g_freedElsewhere[word].exchange(0, std::memory_order_relaxed);
        for (std::size_t bit = 0; bits != 0; ++bit, bits >>= 1)
        {
            if ((bits & 1) == 0)
            {
                continue;
            }
            const std::size_t index = word * 64 + bit;
            for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
            {
                DestroyAt(seat, index, Why::Removed, false);
            }
            auto* slot   = reinterpret_cast<void**>(ItemAt(mgr, index) + facts::kTargetsItemWidget);
            void* widget = nullptr;
            if (listed && !active.test(index) && Read(slot, widget) && widget != nullptr)
            {
                DestroyWidget(widget, players::weak_object::Live(widget));
                void* none = nullptr;
                platform::WriteGuarded(slot, &none, sizeof(none));
                platform::LogDebug(L"target icons: an icon left in a target freed off the game thread destroyed");
            }
            changed = true;
        }
    }
    if (const std::uint32_t frees = g_offThreadFrees.load(std::memory_order_relaxed); frees != g_offThreadSeen)
    {
        g_offThreadSeen = frees;
        platform::LogDebug(L"target icons: {} target free(s) off the game thread so far (handled on the game thread)", frees);
    }
    return changed;
}

}  // namespace

bool ActiveItems(void* mgr, std::bitset<kItems>& active) noexcept
{
    active.reset();
    const void* item = ReadPointer(mgr, facts::kTargetsActiveHead);
    for (std::size_t steps = 0; item != nullptr; ++steps)
    {
        const std::size_t index = IndexOf(mgr, item, 0);
        if (steps >= kItems || index == kItems)
        {
            return false;
        }
        active.set(index);
        if (!Read(static_cast<const std::uint8_t*>(item) + facts::kTargetsItemNext, item))
        {
            return false;
        }
    }
    return true;
}

bool Install(const platform::GameImage& image) noexcept
{
    const auto fail = [](const wchar_t* what) {
        platform::LogWarning(L"target icons: {} is not as the research read it; further players get no prompts and press nothing", what);
        return false;
    };
    const auto ui15      = image.Rebase(facts::kTargetIconSlotGlobal, 8);
    const auto gameMode  = image.Rebase(facts::kGameModeGlobal, 8);
    const auto showSite  = image.Rebase(facts::kTargetIconShowCall, 5);
    const auto show      = image.Rebase(facts::kTargetIconShow);
    const auto ownerSite = image.Rebase(facts::kWidgetCreateOwnerCall, 5);
    const auto first     = image.Rebase(facts::kFirstLocalPlayer);
    const auto destroy   = platform::FindFunction(image, facts::kTargetIconHideDestroy);
    const auto iconTick  = platform::FindFunction(image, facts::kTargetIconTick);
    const auto remove    = platform::FindFunction(image, facts::kRemoveTarget);
    const auto findEntry = platform::FindFunction(image, facts::kFindTargetEntry);
    if (!ui15 || !gameMode || !showSite || !show || !ownerSite || !first || !destroy || !iconTick || !remove || !findEntry)
    {
        return fail(L"an icon function, call site or global");
    }
    if (!players::weak_object::Installed())
    {
        return fail(L"the object array (weak pairs)");
    }
    g_ui15Slot = *ui15, g_gameMode = *gameMode;
    g_showSite = *showSite, g_show = *show, g_ownerSite = *ownerSite, g_firstLocal = *first, g_removeAt = *remove;
    g_destroy   = reinterpret_cast<DestroyFn>(*destroy);
    g_iconTick  = reinterpret_cast<IconTickFn>(*iconTick);
    g_findEntry = reinterpret_cast<FindEntryFn>(*findEntry);
    g_installed = true;
    return true;
}

bool Place() noexcept
{
    if (!g_installed)
    {
        return false;
    }
    g_gameThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (g_placeTried)
    {
        return g_placed;
    }
    g_placeTried = true;
    g_placed     = g_showCall.Install(g_showSite, g_show, reinterpret_cast<std::uintptr_t>(&Show)) &&
               g_ownerCall.InstallFromOwnThread(g_ownerSite, g_firstLocal, reinterpret_cast<std::uintptr_t>(&FirstLocalPlayer)) &&
               g_remove.Install(g_removeAt, std::span(facts::kRemoveTarget.entryBytes.data(), facts::kRemoveTargetFirst),
                                reinterpret_cast<std::uintptr_t>(&RemoveTarget));
    if (!g_placed)
    {
        RemoveRedirects();
        platform::LogWarning(L"target icons: the icon calls could not be redirected; further players get no prompts and press nothing");
        return false;
    }
    platform::Log(L"target icons: each further player's pick makes its own target icons, shown in its own view");
    return true;
}

bool Placed() noexcept
{
    return g_placed;
}

void Remove() noexcept
{
    if (OnGameThreadNow())
    {
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            DropSeat(seat);
        }
    }
    RemoveRedirects();
    g_placed = false;
}

void Tick(void* mgr) noexcept
{
    g_gameThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (!g_placed || mgr == nullptr)
    {
        return;
    }
    bool queued = false;
    for (const auto& word : g_freedElsewhere)
    {
        queued = queued || word.load(std::memory_order_relaxed) != 0;
    }
    if (!queued && !g_anyHeld.load(std::memory_order_relaxed))
    {
        return;  // nothing to check (no walk of the list in single player)
    }
    std::bitset<kItems> active;
    const bool          listed  = ActiveItems(mgr, active);
    bool                changed = queued && DestroyFreedElsewhere(mgr, listed, active);
    if (listed && g_anyHeld.load(std::memory_order_relaxed))
    {
        for (int seatIndex = 1; seatIndex <= players::kSeatsBuilt; ++seatIndex)
        {
            Seat&         seat = g_seats[seatIndex];
            std::uint32_t gone = 0, lost = 0;
            for (std::size_t i = 0; seat.held != 0 && i < kItems; ++i)
            {
                void* widget = seat.widgets[i];
                if (widget == nullptr)
                {
                    continue;
                }
                if (!active.test(i))
                {
                    DestroyAt(seatIndex, i, Why::TargetGone, false);
                    ++gone;
                }
                else if (players::weak_object::Resolve(seat.ids[i]) != widget)
                {
                    // Gone from the engine (the slot-15 tick drops it from its map); one never named (its pair could
                    // not be made) that still lives is destroyed rather than left untracked in player 1's view.
                    if (!seat.ids[i].Set())
                    {
                        DestroyWidget(widget, players::weak_object::Live(widget));
                    }
                    seat.widgets[i] = seat.known[i] = nullptr;
                    seat.ids[i]     = {};
                    ++seat.forgotten;
                    ++lost;
                }
            }
            if (gone + lost != 0)
            {
                changed = true;
                platform::LogDebug(L"target icons: {}'s icons of targets no longer listed destroyed: {}; gone from the engine: {}",
                                   SeatName(seatIndex), gone, lost);
            }
        }
    }
    if (changed)
    {
        RecountAll();
    }
}

void BeforePick(void* mgr) noexcept
{
    if (!g_placed || mgr == nullptr)
    {
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (!KeepDirty(mgr, g_seats[seat].dirty.data()))
        {
            return;
        }
    }
}

SeatSwap::SeatSwap(int seat, void* mgr, void* localPlayer, bool pick) noexcept : seat_(seat), mgr_(mgr), pick_(pick)
{
    if (!g_placed || seat < 1 || seat > players::kSeatsBuilt || mgr == nullptr || localPlayer == nullptr || g_swapSeat != 0)
    {
        return;
    }
    Seat&         s     = g_seats[seat];
    std::uint8_t* dirty = pick ? s.dirty.data() : nullptr;
    if (const std::size_t done = ExchangeItems(mgr, s.widgets.data(), dirty, kItems); done != kItems)
    {
        ExchangeItems(mgr, s.widgets.data(), dirty, done);  // the ones exchanged, back
        return;
    }
    if (pick)
    {
        auto* ui15 = static_cast<std::uint8_t*>(Ui15());
        float live = 0.0f;
        if (ui15 != nullptr && Read(ui15 + facts::kTargetIconGaugeRatio, live) &&
            platform::WriteGuarded(ui15 + facts::kTargetIconGaugeRatio, &s.gauge, sizeof(s.gauge)))
        {
            s.gauge       = live;
            gaugeSwapped_ = true;
        }
    }
    g_swapSeat   = seat;
    g_swapPlayer = localPlayer;
    g_swapHeld.store(true, std::memory_order_relaxed);
    held_ = true;
}

SeatSwap::~SeatSwap()
{
    if (!held_)
    {
        return;
    }
    ClearCreating();  // a fault inside the game's Show skipped its own clear
    Seat& s = g_seats[seat_];
    if (gaugeSwapped_)
    {
        auto* ui15 = static_cast<std::uint8_t*>(Ui15());
        float own  = 0.0f;
        if (ui15 != nullptr && Read(ui15 + facts::kTargetIconGaugeRatio, own) &&
            platform::WriteGuarded(ui15 + facts::kTargetIconGaugeRatio, &s.gauge, sizeof(s.gauge)))
        {
            s.gauge = own;
        }
    }
    // The dirty bytes come back as the seat's unconsumed ones (the pick clears only those of targets in its range).
    if (ExchangeItems(mgr_, s.widgets.data(), pick_ ? s.dirty.data() : nullptr, kItems) != kItems)
    {
        platform::LogWarning(L"target icons: {}'s icons could not all be put back; player 1's may show {}'s", SeatName(seat_), SeatName(seat_));
    }
    g_swapSeat   = 0;
    g_swapPlayer = nullptr;
    g_swapHeld.store(false, std::memory_order_relaxed);
    Note(seat_);
}

CreatingAs::CreatingAs(void* localPlayer) noexcept
{
    if (!g_placed || localPlayer == nullptr || !OnGameThreadNow() || g_creatingPlayer.load(std::memory_order_relaxed) != nullptr)
    {
        return;
    }
    g_creatingPlayer.store(localPlayer, std::memory_order_relaxed);
    g_creatingThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    held_ = true;
}

CreatingAs::~CreatingAs()
{
    if (held_)
    {
        ClearCreating();
    }
}

void DropSeat(int seat) noexcept
{
    if (seat < 1 || seat > players::kSeatsBuilt)
    {
        return;
    }
    Seat& s = g_seats[seat];
    if (s.held != 0)
    {
        platform::LogDebug(L"target icons: {}'s {} icon(s) destroyed (the player left, changed member, or the passes stopped)",
                           SeatName(seat), s.held);
    }
    for (std::size_t i = 0; s.held != 0 && i < kItems; ++i)
    {
        DestroyAt(seat, i, Why::Dropped, false);
    }
    s.dirty.fill(0);
    s.gauge = 0.0f;
    Recount(s);
}

int OwnerSeat(const void* widget) noexcept
{
    if (g_ownedStale)
    {
        g_ownedStale = false;
        g_owned.clear();
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            for (const void* own : g_seats[seat].widgets)
            {
                if (own != nullptr)
                {
                    g_owned.emplace_back(own, seat);
                }
            }
        }
    }
    for (const auto& [own, seat] : g_owned)
    {
        if (own == widget)
        {
            return seat;
        }
    }
    return 0;
}

bool AnySeatIcons() noexcept
{
    return g_anyHeld.load(std::memory_order_relaxed);
}

void PlaceForPass() noexcept
{
    if (!AnySeatIcons() || g_passFaulted)
    {
        return;
    }
    void*           ui15 = Ui15();
    auto*           mode = static_cast<std::uint8_t*>(ReadPointer(reinterpret_cast<const void*>(g_gameMode), 0));
    float           step = 0.0f;
    constexpr float zero = 0.0f;
    if (ui15 == nullptr || mode == nullptr || !Read(mode + facts::kGameModeHudStep, step) ||
        !platform::WriteGuarded(mode + facts::kGameModeHudStep, &zero, sizeof(zero)))
    {
        return;
    }
    const bool ok = platform::CallGuarded(g_iconTick, ui15);
    platform::WriteGuarded(mode + facts::kGameModeHudStep, &step, sizeof(step));
    if (!ok)
    {
        g_passFaulted = true;
        platform::LogWarning(L"target icons: placing the icons for a view raised; each further player's icons stay where player 1's "
                             L"view placed them");
        return;
    }
    ++g_passes;
}

std::wstring Report()
{
    // The icon manager's live icons: its map's count less its free slots.
    const auto*  ui15  = static_cast<const std::uint8_t*>(Ui15());
    std::int32_t count = 0, free = 0;
    const bool   read  = ui15 != nullptr && Read(ui15 + facts::kTargetIconMapCount, count) && Read(ui15 + facts::kTargetIconMapFree, free);
    std::wstring out   = std::format(L"icons {}; on screen (the game's count) {}; owners answered {}, not the player's {}, view placements "
                                     L"{}{}; player 1's freed during a seat's run {}, frees off the game thread {}",
                                     g_placed ? L"per player" : g_placeTried ? L"NOT placed" : L"waiting for the game thread",
                                     read ? std::to_wstring(count - free) : std::wstring(L"?"), g_answered, g_mismatched, g_passes,
                                     g_passFaulted ? L" (stopped after a fault)" : L"", g_playerOneFreed,
                                     g_offThreadFrees.load(std::memory_order_relaxed));
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const Seat& s = g_seats[seat];
        out += std::format(L"; {}'s icons: {} now, made {}, gone in its pick {}, destroyed: target freed {}, target gone {}, dropped {}; "
                           L"lost by the engine {}",
                           SeatName(seat), s.held, s.made, s.gone, s.removed, s.targetGone, s.dropped, s.forgotten);
    }
    return out;
}

}  // namespace lss::world::target_icons
