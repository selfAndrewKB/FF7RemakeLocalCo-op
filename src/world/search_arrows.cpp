#include "world/search_arrows.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/scopes.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "world/target_icons.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <type_traits>

namespace lss::world::search_arrows {

namespace {

using platform::Read;
using players::SeatName;
using players::weak_object::Weak;

using IfaceFn   = std::uint64_t (*)(void* iface);  // enter, stay
using LeaveFn   = char (*)(void* iface);
using ActorFn   = void (*)(void* actor);  // the focus clear
using LostFn    = double (*)(void* actor);
using EndPlayFn = char (*)(void* actor, std::uint32_t reason);
using SlotFn    = std::uint64_t (*)(void* slot);  // ClearAll, Tick
using BitsFn    = void (*)(void* bits, std::uint32_t value, std::int32_t count);
using AllocFn   = void* (*)(std::uint64_t size, std::uint32_t alignment);

enum Call : std::size_t
{
    Enter,
    Stay,
    Leave,
    Focus,
    Lost,
    EndPlay,
    kCalls,
};

constexpr std::array<const platform::SlotFact*, kCalls> kFacts{&facts::kHookAreaEnter,      &facts::kHookAreaStay,
                                                                &facts::kHookAreaLeave,      &facts::kHookAreaFocusClear,
                                                                &facts::kHookAreaTargetLost, &facts::kHookAreaEndPlay};
constexpr std::array<const wchar_t*, kCalls> kCallNames{L"enter", L"stay", L"leave", L"focus clear", L"re-show", L"EndPlay"};

constexpr int         kSeats   = players::scopes::kMaxPlayers;
constexpr std::size_t kRecords = 32;  // hook areas a seat holds a flag or an arrow of (a level has a handful)
using Fields                   = std::array<std::uint8_t, facts::kHookAreaFieldsBytes>;

/// A seat's copy of one hook area's flag and arrow.
struct Record
{
    void*  actor = nullptr;
    Weak   id{};
    Fields fields{};
};

struct Seat
{
    void*                         slot        = nullptr;  // its slot-55 object (the game's allocator; kept for the game's life)
    void*                         localPlayer = nullptr;
    std::int32_t                  playerIndex = -1;  // the local player's object index: its arrows' owner at +0x1E8
    std::array<Record, kRecords>  records{};
    bool                          passTicked = false;  // a HUD pass ticked its slot since the last engine tick
    bool                          slotFailed = false;  // its slot could not be made: its calls stay the game's (not retried)
    bool                          fellBack   = false;  // a call of its ran on player 1's objects (logged once)
    std::uint32_t enters = 0, leaves = 0, focus = 0, lost = 0, endPlays = 0, cleared = 0, notOwned = 0, full = 0;
    std::uint64_t passTicks = 0, gameTicks = 0;
};

// Set at install, then only read.
std::array<platform::VTableEntry, kCalls> g_slots;
std::uintptr_t                            g_slotGlobal = 0;  // &EndUISystem[55]
SlotFn                                    g_clearAll   = nullptr;
SlotFn                                    g_tick       = nullptr;
BitsFn                                    g_bitsInit   = nullptr;
AllocFn                                   g_alloc      = nullptr;
std::uintptr_t                            g_resetSite  = 0;
bool                                      g_installed  = false;

// Game thread.
platform::CallRedirect   g_resetCall;
bool                     g_resetTried = false;
bool                     g_enabled    = true;  // switch search-arrows
bool                     g_inside     = false;  // a seat's call runs now (the game's functions never nest these)
std::array<Seat, kSeats> g_seats{};
std::uint32_t            g_resets = 0, g_resetsSkipped = 0, g_slotFailures = 0;

std::atomic<DWORD> g_gameThread{0};

bool OnGameThread() noexcept
{
    return GetCurrentThreadId() == g_gameThread.load(std::memory_order_relaxed);
}

/// The slot object's arrows now: its map's elements less its free ones.
std::int32_t ArrowCount(const void* slot) noexcept
{
    std::int32_t num = 0, free = 0;
    const auto*  base = static_cast<const std::uint8_t*>(slot);
    return slot != nullptr && Read(base + facts::kSearchSlotMapNum, num) && Read(base + facts::kSearchSlotMapFreeCount, free) ? num - free
                                                                                                                             : -1;
}

bool HasCompass(const void* slot) noexcept
{
    std::int32_t index = -1;
    return slot != nullptr && Read(static_cast<const std::uint8_t*>(slot) + facts::kSearchSlotCompass, index) && index >= 0;
}

void* PlayerOneSlot() noexcept
{
    return platform::ReadPointer(reinterpret_cast<const void*>(g_slotGlobal), 0);
}

/// A slot object as the game's constructor leaves it: three empty weak pairs, state 0, an empty map (its allocation bits
/// through the game's own init, its free list at -1). Null (logged) if the allocation or the init failed.
void* MakeSlot(int seat) noexcept
{
    void* slot = nullptr;
    if (!platform::CallGuarded(g_alloc, slot, std::uint64_t{facts::kSearchSlotBytes}, std::uint32_t{0}) || slot == nullptr)
    {
        ++g_slotFailures;
        platform::LogWarning(L"search arrows: {}'s search slot could not be allocated (not retried); its hook areas use player 1's arrow",
                             SeatName(seat));
        return nullptr;
    }
    auto* bytes = static_cast<std::uint8_t*>(slot);
    std::memset(bytes, 0, facts::kSearchSlotBytes);
    const std::int32_t none = -1;
    for (const std::uint32_t weak : {0u, 8u, 16u})
    {
        std::memcpy(bytes + weak, &none, sizeof(none));
    }
    std::memcpy(bytes + facts::kSearchSlotMapFree, &none, sizeof(none));
    if (!platform::CallGuarded(g_bitsInit, static_cast<void*>(bytes + facts::kSearchSlotMapBits), std::uint32_t{0}, std::int32_t{0}))
    {
        ++g_slotFailures;
        platform::LogWarning(L"search arrows: {}'s search slot could not be set up (128 bytes left allocated, not retried); its hook "
                             L"areas use player 1's arrow", SeatName(seat));
        return nullptr;
    }
    platform::LogDebug(L"search arrows: {}'s search slot made ({})", SeatName(seat), slot);
    return slot;
}

/// The seat's record of `actor`, a stale one (its actor gone) or a free one taken for it, or null if all are held.
Record* RecordFor(Seat& seat, void* actor) noexcept
{
    Record* free = nullptr;
    for (Record& record : seat.records)
    {
        const bool stale = record.actor != nullptr && players::weak_object::Resolve(record.id) != record.actor;
        if (stale)
        {
            record = {};
        }
        if (record.actor == actor)
        {
            return &record;
        }
        if (record.actor == nullptr && free == nullptr)
        {
            free = &record;
        }
    }
    if (free == nullptr)
    {
        return nullptr;
    }
    const Weak id = players::weak_object::MakeWeak(actor);
    if (!id.Set())
    {
        return nullptr;  // no pair: the record would look stale at every call
    }
    free->actor = actor;
    free->id    = id;
    return free;
}

void* ArrowOf(const Fields& fields) noexcept
{
    void* arrow = nullptr;
    std::memcpy(&arrow, fields.data() + (facts::kHookAreaArrow - facts::kHookAreaFields), sizeof(arrow));
    return arrow;
}

/// A record's arrow that the engine no longer lists is forgotten (the game's functions dereference the pointer).
void DropDeadArrow(Fields& fields) noexcept
{
    void* arrow = ArrowOf(fields);
    if (arrow != nullptr && !players::weak_object::Live(arrow))
    {
        std::memset(fields.data() + (facts::kHookAreaArrow - facts::kHookAreaFields), 0, sizeof(void*));
    }
}

/// The actor's flag and arrow exchanged with `fields` (twice puts both back).
bool Exchange(void* actor, Fields& fields) noexcept
{
    auto*  live = static_cast<std::uint8_t*>(actor) + facts::kHookAreaFields;
    Fields held{};
    if (!platform::CopyGuarded(held.data(), live, held.size()) || !platform::WriteGuarded(live, fields.data(), fields.size()))
    {
        return false;
    }
    fields = held;
    return true;
}

/// EndUISystem[55] holds `slot` for the scope's life, then the value it held before.
class SlotIn
{
public:
    explicit SlotIn(void* slot) noexcept : slot_(slot)
    {
        auto* global = reinterpret_cast<void**>(g_slotGlobal);
        held_        = Read(global, saved_) && platform::WriteGuarded(global, &slot_, sizeof(slot_));
    }
    ~SlotIn()
    {
        if (!held_)
        {
            return;
        }
        auto* global = reinterpret_cast<void**>(g_slotGlobal);
        void* now    = nullptr;
        if (!Read(global, now) || now != slot_)
        {
            platform::LogWarning(L"search arrows: the game replaced the search slot during a seat's call; the game's new one kept");
            return;
        }
        platform::WriteGuarded(global, &saved_, sizeof(saved_));
    }
    SlotIn(const SlotIn&)            = delete;
    SlotIn& operator=(const SlotIn&) = delete;

