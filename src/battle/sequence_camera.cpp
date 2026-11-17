#include "battle/sequence_camera.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/game_thread.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/name_text.h"
#include "platform/verified_code.h"
#include "players/field_root.h"
#include "players/seat_camera.h"
#include "players/weak_object.h"
#include "players/scopes.h"
#include "players/seats.h"

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>

namespace lss::battle::sequence_camera {

namespace {

using platform::Read;
using players::kSeatsBuilt;
using players::SeatName;

using ApplyFn     = std::int64_t (*)(void* requests, void* outPose, void* aimLeap, void* eyeLeap, std::uint32_t dt, void* owner, void* view);
using EvalFn      = char (*)(void* a1, void* outPose, void* a3, void* a4, void* a5, void* a6, std::uint32_t dt, void* view, std::uint32_t a9, void* entry);
using CutChangeFn = std::int64_t (*)(void* sequence, float time, void* params, void* pose);
using JumpToCutFn = char (*)(void* sequence, std::uint8_t kind, void* a3, void* a4);
using CutFn       = std::int64_t (*)();
using PlayFn      = std::int64_t (*)(const std::uint64_t* row, void* owner, void* holder);

constexpr int kMaxPlayers = players::scopes::kMaxPlayers;

/// A seat's copy of one sequence's per-view state.
struct View
{
    void*         sequence = nullptr;
    std::uint8_t  camera[facts::kSeqCameraHistoryBytes]{};  // the camera actor's history block
    std::uint8_t  blend[facts::kSeqBlendPoseBytes]{};       // the blend pose: the seat's camera manager's view
    std::uint8_t  cut = 0;                                  // the cut one-shot
    std::uint8_t  delta[facts::kSeqBlendOutDeltaBytes]{};   // the blend-out delta latch and delta
    bool          stopCaptured = false;                     // the blend pose taken at the seat's first evaluation after a stop
    std::uint32_t lastFrame    = 0;
};

/// One entry's active byte as each operator's tick left it this frame (index = seat, 0 player 1's).
struct Active
{
    const void*   entry = nullptr;
    std::uint32_t frame = 0;
    std::uint8_t  bytes[kMaxPlayers]{};
    bool          seen[kMaxPlayers]{};
};

// Set at install, then only read.
std::uintptr_t         g_apply        = 0;
std::atomic<bool>      g_installed{false};      // every check passed and every redirect placed (Remove may run off the
                                                // game thread)
std::uintptr_t         g_eval         = 0;
std::uintptr_t         g_cutChange    = 0;
std::uintptr_t         g_jumpToCut    = 0;
std::uintptr_t         g_cut          = 0;
std::uintptr_t         g_cutSite      = 0;
std::uintptr_t         g_primaryCut   = 0;
platform::CallRedirect g_applyCall;
platform::CallRedirect g_evalCall;
platform::CallRedirect g_cutChangeCall;
platform::CallRedirect g_jumpToCutCall;
platform::CallRedirect g_cutCall;
bool                   g_cutPlaced = false;
platform::EntryRedirect g_play;  // every shot's start, for the log

/// One shot as the log last described it: its entry, id and mode, each operator's active byte, its stop and its end.
struct Shot
{
    const void*   entry = nullptr;
    std::int32_t  id    = -1;
    std::uint8_t  mode  = 0;
    std::uint8_t  bytes[kMaxPlayers]{};
    bool          seen[kMaxPlayers]{};
    std::uint8_t  play     = 0;
    std::uint8_t  finished = 0;
    bool          here     = false;  // in the frame just walked

