#include "battle/sequence_camera.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/field_root.h"
#include "players/seat_camera.h"
#include "players/weak_object.h"
#include "players/scopes.h"
#include "players/seats.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
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
bool                   g_installed    = false;  // every check passed and every redirect placed
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
/// with the first's active camera, so the game's tail applies the first's and its own world state (two players: the
/// union; three: the first's and each later one's, revisited when seat 2 is wired, plan/seats.md step 6).
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
    if (!players::SeatBuilt(seat))
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

/// Both request lists of the battle system, entry by entry.
template <class Fn>
void ForEachEntry(Fn&& fn) noexcept
{
    auto*         system = static_cast<std::uint8_t*>(players::field_root::BattleSystem());
    std::uint8_t* lists  = nullptr;
    if (system == nullptr || !Read(system + facts::kBattleSysCamRequests, lists) || lists == nullptr)
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

/// Each seat's swaps, cuts and captures; the frame seeds and the active-byte unions.
std::wstring Reading()
{
    std::wstring text;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        const Seat& s = g_seats[seat];
        text += std::format(L"{}: swaps {}, cuts to its own view {}, captures {}; ", SeatName(seat), s.swaps, s.cuts, s.captures);
    }
    return text + std::format(L"seeds {}, unions {}", g_seeds, g_unions);
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
    g_installed = true;
    return true;
}

void Remove() noexcept
{
    g_cutCall.Restore();
    g_applyCall.Restore();
    g_jumpToCutCall.Restore();
    g_cutChangeCall.Restore();
    g_evalCall.Restore();
}

void Tick() noexcept
{
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
    if (!g_installed || seat < 0 || seat >= kMaxPlayers)  // not installed: the game keeps its one state
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