    bool  Held() const noexcept { return held_; }
    void* Saved() const noexcept { return saved_; }

private:
    void* slot_  = nullptr;
    void* saved_ = nullptr;
    bool  held_  = false;
};

/// The seat's slot cleared with the game's ClearAll and its copies forgotten (its player left, a new local player, the
/// switch). `keepFlags` (the game mode's reset): only the arrows forgotten, as the game's reset leaves player 1's in-area
/// flags.
void ClearSeat(int index, const wchar_t* why, bool keepFlags = false) noexcept
{
    Seat& seat = g_seats[index];
    if (seat.slot != nullptr && (HasCompass(seat.slot) || ArrowCount(seat.slot) > 0))
    {
        std::uint64_t ignored = 0;
        platform::CallGuarded(g_clearAll, ignored, seat.slot);
        ++seat.cleared;
        platform::LogDebug(L"search arrows: {}'s search compass and arrows cleared ({})", SeatName(index), why);
    }
    if (!keepFlags)
    {
        seat.records.fill({});
        return;
    }
    for (Record& record : seat.records)
    {
        std::memset(record.fields.data() + (facts::kHookAreaArrow - facts::kHookAreaFields), 0, sizeof(void*));
    }
}

/// The seat whose call this is: one of its Leader or UiSlot scopes runs the game's code now and its player is in.
int CallingSeat() noexcept
{
    if (!OnGameThread() || !g_enabled || g_inside || !target_icons::Placed())
    {
        return 0;
    }
    const int seat = players::scopes::OpenPlayer(players::scopes::Leader | players::scopes::UiSlot);
    return players::SeatBuilt(seat) && g_seats[seat].localPlayer != nullptr && !g_seats[seat].slotFailed ? seat : 0;
}

/// One call of a hook-area function for a seat: its slot in EndUISystem[55] and its copy of the area's flag and arrow in
/// the actor while held; for the enter and the re-show the widgets made are owned by its local player.
class SeatCall
{
public:
    SeatCall(int seat, Call call, void* actor) noexcept : seat_(seat), call_(call), actor_(actor)
    {
        Seat& s = g_seats[seat];
        if (s.slot == nullptr)
        {
            s.slot = MakeSlot(seat);
            if (s.slot == nullptr)
            {
                s.slotFailed = true;
                return;
            }
        }
        record_ = RecordFor(s, actor);
        if (record_ == nullptr)
        {
            // A scratch copy: the call still runs on the seat's objects, its flag is not kept.
            if (s.full++ == 0)
            {
                platform::LogDebug(L"search arrows: {} holds {} hook areas (or one has no weak pair); a further one's flag is not kept",
                                   SeatName(seat), kRecords);
            }
            record_ = &scratch_;
        }
        DropDeadArrow(record_->fields);
        before_ = record_->fields;
        slotIn_.emplace(s.slot);
        if (!slotIn_->Held())
        {
            slotIn_.reset();
            return;
        }
        if (!Exchange(actor, record_->fields))
        {
            slotIn_.reset();
            return;
        }
        playerOneBefore_ = ArrowCount(slotIn_->Saved());
        if (call == Enter || call == Lost)
        {
            creating_.emplace(s.localPlayer);
            if (!creating_->Held())
            {
                platform::LogDebug(L"search arrows: {}'s {}: the owner answer is busy; its compass and arrow would be player 1's",
                                   SeatName(seat), kCallNames[call]);
            }
        }
        g_inside = true;
        held_    = true;
    }