    bool Same(const Shot& o) const noexcept
    {
        return entry == o.entry && id == o.id && mode == o.mode && play == o.play && finished == o.finished &&
               std::memcmp(bytes, o.bytes, sizeof(bytes)) == 0 && std::memcmp(seen, o.seen, sizeof(seen)) == 0;
    }
};

// Game thread. Index = seat ([0] unused): each seat's copies and counts.
struct Seat
{
    std::array<View, 8> views;
    std::uint32_t       swaps = 0, cuts = 0, captures = 0;
    bool                loggedSwap = false, loggedCut = false;
};
std::array<Seat, kMaxPlayers> g_seats;
std::uint32_t         g_frame     = 1;
int                   g_firstSeat = -1;        // the operator that evaluated first this frame (0 player 1's, else the seat), -1 before
std::uint8_t          g_firstActive[12]{};     // its out pose's active camera weak (index, serial) and valid byte
std::array<Active, 8> g_actives;
std::uint32_t         g_seeds = 0, g_unions = 0;
std::array<Shot, 8>   g_shots;
std::atomic<std::uint32_t> g_starts{0}, g_startsOffThread{0};  // counted on any thread, read by the console's

/// The operators that must tick in a frame, as bits: player 1's (bit 0) and every seat's whose player holds a
/// member. Bit 0 alone: the game alone, nothing of this module runs.
unsigned RequiredMask() noexcept
{
    unsigned mask = 1u;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        if (players::scopes::Member(seat) != nullptr)
        {
            mask |= 1u << seat;
        }
    }
    return mask;
}

void* Sequence(const void* entry) noexcept
{
    std::int32_t pair[2]{};
    return Read(static_cast<const std::uint8_t*>(entry) + facts::kSeqEntrySequenceWeak, pair) ? players::weak_object::ObjectOf(pair)
                                                                                                : nullptr;
}

void* CameraActor(const void* sequence) noexcept
{
    void* camera = nullptr;
    return Read(static_cast<const std::uint8_t*>(sequence) + facts::kSeqCameraActor, camera) ? camera : nullptr;
}

/// The seat's camera manager's cached view (the game's own read of the primary's: the cache pointer, else the inline
/// cache, then the POV block). False if the seat has no controller.
bool SeatView(int seat, std::uint8_t (&out)[facts::kSeqBlendPoseBytes]) noexcept
{
    void*         controller = players::scopes::Controller(seat);
    std::uint8_t* manager    = nullptr;
    std::uint8_t* cache      = nullptr;
    if (controller == nullptr || !Read(static_cast<std::uint8_t*>(controller) + facts::kControllerCameraManager, manager) ||
        manager == nullptr || !Read(manager + facts::kCameraManagerCachePointer, cache))
    {
        return false;
    }
    if (cache == nullptr)
    {
        cache = manager + facts::kCameraManagerInlineCache;
    }
    return platform::CopyGuarded(out, cache + facts::kCameraCachePov, sizeof(out));
}

/// The seat's copy for `sequence`, made fresh (zero latches, the live blend pose) when new.
View& ViewFor(int seat, void* sequence) noexcept
{
    auto& views  = g_seats[seat].views;
    View* oldest = &views[0];
    for (View& view : views)
    {
        if (view.sequence == sequence)
        {
            view.lastFrame = g_frame;
            return view;
        }
        if (view.lastFrame < oldest->lastFrame)
        {
            oldest = &view;
        }
    }
    *oldest           = View{};
    oldest->sequence  = sequence;
    oldest->lastFrame = g_frame;
    platform::CopyGuarded(oldest->blend, static_cast<const std::uint8_t*>(sequence) + facts::kSeqBlendPose, sizeof(oldest->blend));
    return *oldest;
}

/// The game captured a pose into `sequence` (a cut, a jump): each seat's copy takes the seat's view, its latches
/// start again, its cut one-shot is the game's.
void Captured(void* sequence, const wchar_t* why) noexcept
{
    if (sequence == nullptr)
    {
        return;
    }
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        if (players::scopes::Member(seat) == nullptr)
        {
            continue;
        }
        View& view = ViewFor(seat, sequence);
        if (!SeatView(seat, view.blend))
        {
            platform::CopyGuarded(view.blend, static_cast<const std::uint8_t*>(sequence) + facts::kSeqBlendPose, sizeof(view.blend));
        }
        Read(static_cast<const std::uint8_t*>(sequence) + facts::kSeqCutOneShot, view.cut);
        std::memset(view.camera, 0, facts::kSeqCameraLatchBytes);
        std::memset(view.delta, 0, sizeof(view.delta));
        view.stopCaptured = false;
        ++g_seats[seat].captures;
        platform::LogTrace(L"sequence camera: {}'s copy refreshed at {} (cut one-shot {})", SeatName(seat), why, view.cut);
    }
}