    ~SeatCall()
    {
        if (!held_)
        {
            return;
        }
        creating_.reset();
        Seat& s = g_seats[seat_];
        if (completed_)
        {
            void* arrow = nullptr;
            Read(static_cast<const std::uint8_t*>(actor_) + facts::kHookAreaArrow, arrow);
            if ((call_ == Enter || call_ == Lost) && arrow != nullptr && arrow != ArrowOf(before_) && !OwnedBySeat(arrow, s))
            {
                // Player 1's (the creation's owner call was not answered): it would point from player 1's camera.
                ++s.notOwned;
                platform::LogWarning(L"search arrows: an arrow made for {} is not that player's; cleared", SeatName(seat_));
                platform::CallGuarded(reinterpret_cast<ActorFn>(g_slots[Focus].Original()), actor_);
            }
        }
        const std::int32_t seatArrows = ArrowCount(s.slot);
        const bool         back       = Exchange(actor_, record_->fields);
        const std::int32_t playerOne  = ArrowCount(slotIn_->Saved());
        slotIn_.reset();
        g_inside = false;
        if (!back)
        {
            platform::LogWarning(L"search arrows: hook area {}'s flag and arrow could not be put back after {}'s {}", actor_, SeatName(seat_),
                                 kCallNames[call_]);
            *record_ = {};  // it may hold player 1's: never exchanged into the actor again
            return;
        }
        if (!completed_)
        {
            return;  // the game's call faulted: put back, nothing counted
        }
        Count();
        if (record_ != &scratch_ && record_->fields == Fields{})
        {
            *record_ = {};  // out of the area, no arrow: the record is free again
        }
        platform::LogAt(call_ == Stay ? platform::Level::Trace : platform::Level::Debug,
                        L"search arrows: {}'s {} of hook area {}: in area {} -> {}, arrow {} -> {}; its arrows {}, player 1's {} -> {}",
                        SeatName(seat_), kCallNames[call_], actor_, before_[0], record_->fields[0], ArrowOf(before_), ArrowOf(record_->fields),
                        seatArrows, playerOneBefore_, playerOne);
    }