/// BattleCamRequests_ApplySequenceOverride at the operator tick: every operator after the frame's first is seeded
/// with the first's active camera, so the game's tail applies the first's and its own world state (the union of the
/// two; with three players, the first's and each later one's).
std::int64_t Apply(void* requests, void* outPose, void* aimLeap, void* eyeLeap, std::uint32_t dt, void* owner, void* view)
{
    const int   seat   = players::seat_camera::TickingSeat();  // 0: player 1's operator
    auto* const active = static_cast<std::uint8_t*>(outPose) + facts::kSeqOutPoseActiveWeak;
    const bool  anyIn  = RequiredMask() != 1u;
    if (anyIn && g_firstSeat != -1 && g_firstSeat != seat && g_firstActive[8] != 0)
    {
        if (platform::WriteGuarded(active, g_firstActive, sizeof(g_firstActive)))
        {
            ++g_seeds;
        }
    }
    const std::int64_t result = reinterpret_cast<ApplyFn>(g_apply)(requests, outPose, aimLeap, eyeLeap, dt, owner, view);
    if (anyIn && g_firstSeat == -1)
    {
        g_firstSeat = seat;
        if (!platform::CopyGuarded(g_firstActive, active, sizeof(g_firstActive)))
        {
            std::memset(g_firstActive, 0, sizeof(g_firstActive));
        }
    }
    return result;
}

/// BattleCamRequest_EvalSequenceOverride for one entry: in a seat's operator's tick, with the seat's copy of the
/// camera actor's history and the sequence's blend, cut and delta fields in.
char Eval(void* a1, void* outPose, void* a3, void* a4, void* a5, void* a6, std::uint32_t dt, void* view, std::uint32_t a9, void* entry)
{
    const int seat = players::seat_camera::TickingSeat();
    if (!players::SeatBuilt(seat))
    {
        return reinterpret_cast<EvalFn>(g_eval)(a1, outPose, a3, a4, a5, a6, dt, view, a9, entry);
    }
    void* sequence = Sequence(entry);
    void* camera   = sequence != nullptr ? CameraActor(sequence) : nullptr;
    if (sequence == nullptr || camera == nullptr)
    {
        return reinterpret_cast<EvalFn>(g_eval)(a1, outPose, a3, a4, a5, a6, dt, view, a9, entry);
    }
    Seat&       s    = g_seats[seat];
    auto* const seq  = static_cast<std::uint8_t*>(sequence);
    auto* const cam  = static_cast<std::uint8_t*>(camera);
    View&       mine = ViewFor(seat, sequence);
    std::uint8_t state = 0;
    if (Read(seq + facts::kSeqPlayState, state) && state == facts::kSeqPlayStateStopped)
    {
        if (!mine.stopCaptured)
        {
            mine.stopCaptured = true;  // the game took player 1's view at the stop; the seat's copy takes its own now
            if (SeatView(seat, mine.blend))
            {
                ++s.captures;
            }
        }
    }
    else
    {
        mine.stopCaptured = false;
    }
    // The live bytes out, the seat's in; after the call the seat's back out, the live ones in.
    View live{};
    const bool held = platform::CopyGuarded(live.camera, cam + facts::kSeqCameraHistory, sizeof(live.camera)) &&
                      platform::CopyGuarded(live.blend, seq + facts::kSeqBlendPose, sizeof(live.blend)) &&
                      Read(seq + facts::kSeqCutOneShot, live.cut) &&
                      platform::CopyGuarded(live.delta, seq + facts::kSeqBlendOutDelta, sizeof(live.delta)) &&
                      platform::WriteGuarded(cam + facts::kSeqCameraHistory, mine.camera, sizeof(mine.camera)) &&
                      platform::WriteGuarded(seq + facts::kSeqBlendPose, mine.blend, sizeof(mine.blend)) &&
                      platform::WriteGuarded(seq + facts::kSeqCutOneShot, &mine.cut, sizeof(mine.cut)) &&
                      platform::WriteGuarded(seq + facts::kSeqBlendOutDelta, mine.delta, sizeof(mine.delta));
    if (!held)
    {
        platform::Log(L"sequence camera: {}'s copy could not go in; this evaluation runs on player 1's state", SeatName(seat));
        return reinterpret_cast<EvalFn>(g_eval)(a1, outPose, a3, a4, a5, a6, dt, view, a9, entry);
    }
    ++s.swaps;
    if (!s.loggedSwap)
    {
        s.loggedSwap = true;
        platform::LogDebug(L"sequence camera: {}'s operator evaluates sequence cameras on its own copy of their state", SeatName(seat));
    }
    const char result = reinterpret_cast<EvalFn>(g_eval)(a1, outPose, a3, a4, a5, a6, dt, view, a9, entry);
    platform::CopyGuarded(mine.camera, cam + facts::kSeqCameraHistory, sizeof(mine.camera));
    platform::CopyGuarded(mine.blend, seq + facts::kSeqBlendPose, sizeof(mine.blend));
    Read(seq + facts::kSeqCutOneShot, mine.cut);
    platform::CopyGuarded(mine.delta, seq + facts::kSeqBlendOutDelta, sizeof(mine.delta));
    platform::WriteGuarded(cam + facts::kSeqCameraHistory, live.camera, sizeof(live.camera));
    platform::WriteGuarded(seq + facts::kSeqBlendPose, live.blend, sizeof(live.blend));
    platform::WriteGuarded(seq + facts::kSeqCutOneShot, &live.cut, sizeof(live.cut));
    platform::WriteGuarded(seq + facts::kSeqBlendOutDelta, live.delta, sizeof(live.delta));
    return result;
}