    SeatCall(const SeatCall&)            = delete;
    SeatCall& operator=(const SeatCall&) = delete;

    bool Held() const noexcept { return held_; }
    /// The game's call returned (the checks and the counts run only then).
    void Completed() noexcept { completed_ = true; }

private:
    void Count() const noexcept
    {
        Seat& s = g_seats[seat_];
        switch (call_)
        {
        case Enter:
            ++s.enters;
            break;
        case Leave:
            ++s.leaves;
            break;
        case Focus:
            ++s.focus;
            break;
        case Lost:
            ++s.lost;
            break;
        default:
            break;
        }
    }

    static bool OwnedBySeat(const void* widget, const Seat& seat) noexcept
    {
        std::int32_t owner = -2;
        return Read(static_cast<const std::uint8_t*>(widget) + facts::kWidgetOwnerPlayer, owner) && owner == seat.playerIndex;
    }

    int                                       seat_ = 0;
    Call                                      call_ = Enter;
    void*                                     actor_ = nullptr;
    Record*                                   record_ = nullptr;
    Record                                    scratch_{};
    Fields                                    before_{};
    std::int32_t                              playerOneBefore_ = -1;
    std::optional<SlotIn>                     slotIn_;
    std::optional<target_icons::CreatingAs>   creating_;
    bool                                      held_      = false;
    bool                                      completed_ = false;
};

void* ActorOfIface(void* iface) noexcept
{
    return static_cast<std::uint8_t*>(iface) - facts::kHookAreaIface;
}

/// The game's function for a seat with its objects held (a failed hold runs it on player 1's objects, as before).
template <class R, class... P, class... A>
R RunForSeat(int seat, Call call, void* actor, R (*original)(P...), A... args)
{
    SeatCall held(seat, call, actor);
    if (!held.Held() && !g_seats[seat].fellBack)
    {
        g_seats[seat].fellBack = true;
        platform::LogDebug(L"search arrows: {}'s {} ran on player 1's objects (its own could not be held; logged once)", SeatName(seat),
                           kCallNames[call]);
    }
    if constexpr (std::is_void_v<R>)
    {
        original(args...);
        held.Completed();
    }
    else
    {
        const R result = original(args...);
        held.Completed();
        return result;
    }
}

// --- The six slots ----------------------------------------------------------------------------------------------------

template <Call C>
std::uint64_t IfaceCall(void* iface)
{
    const auto original = reinterpret_cast<IfaceFn>(g_slots[C].Original());
    const int  seat     = CallingSeat();
    return seat == 0 ? original(iface) : RunForSeat(seat, C, ActorOfIface(iface), original, iface);
}

char LeaveCall(void* iface)
{
    const auto original = reinterpret_cast<LeaveFn>(g_slots[Leave].Original());
    const int  seat     = CallingSeat();
    return seat == 0 ? original(iface) : RunForSeat(seat, Leave, ActorOfIface(iface), original, iface);
}

void FocusCall(void* actor)
{
    const auto original = reinterpret_cast<ActorFn>(g_slots[Focus].Original());
    const int  seat     = CallingSeat();
    if (seat == 0)
    {
        original(actor);
        return;
    }
    RunForSeat(seat, Focus, actor, original, actor);
}

double LostCall(void* actor)
{
    const auto original = reinterpret_cast<LostFn>(g_slots[Lost].Original());
    const int  seat     = CallingSeat();
    return seat == 0 ? original(actor) : RunForSeat(seat, Lost, actor, original, actor);
}

/// The game's EndPlay (player 1's arrow of this area), then each seat's arrow of it cleared with the game's own clear on
/// the seat's slot, and the seats' copies of it forgotten.
char EndPlayCall(void* actor, std::uint32_t reason)
{
    const char result = reinterpret_cast<EndPlayFn>(g_slots[EndPlay].Original())(actor, reason);
    if (!OnGameThread() || g_inside)
    {
        return result;
    }
    const auto clear = reinterpret_cast<ActorFn>(g_slots[Focus].Original());
    for (int index = 1; index <= players::kSeatsBuilt; ++index)
    {
        Seat& seat = g_seats[index];
        for (Record& record : seat.records)
        {
            if (record.actor != actor)
            {
                continue;
            }
            DropDeadArrow(record.fields);
            if (ArrowOf(record.fields) != nullptr && seat.slot != nullptr)
            {
                const SlotIn in(seat.slot);
                if (in.Held() && Exchange(actor, record.fields))
                {
                    g_inside = true;
                    platform::CallGuarded(clear, actor);
                    g_inside = false;
                    Exchange(actor, record.fields);
                    ++seat.endPlays;
                    platform::LogDebug(L"search arrows: hook area {} ended; {}'s arrow of it cleared", actor, SeatName(index));
                }
            }
            record = {};
        }
    }
    return result;
}

/// The game mode's reset clears player 1's slot here: every seat's too.
std::uint64_t ResetClearAll(void* slot)
{
    const std::uint64_t result = reinterpret_cast<SlotFn>(g_resetCall.Original())(slot);
    if (!OnGameThread() || g_inside)
    {
        ++g_resetsSkipped;  // never expected: a reset inside a seat's call, or off the game thread
        return result;
    }
    ++g_resets;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        ClearSeat(seat, L"the game mode's reset", true);
    }
    return result;
}

const std::array<std::uintptr_t, kCalls> kWrappers{
    reinterpret_cast<std::uintptr_t>(&IfaceCall<Enter>), reinterpret_cast<std::uintptr_t>(&IfaceCall<Stay>),
    reinterpret_cast<std::uintptr_t>(&LeaveCall),        reinterpret_cast<std::uintptr_t>(&FocusCall),
    reinterpret_cast<std::uintptr_t>(&LostCall),         reinterpret_cast<std::uintptr_t>(&EndPlayCall)};

void RestoreSlots() noexcept
{
    for (auto& slot : g_slots)
    {
        slot.Restore();
    }
}

void ClearAllSeats(const wchar_t* why) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        ClearSeat(seat, why);
    }
}