/// AEndBattleLevelSequenceActor::ApplyCutChange (a cut starts: the game captures player 1's view): each seat's copy too.
std::int64_t CutChange(void* sequence, float time, void* params, void* pose)
{
    const std::int64_t result = reinterpret_cast<CutChangeFn>(g_cutChange)(sequence, time, params, pose);
    Captured(sequence, L"a cut");
    return result;
}

/// AEndBattleLevelSequenceActor::JumpToCut: the same.
char JumpToCut(void* sequence, std::uint8_t kind, void* a3, void* a4)
{
    const char result = reinterpret_cast<JumpToCutFn>(g_jumpToCut)(sequence, kind, a3, a4);
    Captured(sequence, L"a jump");
    return result;
}

/// The cut raised by the evaluation: in a seat's operator's tick, the game's primary-scoped cut (the seat's camera
/// manager, the primary there) instead of the first player's.
std::int64_t Cut()
{
    const int seat = players::seat_camera::TickingSeat();
    if (!g_installed.load(std::memory_order_relaxed) || !players::SeatBuilt(seat))
    {
        return reinterpret_cast<CutFn>(g_cut)();
    }
    Seat& s = g_seats[seat];
    ++s.cuts;
    if (!s.loggedCut)
    {
        s.loggedCut = true;
        platform::LogDebug(L"sequence camera: a sequence cut during {}'s camera update went to their own camera", SeatName(seat));
    }
    return reinterpret_cast<CutFn>(g_primaryCut)();
}

/// The battle system's request lists, or null outside a battle.
std::uint8_t* Lists() noexcept
{
    auto*         system = static_cast<std::uint8_t*>(players::field_root::BattleSystem());
    std::uint8_t* lists  = nullptr;
    return system != nullptr && Read(system + facts::kBattleSysCamRequests, lists) ? lists : nullptr;
}

/// Both request lists of the battle system, entry by entry.
template <class Fn>
void ForEachEntry(Fn&& fn) noexcept
{
    std::uint8_t* const lists = Lists();
    if (lists == nullptr)
    {
        return;
    }
    for (const std::uint32_t head : {facts::kSeqListOneHead, facts::kSeqListTwoHead})
    {
        std::uint8_t* entry = nullptr;
        if (!Read(lists + head, entry))
        {
            continue;
        }
        for (int guard = 0; entry != nullptr && guard < 64; ++guard)
        {
            fn(entry);
            std::uint8_t* next = nullptr;
            entry = Read(entry + facts::kSeqEntryNext, next) ? next : nullptr;
        }
    }
}

/// What a shot's mode means (research/RE-scene-like-battle-shots.md), for the log.
const wchar_t* ModeText(std::uint8_t mode) noexcept
{
    switch (mode)
    {
        case 0: return L"plays for the player it involves";
        case 1: return L"plays for every player, a command menu takes a view back";
        case 2: return L"plays for its user";
        case 3: return L"scene-like, plays for every player";
        default: return L"unknown";
    }
}

/// An actor's name, and whose member it is when a player's.
std::wstring WhoText(const void* actor)
{
    if (actor == nullptr)
    {
        return L"none";
    }
    std::uint64_t bits = 0;
    std::wstring  text = Read(static_cast<const std::uint8_t*>(actor) + facts::kObjectName, bits) ? platform::NameOrBits(bits) : L"?";
    for (int seat = 0; seat <= kSeatsBuilt; ++seat)
    {
        if (players::scopes::Member(seat) == actor)
        {
            return std::format(L"{} ({}'s member)", text, SeatName(seat));
        }
    }
    return text;
}

/// The entry whose sequence carries `id`, or null.
std::uint8_t* EntryOf(std::int32_t id) noexcept
{
    std::uint8_t* found = nullptr;
    ForEachEntry([&found, id](std::uint8_t* entry) {
        const auto*  sequence = static_cast<const std::uint8_t*>(Sequence(entry));
        std::int32_t own      = -1;
        if (found == nullptr && sequence != nullptr && Read(sequence + facts::kSeqSequenceId, own) && own == id)
        {
            found = entry;
        }
    });
    return found;
}

/// BattleCamera_PlaySequenceForOwner: every battle shot's start, logged with its row, its owner, its starter (the call's
/// return address, as IDA names it) and the mode the game gave it. The game's function runs unchanged.
std::int64_t Play(const std::uint64_t* row, void* owner, void* holder)
{
    const auto         from = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const std::int64_t id   = reinterpret_cast<PlayFn>(g_play.Original())(row, owner, holder);
    if (!platform::OnGameThread())
    {
        g_startsOffThread.fetch_add(1, std::memory_order_relaxed);
        return id;
    }
    g_starts.fetch_add(1, std::memory_order_relaxed);
    if (!platform::Enabled(platform::Level::Debug))
    {
        return id;
    }
    try
    {
        std::uint64_t rowName = 0;
        Read(row, rowName);
        const auto          shot     = static_cast<std::int32_t>(id);  // -1: no shot (no row, no sequence)
        const std::uint8_t* entry    = shot != -1 ? EntryOf(shot) : nullptr;
        const std::uint8_t* data     = nullptr;
        std::uint8_t        mode     = 0xFF;
        std::uint8_t        priority = 0xFF;
        if (entry != nullptr && Read(entry + facts::kSeqEntryMode, mode) && Read(entry + facts::kSeqEntryRow, data) && data != nullptr)
        {
            Read(data + facts::kSeqRowPriorityType, priority);
        }
        platform::LogDebug(L"battle shot: {} started by {}{} (called from 0x{:X}): id {}, priority type {}, mode {} ({})",
                           platform::NameOrBits(rowName), WhoText(owner),
                           holder != nullptr && holder != owner ? L", held by " + WhoText(holder) : std::wstring(),
                           platform::IdaAddress(from), shot, priority, mode, entry != nullptr ? ModeText(mode) : L"no entry");
    }
    catch (...)
    {
    }
    return id;
}

/// The frame just ticked: each shot's activity on every operator, its stop and its end, logged when it changes (the
/// records AfterOperatorTick kept, so only while a seat is in and the operators tick). A frame with no record (the
/// operators did not tick) changes nothing; a kept shot with no record is gone only once its entry has left the lists.
void WatchShots() noexcept
{
    if (!platform::Enabled(platform::Level::Debug) ||
        std::none_of(g_actives.begin(), g_actives.end(), [](const Active& record) { return record.entry != nullptr && record.frame == g_frame; }))
    {
        return;
    }
    for (Shot& kept : g_shots)
    {
        kept.here = false;
    }
    for (const Active& record : g_actives)
    {
        const auto* sequence = record.entry != nullptr && record.frame == g_frame ? static_cast<const std::uint8_t*>(Sequence(record.entry)) : nullptr;
        Shot        now;
        now.entry = record.entry;
        if (sequence == nullptr || !Read(sequence + facts::kSeqSequenceId, now.id))
        {
            continue;
        }
        Read(static_cast<const std::uint8_t*>(record.entry) + facts::kSeqEntryMode, now.mode);
        Read(sequence + facts::kSeqPlayState, now.play);
        Read(sequence + facts::kSeqFinished, now.finished);
        std::memcpy(now.bytes, record.bytes, sizeof(now.bytes));
        std::memcpy(now.seen, record.seen, sizeof(now.seen));
        now.here = true;
        Shot* slot = nullptr;
        for (Shot& kept : g_shots)
        {
            if (kept.entry == now.entry && kept.id == now.id)
            {
                slot = &kept;
                break;
            }
            if (slot == nullptr && kept.entry == nullptr)
            {
                slot = &kept;
            }
        }
        if (slot == nullptr)
        {
            continue;  // more shots at once than the log follows
        }
        if (!slot->Same(now))
        {
            try
            {
                std::wstring active;
                for (int seat = 0; seat < kMaxPlayers; ++seat)
                {
                    if (now.seen[seat])
                    {
                        active += std::format(L"{}{}'s camera {}", active.empty() ? L"" : L", ", SeatName(seat), now.bytes[seat] != 0 ? L"yes" : L"no");
                    }
                }
                platform::LogDebug(L"battle shot {} (mode {}): active on {}; {}{}", now.id, now.mode, active.empty() ? L"no camera read" : active,
                                   now.play == facts::kSeqPlayStateStopped ? L"stopped" : L"playing", now.finished != 0 ? L", finished" : L"");
            }
            catch (...)
            {
            }
        }
        *slot = now;
    }
    for (Shot& kept : g_shots)
    {
        if (kept.entry != nullptr && !kept.here && EntryOf(kept.id) == nullptr)
        {
            platform::LogDebug(L"battle shot {}: gone from the game's list", kept.id);
            kept = Shot{};
        }
    }
}