void TickSlot(Seat& seat) noexcept
{
    std::uint64_t ignored = 0;
    platform::CallGuarded(g_tick, ignored, seat.slot);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto fail = [](const wchar_t* what) {
        platform::LogWarning(L"search arrows: {} is not as the research read it; a further player's hook area keeps using player 1's "
                             L"search arrow", what);
        return false;
    };
    const auto global   = image.Rebase(facts::kSearchSlotGlobal, sizeof(void*));
    const auto clearAll = platform::FindFunction(image, facts::kSearchSlotClearAll);
    const auto tick     = platform::FindFunction(image, facts::kSearchSlotTick);
    const auto bits     = platform::FindFunction(image, facts::kBitArrayInit);
    const auto alloc    = image.Rebase(facts::kGameMalloc);
    const auto reset    = image.Rebase(facts::kSearchSlotResetCall, 5);
    if (!global || !clearAll || !tick || !bits || !alloc || !reset)
    {
        return fail(L"the search slot, its functions or the allocator");
    }
    if (!players::weak_object::Installed())
    {
        return fail(L"the object array (weak pairs)");
    }
    g_slotGlobal = *global;
    g_clearAll   = reinterpret_cast<SlotFn>(*clearAll);
    g_tick       = reinterpret_cast<SlotFn>(*tick);
    g_bitsInit   = reinterpret_cast<BitsFn>(*bits);
    g_alloc      = reinterpret_cast<AllocFn>(*alloc);
    g_resetSite  = *reset;
    for (std::size_t i = 0; i < kCalls; ++i)
    {
        if (!platform::ReplaceSlot(image, *kFacts[i], kWrappers[i], g_slots[i]))
        {
            RestoreSlots();
            return fail(kFacts[i]->what);
        }
    }
    g_installed = true;
    control::Register({.name  = L"search-arrows",
                       .shape = control::Shape::OnOff,
                       .help  = L"each player's own grappling-hook search arrow, in their own view (world/search_arrows.h; on); off: the "
                                L"game's one arrow",
                       .set   = [](const control::Value& value) {
                           const bool on = std::get<bool>(value);
                           if (on != g_enabled)
                           {
                               g_enabled = on;
                               if (!on && OnGameThread())
                               {
                                   ClearAllSeats(L"the switch is off");
                               }
                               if (on)
                               {
                                   platform::Log(L"search arrows: each player's own (switch on)");
                               }
                               else
                               {
                                   platform::Log(L"search arrows: the game's one arrow (switch off)");
                               }
                           }
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled}; }});
    control::Register({.name = L"world.search-arrows", .shape = control::Shape::Text,
                       .help = L"(read only) each further player's search slot: compass and arrows now, hook areas held, and per call "
                               L"the enters, leaves, focus clears, re-shows and EndPlays handled",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"search arrows: installed; each further player's grappling-hook search arrow is their own, in their own view");
    return true;
}