/// Each seat's swaps, cuts and captures; the frame seeds and the active-byte unions; the shots started.
std::wstring Reading()
{
    std::wstring text;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        const Seat& s = g_seats[seat];
        text += std::format(L"{}: swaps {}, cuts to its own view {}, captures {}; ", SeatName(seat), s.swaps, s.cuts, s.captures);
    }
    return text + std::format(L"seeds {}, unions {}; battle shots started {} ({} off the game thread){}", g_seeds, g_unions,
                              g_starts.load(std::memory_order_relaxed), g_startsOffThread.load(std::memory_order_relaxed),
                              g_play.IsInstalled() ? L"" : L" (their start is not followed)");
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto apply      = platform::FindFunction(image, facts::kSeqApply);
    const auto applySite  = image.Rebase(facts::kSeqApplyCall, 5);
    const auto eval       = platform::FindFunction(image, facts::kSeqEval);
    const auto evalSite   = image.Rebase(facts::kSeqEvalCall, 5);
    const auto cutChange  = platform::FindFunction(image, facts::kSeqCutChange);
    const auto cutSite    = image.Rebase(facts::kSeqCutChangeCall, 5);
    const auto jump       = platform::FindFunction(image, facts::kSeqJumpToCut);
    const auto jumpSite   = image.Rebase(facts::kSeqJumpToCutCall, 5);
    const auto cut        = platform::FindFunction(image, facts::kSeqCutFirstPlayer);
    const auto cutRaise   = image.Rebase(facts::kSeqCutRaiseCall, 5);
    const auto primaryCut = platform::FindFunction(image, facts::kSeqCutPrimary);
    if (!players::field_root::Installed() || !apply || !applySite || !eval || !evalSite || !cutChange || !cutSite || !jump || !jumpSite || !cut || !cutRaise || !primaryCut)
    {
        platform::Log(L"sequence camera: an engine check failed; sequence cameras keep one state for every view");
        return false;
    }
    g_apply      = *apply;
    g_eval       = *eval;
    g_cutChange  = *cutChange;
    g_jumpToCut  = *jump;
    g_cut        = *cut;
    g_cutSite    = *cutRaise;
    g_primaryCut = *primaryCut;
    const bool installed = g_evalCall.Install(*evalSite, g_eval, reinterpret_cast<std::uintptr_t>(&Eval)) &&
                           g_cutChangeCall.Install(*cutSite, g_cutChange, reinterpret_cast<std::uintptr_t>(&CutChange)) &&
                           g_jumpToCutCall.Install(*jumpSite, g_jumpToCut, reinterpret_cast<std::uintptr_t>(&JumpToCut)) &&
                           g_applyCall.Install(*applySite, g_apply, reinterpret_cast<std::uintptr_t>(&Apply));
    if (!installed)
    {
        Remove();
        platform::Log(L"sequence camera: a call could not be redirected; sequence cameras keep one state for every view");
        return false;
    }
    control::Register({.name = L"sequence-camera", .shape = control::Shape::Text,
                       .help = L"sequence cameras on each seat's own state: evaluations swapped, cuts to the seat's own view, captures; the "
                               L"world-state seeds and active-byte unions",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"sequence camera: installed; each seat's operator evaluates sequence cameras on its own copy of their state");
    g_installed.store(true, std::memory_order_relaxed);
    // The shots' start, for the log only: a single instruction swapped atomically, so any thread may be inside.
    const auto play = platform::FindFunction(image, facts::kSeqPlayForOwner);
    if (!play || !g_play.Install(*play, std::span(facts::kSeqPlayForOwnerEntry.data(), facts::kSeqPlayForOwnerFirst),
                                 reinterpret_cast<std::uintptr_t>(&Play)))
    {
        platform::Log(L"sequence camera: a battle shot's start could not be followed; its activity and end are still logged");
    }
    else
    {
        platform::Log(L"sequence camera: each battle shot's start is logged (its row, owner and mode), and while a seat is in, its "
                      L"activity on each player's camera and its end");
    }
    return true;
}

std::optional<std::uint8_t> AppliedShotMode() noexcept
{
    std::uint8_t* const lists = Lists();
    std::uint8_t        mode  = 0;
    if (lists == nullptr || !Read(lists + facts::kSeqListsAppliedMode, mode))
    {
        return std::nullopt;
    }
    return mode;
}

void Remove() noexcept
{
    g_installed.store(false, std::memory_order_relaxed);  // anything left in place passes the game's state through
    // The cut call crosses a 16-byte block: put back only from the game thread, outside the camera update (Tick's).
    if (g_cutCall.IsInstalled() && (!platform::OnGameThread() || !g_cutCall.Restore()))
    {
        platform::Log(L"sequence camera: the cut call is left in place (not on the game thread, or changed after us); it "
                      L"passes the game's cut through");
    }
    g_play.Restore();
    g_applyCall.Restore();
    g_jumpToCutCall.Restore();
    g_cutChangeCall.Restore();
    g_evalCall.Restore();
}

void Tick() noexcept
{
    WatchShots();
    ++g_frame;
    g_firstSeat = -1;
    if (!g_cutPlaced && g_cutSite != 0)
    {
        g_cutPlaced = true;
        if (!g_cutCall.InstallFromOwnThread(g_cutSite, g_cut, reinterpret_cast<std::uintptr_t>(&Cut)))
        {
            platform::Log(L"sequence camera: the cut call could not be redirected; a cut in a seat's tick cuts player 1's view");
        }
    }
}

void AfterOperatorTick(int seat) noexcept
{
    if (!g_installed.load(std::memory_order_relaxed) || seat < 0 || seat >= kMaxPlayers)  // not installed: the game keeps its one state
    {
        return;
    }
    const unsigned required = RequiredMask();
    if (required == 1u)
    {
        return;
    }
    // Eight records serve a frame; a frame with more entries than that (up to 128 exist) overwrites the oldest, and
    // the OR is then not written for the entries whose record went: the last operator's byte stands, as the game's.
    ForEachEntry([seat, required](std::uint8_t* entry) {
        std::uint8_t active = 0;
        if (!Read(entry + facts::kSeqEntryActive, active))
        {
            return;
        }
        Active* slot   = nullptr;
        Active* oldest = &g_actives[0];
        for (Active& record : g_actives)
        {
            if (record.entry == entry && record.frame == g_frame)
            {
                slot = &record;
                break;
            }
            if (record.frame < oldest->frame)
            {
                oldest = &record;
            }
        }
        if (slot == nullptr)
        {
            slot  = oldest;
            *slot = Active{};
            slot->entry = entry;
            slot->frame = g_frame;
        }
        slot->bytes[seat] = active;
        slot->seen[seat]  = true;
        unsigned     seen   = 0;
        std::uint8_t either = 0;
        for (int s = 0; s < kMaxPlayers; ++s)
        {
            if (slot->seen[s])
            {
                seen |= 1u << s;
                either |= slot->bytes[s];
            }
        }
        if ((seen & required) == required && either != active && platform::WriteGuarded(entry + facts::kSeqEntryActive, &either, sizeof(either)))
        {
            ++g_unions;
        }
    });
}

}  // namespace lss::battle::sequence_camera