void Remove() noexcept
{
    RestoreSlots();
    if (OnGameThread())
    {
        ClearAllSeats(L"the mod is unloading");
        g_resetCall.Restore();  // crosses a block: only from the game thread, outside the reset
    }
    g_installed = false;
}

void TickSeat(int seat, void* localPlayer) noexcept
{
    if (!g_installed || !players::SeatBuilt(seat))
    {
        return;
    }
    g_gameThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    Seat& s = g_seats[seat];
    if (localPlayer != s.localPlayer)
    {
        ClearSeat(seat, localPlayer == nullptr ? L"the player left" : L"a new local player");
        s.localPlayer = localPlayer;
        s.playerIndex = -1;
        if (localPlayer != nullptr)
        {
            Read(static_cast<const std::uint8_t*>(localPlayer) + facts::kObjectIndex, s.playerIndex);
        }
    }
}

void Tick() noexcept
{
    if (!g_installed)
    {
        return;
    }
    g_gameThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (!g_resetTried)
    {
        g_resetTried = true;
        if (!g_resetCall.InstallFromOwnThread(g_resetSite, reinterpret_cast<std::uintptr_t>(g_clearAll),
                                              reinterpret_cast<std::uintptr_t>(&ResetClearAll)))
        {
            platform::LogWarning(L"search arrows: the game mode reset's clear could not be redirected; a further player's arrow may stay "
                                 L"up across a reset until their next hook area");
        }
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        Seat& s = g_seats[seat];
        if (s.slot != nullptr && s.localPlayer != nullptr && g_enabled && !s.passTicked)
        {
            TickSlot(s);  // no pass of its own this frame (one view, or the mirror): the game's tick from here
            ++s.gameTicks;
        }
        s.passTicked = false;
    }
}

void TickForPass(int seat) noexcept
{
    if (!g_installed || !g_enabled || !players::SeatBuilt(seat) || !OnGameThread())
    {
        return;
    }
    Seat& s = g_seats[seat];
    if (s.slot != nullptr && s.localPlayer != nullptr)
    {
        TickSlot(s);
        s.passTicked = true;
        ++s.passTicks;
    }
}

int OwnerSeat(const void* widget) noexcept
{
    std::int32_t owner = -2;
    if (widget == nullptr || !Read(static_cast<const std::uint8_t*>(widget) + facts::kWidgetOwnerPlayer, owner))
    {
        return 0;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const Seat& s = g_seats[seat];
        if (s.slot != nullptr && s.localPlayer != nullptr && s.playerIndex >= 0 && s.playerIndex == owner)
        {
            return seat;
        }
    }
    return 0;
}

bool AnySeatArrows() noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const Seat& s = g_seats[seat];
        if (s.slot != nullptr && s.localPlayer != nullptr && (HasCompass(s.slot) || ArrowCount(s.slot) > 0))
        {
            return true;
        }
    }
    return false;
}

std::wstring Reading() noexcept
try
{
    std::wstring out = std::format(L"{} (switch {}); reset clear {}, resets {} (skipped {}); player 1's arrows {}; slot failures {}",
                                   g_installed ? L"installed" : L"not installed", g_enabled ? L"on" : L"off",
                                   g_resetCall.IsInstalled() ? L"redirected" : L"not redirected", g_resets, g_resetsSkipped,
                                   ArrowCount(PlayerOneSlot()), g_slotFailures);
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const Seat&  s    = g_seats[seat];
        std::size_t  held = 0;
        for (const Record& record : s.records)
        {
            held += record.actor != nullptr ? 1 : 0;
        }
        out += std::format(L"; {}: slot {}, compass {}, arrows {}, hook areas held {}; enters {}, leaves {}, focus clears {}, re-shows {}, "
                           L"EndPlays {}, cleared {}, not the player's {}, table full {}; ticked in its pass {}, from the game tick {}",
                           SeatName(seat), s.slot != nullptr ? L"made" : L"none", HasCompass(s.slot) ? L"up" : L"none", ArrowCount(s.slot),
                           held, s.enters, s.leaves, s.focus, s.lost, s.endPlays, s.cleared, s.notOwned, s.full, s.passTicks, s.gameTicks);
    }
    return out;
}
catch (...)
{
    return L"unread";
}

}  // namespace lss::world::search_arrows
