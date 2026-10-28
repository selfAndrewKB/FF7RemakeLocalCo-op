#include "audio/listeners.h"

#include "audio/listener_rules.h"
#include "control/registry.h"
#include "engine_facts.h"
#include "hud/overlay_pass.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/seqlock.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/seats.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <mutex>
#include <span>
#include <string>

namespace lss::audio::listeners {

namespace {

using listener_rules::Vec3;

constexpr int   kSeats        = players::kSeatsBuilt;            // the further players, seats 1..kSeats
constexpr int   kListeners    = kSeats + 1;                      // 0: player 1's (SEAD's own), then each seat's copy
constexpr int   kColumns      = hud::overlay_pass::kMaxColumns;  // the engine listeners read: one per column
constexpr auto  kSummaryEvery = std::chrono::seconds(10);
constexpr float kMetres       = 0.01f;
constexpr float kFreshCm      = 100.0f;  // a seat's engine listener this near its controller's own follows its camera
constexpr int   kStaleTicks   = 8;       // ticks it may differ (a camera cut lands a tick apart) before the seat goes off
constexpr std::size_t kChoiceBits = 12;  // 4096 remembered choices; a few hundred live SEAD objects at most
constexpr std::size_t kChoiceProbe = 8;
constexpr int   kSnapshotFrames = 4;     // SEAD frames a seat's listener may be set from the last good push read
constexpr float kSamePlaceCm    = 500.0f;  // the push's entry at a seat's column this near the game thread's check is its
constexpr float kInfinite     = std::numeric_limits<float>::infinity();

using PushFn       = std::uint64_t (*)(void* device);
using AudibleFn    = std::uint64_t (*)(void* object);
using UpdateFn     = std::uint64_t (*)(void* object, float seconds);
using VelocityFn   = void (*)();
using SetFn        = std::int32_t (*)(void* listener, float px, float py, float pz, float fx, float fy, float fz, float ux,
                                   float uy, float uz);
using SpeedFn      = void (*)(void* listener);
using InitFn       = std::uint8_t (*)(void* source, void* waveInstance);
using ClipFn       = std::uint8_t (*)(const void* character, float distance);
using AnalyzeFn    = void (*)(void* record, void* analysis);
using NotifyFn     = void (*)(void* notify, void* mesh, void* animation, void* extra);
using EventFn      = std::uint8_t (*)(void* callback, void* out, const std::int32_t* event);
using ConvertFn    = float* (*)(float* out, const float* in);
using ListenerAtFn = void (*)(void* controller, float* location, float* front, float* right);

// The SEAD sound classes, in the order of their slots below. The first kChosenKinds take the nearest listener; the
// area sounds are measured from player 1's (their test measures a shape's point nearest the listener: a choice would
// need a trial per listener) and are counted only.
enum Kind : int
{
    kMoving,   // DynamicSound: every spatialized engine sound
    kPlaced,   // PointSound
    kWedge,    // SphericalWedgeSound
    kPolygon,  // PolygonSound (an area)
    kSector,   // CylindricalSectorSound (an area)
    kKinds
};
constexpr int kChosenKinds = kPolygon;
constexpr const wchar_t* kKindNames[kKinds] = {L"moving", L"placed", L"wedge", L"polygon area", L"sector area"};
constexpr const platform::SlotFact* kAudibleSlots[kKinds] = {&facts::kSeadDynamicAudibleSlot, &facts::kSeadPointAudibleSlot,
                                                             &facts::kSeadWedgeAudibleSlot, &facts::kSeadPolygonAudibleSlot,
                                                             &facts::kSeadSectorAudibleSlot};
constexpr const platform::SlotFact* kUpdateSlots[kChosenKinds] = {&facts::kSeadDynamicUpdateSlot,
                                                                  &facts::kSeadPointUpdateSlot, &facts::kSeadWedgeUpdateSlot};
constexpr std::uint32_t kSources[kKinds] = {facts::kSeadDynamicSource, facts::kSeadPointSource, facts::kSeadPointSource,
                                            facts::kSeadObjectMeasured, facts::kSeadObjectMeasured};

// The hooks whose thread is noted once.
enum Hook : int
{
    kPushHook,
    kAudibleHook,
    kVelocityHook,
    kInitHook,
    kClipHook,
    kAnalyzeHook,
    kNotifyHook,
    kEventHook,
    kHooks
};
constexpr const wchar_t* kHookNames[kHooks] = {L"the listener push",       L"the sound classes' audibility tests",
                                               L"the listeners' SEAD frame", L"the sound start",
                                               L"AutoSe's distance clip",  L"AutoSe's frame analysis",
                                               L"the foot sound notify",   L"AutoSe's event sounds"};

// The audio thread's view after each push: the engine's listener count; per column its position and the front and up
// the push would send SEAD for it; the position the push sent; whether that was the override. NaN: not read.
enum ListenerField : std::size_t
{
    kCount,
    kColumnX,  // column c: position at kColumnX + kPerColumn c, then front, then up
    kPerColumn      = 9,
    kSentX          = kColumnX + kPerColumn * kColumns,
    kOverride       = kSentX + 3,
    kListenerFields
};
constexpr std::size_t kFrontAt = 3;
constexpr std::size_t kUpAt    = 6;
static_assert(kColumns == 3, "the Trace line names three engine listeners");
// The latest sound another player's listener would have heard but the chosen one cut (SEAD thread).
enum ExampleField : std::size_t
{
    kExampleKind,
    kExampleOwner,   // the listener that would have heard it
    kExampleChosen,  // the listener it was measured from
    kExampleRange,
    kExampleToChosen,
    kExampleToOwner,
    kExampleX,
    kExampleFields = kExampleX + 3
};

platform::SeqFloats<kListenerFields> g_listeners;
platform::SeqFloats<kExampleFields>  g_example;
// Each seat's listener place as the game thread checked it this tick (NaN: off), for the SEAD thread: the audio side's
// entry at the seat's column trails it, and after a column change still holds the previous view's place for a moment.
platform::SeqFloats<3 * kListeners>  g_checked;

// Placed once at install on the game thread, before the hooks that read them go live.
platform::EntryRedirect                         g_getter;
platform::VTableEntry                           g_pushSlot, g_initSlot, g_notifySlot, g_eventSlot;
std::array<platform::VTableEntry, kKinds>       g_audibleSlots;
std::array<platform::VTableEntry, kChosenKinds> g_updateSlots;
platform::CallRedirect                          g_velocityCall, g_clipCall, g_analyzeCall;
const void* const*                              g_listenerGlobal = nullptr;  // live address of SEAD's listener pointer
ConvertFn const*                                g_converter      = nullptr;  // live address of AutoSe's converter pointer
SetFn                                           g_set            = nullptr;
SpeedFn                                         g_speed          = nullptr;
std::atomic<bool>                               g_installed{false};
// The levers that may read another player's listener (cleared first at remove). Each kind's choice needs its test and
// its update both placed: an update must run from the listener its test chose.
std::atomic<bool>                               g_seatsAllowed{false};
std::array<std::atomic<bool>, kChosenKinds>     g_paired{};
std::atomic<bool>                               g_autoSeAllowed{false};
// A hook could not be put back at remove: the updates still ours answer as the game's from then on.
std::atomic<bool>                               g_leftBehind{false};

std::atomic<void*> g_device{nullptr};  // the first audio device whose push ran (the game's one SEAD device)
std::array<std::atomic<DWORD>, kHooks> g_threads{};
std::array<std::atomic<bool>, kHooks>  g_otherThread{};  // a hook ran on a second thread
std::atomic<DWORD>                     g_gameThread{0};
// Each seat's column while its listener is on (-1: off), written by the game thread, read by the SEAD thread.
std::array<std::atomic<int>, kListeners> g_seatColumn{};

// The answer of the getter on this thread while the mod runs one of the game's functions for another listener.
thread_local const void* t_listener = nullptr;

/// Runs the game's code inside it from `listener` (null: the game's own), then restores the thread's previous answer.
class FromListener
{
public:
    explicit FromListener(const void* listener) noexcept : saved_(t_listener) { t_listener = listener; }
    ~FromListener() { t_listener = saved_; }
    FromListener(const FromListener&)            = delete;
    FromListener& operator=(const FromListener&) = delete;

private:
    const void* saved_;
};

// SEAD's thread only: each seat's listener, a copy of the game's object set every SEAD frame; whether it was set this
// frame; each sound's listener, chosen in its test and used again in its update.
struct alignas(16) ListenerObject
{
    std::array<std::byte, facts::kSeadListenerSize> bytes{};
};
std::array<ListenerObject, kListeners> g_copies{};
std::array<bool, kListeners>           g_copied{};
std::array<bool, kListeners>           g_on{};
std::array<std::atomic<bool>, kListeners> g_onSeen{};  // g_on for the game thread's log
listener_rules::ChoiceTable<kChoiceBits, kChoiceProbe> g_choices;
std::uint32_t                          g_frame = 1;      // SEAD frames, counted where the seats' listeners are set
std::array<float, kListenerFields>     g_lastSeen{};     // the last push read that succeeded
int                                    g_lastSeenAge = kSnapshotFrames + 1;
std::array<float, 3 * kListeners>      g_lastChecked{};  // the last check read that succeeded

struct KindCounts
{
    std::atomic<unsigned>                      checked{0};
    std::atomic<unsigned>                      relative{0};  // listener-relative moving sounds (player 1's mix)
    std::array<std::atomic<unsigned>, kListeners> measured{};  // measured from listener k
    std::array<std::atomic<unsigned>, kListeners> heard{};     //   ... and heard
    std::array<std::atomic<unsigned>, kListeners> missed{};    // cut by the chosen listener, within listener k's reach
};
std::array<KindCounts, kKinds> g_kinds;

struct Counts
{
    std::atomic<unsigned>                         pushes{0};
    std::atomic<unsigned>                         otherDevicePushes{0};  // a push of another device, not read
    std::atomic<unsigned>                         overrides{0};          // pushes that sent the override
    std::atomic<unsigned>                         setFailures{0};        // the game's setter refused a seat's values
    std::array<std::atomic<unsigned>, kListeners> updates{};             // sound updates run from seat k's listener
    std::atomic<unsigned>                         untested{0};           // updates whose sound was not tested this frame
    std::atomic<unsigned>                         tableFull{0};          // choices refused for want of an entry
    std::atomic<unsigned>                         starts{0};
    std::atomic<unsigned>                         examples{0};
    std::atomic<unsigned>                         clipReads{0};
    std::atomic<unsigned>                         clipCuts{0};      // beyond player 1's listener
    std::array<std::atomic<unsigned>, kListeners> clipKept{};       //   ... kept: within seat k's
    std::atomic<unsigned>                         analyses{0};
    std::array<std::atomic<unsigned>, kListeners> analysisFrom{};   // measured from seat k's listener (nearest)
    std::atomic<unsigned>                         notifySteps{0};
    std::atomic<unsigned>                         autoSteps{0};
    std::atomic<unsigned>                         autoOther{0};
};
Counts g_counts;

// Game thread.
std::array<void*, kListeners>         g_controllers{};
std::array<Vec3, kListeners>          g_at{};       // each listener's place this tick (0: the engine's first)
std::array<bool, kListeners>          g_live{};     //   ... and whether it is on
std::array<int, kListeners>           g_stale{};    // ticks a seat's engine listener has not followed its camera
std::array<int, kListeners>           g_columnSeen{};  // each seat's column last tick (-1 none)
std::array<int, kListeners>           g_stateLogged{};
std::array<int, kListeners>           g_onLogged{};
int                                   g_gameCountLogged = -1;
std::array<bool, kHooks>              g_threadLogged{};
std::array<bool, kHooks>              g_otherLogged{};
std::chrono::steady_clock::time_point g_windowStart{};
unsigned                              g_exampleSeen = 0;
// The window's largest gaps, cm: what the push sent SEAD against entry 0; each seat's entry against its controller's
// own listener; each mirror column's entry against entry 0. -1: not read this window.
float                                 g_sentGap = -1.0f;
std::array<float, kListeners>         g_controllerGap{};
std::array<float, kColumns>           g_mirrorGap{};

std::mutex   g_readingLock;  // the reading (written on the game thread; the console reads it too)
std::wstring g_reading = L"-";

void Note(Hook hook) noexcept
{
    auto& slot = g_threads[static_cast<std::size_t>(hook)];
    const DWORD here = GetCurrentThreadId();
    DWORD       seen = slot.load(std::memory_order_relaxed);
    if (seen == 0 && slot.compare_exchange_strong(seen, here, std::memory_order_relaxed))
    {
        return;
    }
    if (seen != here)
    {
        g_otherThread[static_cast<std::size_t>(hook)].store(true, std::memory_order_relaxed);
    }
}

bool OnThreadOf(Hook hook) noexcept
{
    return g_threads[static_cast<std::size_t>(hook)].load(std::memory_order_relaxed) == GetCurrentThreadId();
}

bool OnGameThread() noexcept
{
    return g_gameThread.load(std::memory_order_relaxed) == GetCurrentThreadId();
}

unsigned Take(std::atomic<unsigned>& counter) noexcept
{
    return counter.exchange(0, std::memory_order_relaxed);
}

Vec3 Read3(const void* base, std::uint32_t offset) noexcept
{
    Vec3 out{};
    std::memcpy(out.data(), static_cast<const std::uint8_t*>(base) + offset, sizeof(out));
    return out;
}

Vec3 Read3Guarded(const void* base, std::uint32_t offset, bool& ok) noexcept
{
    Vec3 out{};
    ok = base != nullptr && platform::CopyGuarded(out.data(), static_cast<const std::uint8_t*>(base) + offset, sizeof(out));
    return out;
}

template <std::size_t N>
Vec3 At(const std::array<float, N>& seen, std::size_t index) noexcept
{
    return {seen[index], seen[index + 1], seen[index + 2]};
}

bool Valid(const Vec3& at) noexcept
{
    return std::isfinite(at[0]) && std::isfinite(at[1]) && std::isfinite(at[2]);
}

/// SEAD's listener singleton, as the game's getter reads it (created and destroyed on SEAD's thread at its start and
/// end only).
const void* Singleton() noexcept
{
    return *static_cast<const void* const volatile*>(g_listenerGlobal);
}

/// The game's SeadLayListener_Get, replaced in full (it is `mov rax, [g]; ret`): this thread's listener while the mod
/// runs the game's code for another player, the singleton otherwise. Any thread, every SEAD frame per sound: no more.
const void* Getter() noexcept
{
    const void* const own = t_listener;
    return own != nullptr ? own : Singleton();
}

/// A listener object for the game's AutoSe reads, which take only its position (the getter's +0x30): a seat's place.
struct PlaceOnly
{
    ListenerObject object{};

    explicit PlaceOnly(const Vec3& at) noexcept
    {
        const float place[4] = {at[0], at[1], at[2], 1.0f};
        std::memcpy(object.bytes.data() + facts::kSeadListenerPosition, place, sizeof(place));
    }
    const void* Get() const noexcept { return object.bytes.data(); }
};

/// A UE position in AutoSe's space, by the game's own converter (its host copy if the pointer is unset).
Vec3 ToAutoSe(const Vec3& ue) noexcept
{
    const ConvertFn convert = g_converter != nullptr ? *g_converter : nullptr;
    if (convert == nullptr)
    {
        return listener_rules::UeCmToAutoSe(ue);
    }
    float out[3] = {};
    convert(out, ue.data());
    return {out[0], out[1], out[2]};
}

/// Audio thread: the game's push, then the engine's listeners as the push reads them (each column's place, and the
/// front and up the push would send SEAD for it) and what it sent. Only the first device's pushes are read (the push
/// itself works for the engine's main device only).
std::uint64_t Push(void* device) noexcept
{
    const auto result = reinterpret_cast<PushFn>(g_pushSlot.Original())(device);
    Note(kPushHook);
    void* first = nullptr;
    if (!g_device.compare_exchange_strong(first, device, std::memory_order_relaxed) && first != device)
    {
        g_counts.otherDevicePushes.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    if (!OnThreadOf(kPushHook))
    {
        return result;  // the snapshot has one writer, the push's first thread (a second is logged)
    }
    g_counts.pushes.fetch_add(1, std::memory_order_relaxed);
    std::array<float, kListenerFields> out{};
    out.fill(std::nanf(""));
    const auto* data  = static_cast<const std::uint8_t*>(platform::ReadPointer(device, facts::kAudioDeviceListeners));
    std::int32_t count = 0;
    if (data != nullptr)
    {
        platform::Read(static_cast<const std::uint8_t*>(device) + facts::kAudioDeviceListenersNum, count);
    }
    out[kCount] = static_cast<float>(count);
    for (std::int32_t i = 0; i < kColumns && i < count; ++i)
    {
        const auto* element = data + static_cast<std::size_t>(facts::kAudioDeviceListenerStride) * i;
        float       q[4]    = {};
        bool        ok      = false;
        const Vec3  at      = Read3Guarded(element, facts::kTransformTranslation, ok);
        if (!ok || !platform::CopyGuarded(q, element + facts::kTransformRotation, sizeof(q)))
        {
            continue;
        }
        const Vec3        front = listener_rules::SeadFront(q);
        const Vec3        up    = listener_rules::SeadUp(q);
        const std::size_t base  = kColumnX + kPerColumn * static_cast<std::size_t>(i);
        std::copy(at.begin(), at.end(), out.begin() + base);
        std::copy(front.begin(), front.end(), out.begin() + base + kFrontAt);
        std::copy(up.begin(), up.end(), out.begin() + base + kUpAt);
    }
    bool       ok   = false;
    const Vec3 sent = Read3Guarded(device, facts::kSeadDeviceSentPosition, ok);
    if (ok)
    {
        std::copy(sent.begin(), sent.end(), out.begin() + kSentX);
    }
    std::uint8_t override = 0;
    out[kOverride]        = platform::Read(static_cast<const std::uint8_t*>(device) + facts::kSeadDeviceOverride, override)
                                ? static_cast<float>(override != 0)
                                : 0.0f;
    if (override != 0)
    {
        g_counts.overrides.fetch_add(1, std::memory_order_relaxed);
    }
    g_listeners.Publish(out);
    return result;
}

/// SEAD's thread, after the game's own velocity update of its listener: each seat's listener set by the game's setter
/// from the engine's listener at the seat's column, then given its velocity by the game's update, as SEAD's command
/// processing and frame do for the singleton. A seat that is off, or whose values the setter refuses, is off for the
/// frame (its sounds go back to the nearest listener that is on). A push read that crosses a publish uses the last good
/// one, up to kSnapshotFrames old, so a seat does not drop out for one frame.
void SetSeatListeners() noexcept
{
    const void* const one = Singleton();
    std::array<float, kListenerFields> fresh{};
    if (g_listeners.Read(fresh))
    {
        g_lastSeen    = fresh;
        g_lastSeenAge = 0;
    }
    else if (g_lastSeenAge <= kSnapshotFrames)
    {
        ++g_lastSeenAge;
    }
    const auto& seen = g_lastSeen;
    const bool  read = g_seatsAllowed.load(std::memory_order_relaxed) && one != nullptr && g_lastSeenAge <= kSnapshotFrames;
    std::array<float, 3 * kListeners> checked{};
    if (g_checked.Read(checked))
    {
        g_lastChecked = checked;
    }
    for (int seat = 1; seat <= kSeats; ++seat)
    {
        const auto i      = static_cast<std::size_t>(seat);
        const int  column = g_seatColumn[i].load(std::memory_order_relaxed);
        bool       on     = false;
        if (read && column >= 1 && column < kColumns && static_cast<float>(column) < seen[kCount])
        {
            const std::size_t base  = kColumnX + kPerColumn * static_cast<std::size_t>(column);
            const Vec3        at    = At(seen, base);
            const Vec3        front = At(seen, base + kFrontAt);
            const Vec3        up    = At(seen, base + kUpAt);
            const Vec3 place = At(g_lastChecked, 3 * i);
            if (Valid(at) && Valid(front) && Valid(up) && Valid(place) &&
                listener_rules::Distance(at, place) <= kSamePlaceCm)
            {
                void* const copy = g_copies[i].bytes.data();
                if (!g_copied[i])
                {
                    std::memcpy(copy, one, facts::kSeadListenerSize);  // its vtable and its identity matrix
                    g_copied[i] = true;
                }
                on = g_set(copy, at[0], at[1], at[2], front[0], front[1], front[2], up[0], up[1], up[2]) == 0;
                if (on)
                {
                    g_speed(copy);
                }
                else
                {
                    g_counts.setFailures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        g_on[i] = on;
        g_onSeen[i].store(on, std::memory_order_relaxed);
    }
}

void Velocity() noexcept
{
    reinterpret_cast<VelocityFn>(g_velocityCall.Original())();
    Note(kVelocityHook);
    if (OnThreadOf(kVelocityHook))
    {
        ++g_frame;
        SetSeatListeners();
    }
}

/// SEAD's thread: listener k's object, if it is on this frame (0 is the singleton: always, while it exists).
const void* ListenerOf(int k) noexcept
{
    if (k == 0)
    {
        return Singleton();
    }
    return k > 0 && k < kListeners && g_on[static_cast<std::size_t>(k)] ? g_copies[static_cast<std::size_t>(k)].bytes.data()
                                                                       : nullptr;
}

/// SEAD's thread: each listener's distance to `source` (infinite for a listener that is off).
std::array<float, kListeners> Distances(const Vec3& source) noexcept
{
    std::array<float, kListeners> distances{};
    for (int k = 0; k < kListeners; ++k)
    {
        const void* const listener                 = ListenerOf(k);
        distances[static_cast<std::size_t>(k)] = listener != nullptr
                                                     ? listener_rules::Distance(Read3(listener, facts::kSeadListenerPosition), source)
                                                     : kInfinite;
    }
    return distances;
}

/// On SEAD's frame thread (the one that sets the seats' listeners): the choices and the copies are its alone.
bool OnSeadFrame() noexcept
{
    return OnThreadOf(kVelocityHook);
}

/// After the game's test of one sound: its counts, and the sounds the chosen listener cut that another would hear.
void Count(Kind kind, const void* object, int chosen, bool here) noexcept
{
    KindCounts& counts = g_kinds[kind];
    const auto  c      = static_cast<std::size_t>(chosen);
    std::uint8_t heard = 0;
    float        range = 0.0f;
    std::memcpy(&heard, static_cast<const std::uint8_t*>(object) + facts::kSeadObjectAudible, 1);
    std::memcpy(&range, static_cast<const std::uint8_t*>(object) + facts::kSeadObjectRange, sizeof(range));
    counts.measured[c].fetch_add(1, std::memory_order_relaxed);
    if (heard != 0)
    {
        counts.heard[c].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!here || range <= 0.0f)
    {
        return;
    }
    const Vec3 source    = Read3(object, kSources[kind]);
    const auto distances = Distances(source);
    for (int k = 0; k < kListeners; ++k)
    {
        const float to = distances[static_cast<std::size_t>(k)];
        if (k == chosen || !listener_rules::Within(range, to))
        {
            continue;
        }
        counts.missed[static_cast<std::size_t>(k)].fetch_add(1, std::memory_order_relaxed);
        g_example.Publish({static_cast<float>(kind), static_cast<float>(k), static_cast<float>(chosen), range,
                           distances[c], to, source[0], source[1], source[2]});
        g_counts.examples.fetch_add(1, std::memory_order_relaxed);
    }
}

/// A sound's audibility test, from the nearest player's listener for the kinds that choose (the game's own test either
/// way). Its choice is kept for its update.
template <int K>
std::uint64_t Audible(void* object) noexcept
{
    const auto test = reinterpret_cast<AudibleFn>(g_audibleSlots[K].Original());
    Note(kAudibleHook);
    KindCounts& counts = g_kinds[K];
    counts.checked.fetch_add(1, std::memory_order_relaxed);
    std::uint8_t relative = 0;  // a moving sound's field only
    if constexpr (K == kMoving)
    {
        std::memcpy(&relative, static_cast<const std::uint8_t*>(object) + facts::kSeadDynamicRelative, 1);
    }
    const bool sead = OnSeadFrame();
    const bool here = sead && g_seatsAllowed.load(std::memory_order_relaxed);
    int        chosen = 0;
    if (relative == 1)
    {
        counts.relative.fetch_add(1, std::memory_order_relaxed);
    }
    else if constexpr (K < kChosenKinds)
    {
        if (here && g_paired[K].load(std::memory_order_relaxed))
        {
            float range = 0.0f;
            std::memcpy(&range, static_cast<const std::uint8_t*>(object) + facts::kSeadObjectRange, sizeof(range));
            chosen = listener_rules::ChooseNearest(g_choices.Held(object, g_frame), Distances(Read3(object, kSources[K])),
                                                   range);
        }
    }
    // Every test on SEAD's frame thread records its listener (player 1's too), so the update that follows in this
    // frame runs from it, whatever the mod did before.
    if (sead && K < kChosenKinds && !g_choices.Record(object, chosen, g_frame))
    {
        chosen = 0;
        g_counts.tableFull.fetch_add(1, std::memory_order_relaxed);
    }
    if (relative == 1)
    {
        return test(object);  // relative to player 1's camera, its update too
    }
    std::uint64_t result = 0;
    if (chosen == 0)
    {
        result = test(object);
    }
    else
    {
        const FromListener from(ListenerOf(chosen));
        result = test(object);
    }
    Count(static_cast<Kind>(K), object, chosen, here);
    return result;
}

constexpr AudibleFn kAudibleHooks[kKinds] = {&Audible<kMoving>, &Audible<kPlaced>, &Audible<kWedge>, &Audible<kPolygon>,
                                             &Audible<kSector>};

/// A sound's update (doppler, gain, pan, the voice's start and stop), from the listener its test chose.
template <int K>
std::uint64_t Update(void* object, float seconds) noexcept
{
    const auto update = reinterpret_cast<UpdateFn>(g_updateSlots[K].Original());
    if (!OnSeadFrame() || g_leftBehind.load(std::memory_order_relaxed))
    {
        return update(object, seconds);
    }
    // The frame's own record, not the switches: a test that chose a seat's listener in this frame is followed by an
    // update from it even if the mod is removed between the two loops.
    const int k = g_choices.ChosenIn(object, g_frame);
    if (k == listener_rules::kNoChoice)
    {
        g_counts.untested.fetch_add(1, std::memory_order_relaxed);
    }
    const void* const from = k > 0 ? ListenerOf(k) : nullptr;
    if (from == nullptr)
    {
        return update(object, seconds);
    }
    g_counts.updates[static_cast<std::size_t>(k)].fetch_add(1, std::memory_order_relaxed);
    const FromListener use(from);
    return update(object, seconds);
}

constexpr UpdateFn kUpdateHooks[kChosenKinds] = {&Update<kMoving>, &Update<kPlaced>, &Update<kWedge>};

std::uint8_t Start(void* source, void* waveInstance) noexcept
{
    const auto result = reinterpret_cast<InitFn>(g_initSlot.Original())(source, waveInstance);
    Note(kInitHook);
    g_counts.starts.fetch_add(1, std::memory_order_relaxed);
    return result;
}

/// Game thread: whether AutoSe may be answered from the seats' listeners now.
bool AutoSeHere() noexcept
{
    return g_autoSeAllowed.load(std::memory_order_relaxed) && OnGameThread();
}

/// AutoSe's distance clip: the game's answer from player 1's listener; a character it cuts is kept when the game's
/// clip, run from a seat's listener, keeps it. Beyond only when beyond every player.
std::uint8_t Clip(const void* character, float distance) noexcept
{
    const auto clip   = reinterpret_cast<ClipFn>(g_clipCall.Original());
    const auto beyond = clip(character, distance);
    Note(kClipHook);
    g_counts.clipReads.fetch_add(1, std::memory_order_relaxed);
    if (beyond == 0)
    {
        return beyond;
    }
    g_counts.clipCuts.fetch_add(1, std::memory_order_relaxed);
    if (!AutoSeHere())
    {
        return beyond;
    }
    for (int seat = 1; seat <= kSeats; ++seat)
    {
        const auto i = static_cast<std::size_t>(seat);
        if (!g_live[i])
        {
            continue;
        }
        const PlaceOnly    place(g_at[i]);
        const FromListener from(place.Get());
        if (clip(character, distance) == 0)
        {
            g_counts.clipKept[i].fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
    }
    return beyond;
}

/// AutoSe's frame analysis (its rustle and swing range is compared with the distance it stores): measured from the
/// nearest player's listener to the point the game measures. Frames the game does not measure are passed through.
void Analyze(void* record, void* analysis) noexcept
{
    const auto analyze = reinterpret_cast<AnalyzeFn>(g_analyzeCall.Original());
    Note(kAnalyzeHook);
    const void*  frame = platform::ReadPointer(analysis, facts::kAutoSeAnalysisFrame);
    std::uint8_t flags = 0;
    bool         ok    = false;
    const Vec3   at    = frame != nullptr ? Read3Guarded(frame, facts::kAutoSeFramePoint, ok) : Vec3{};
    if (!ok || !platform::Read(static_cast<const std::uint8_t*>(frame) + facts::kAutoSeFrameFlags, flags) ||
        (flags & facts::kAutoSeFrameUnmeasured) != 0)
    {
        analyze(record, analysis);
        return;
    }
    g_counts.analyses.fetch_add(1, std::memory_order_relaxed);
    int nearest = 0;
    if (AutoSeHere() && g_live[0])
    {
        float best = listener_rules::Distance(at, ToAutoSe(g_at[0]));
        for (int seat = 1; seat <= kSeats; ++seat)
        {
            const auto i = static_cast<std::size_t>(seat);
            if (!g_live[i])
            {
                continue;
            }
            const float to = listener_rules::Distance(at, ToAutoSe(g_at[i]));
            if (to < best)
            {
                best    = to;
                nearest = seat;
            }
        }
    }
    if (nearest == 0)
    {
        analyze(record, analysis);
        return;
    }
    g_counts.analysisFrom[static_cast<std::size_t>(nearest)].fetch_add(1, std::memory_order_relaxed);
    const PlaceOnly    place(g_at[static_cast<std::size_t>(nearest)]);
    const FromListener from(place.Get());
    analyze(record, analysis);
}

void FootNotify(void* notify, void* mesh, void* animation, void* extra) noexcept
{
    reinterpret_cast<NotifyFn>(g_notifySlot.Original())(notify, mesh, animation, extra);
    Note(kNotifyHook);
    g_counts.notifySteps.fetch_add(1, std::memory_order_relaxed);
}

std::uint8_t SoundForEvent(void* callback, void* out, const std::int32_t* event) noexcept
{
    std::int32_t type = 0;
    platform::Read(event, type);
    const auto result = reinterpret_cast<EventFn>(g_eventSlot.Original())(callback, out, event);
    Note(kEventHook);
    (type >= facts::kAutoSeStepTypeFirst && type <= facts::kAutoSeStepTypeLast ? g_counts.autoSteps : g_counts.autoOther)
        .fetch_add(1, std::memory_order_relaxed);
    return result;
}

/// The controller's own listener position (the call the viewport's Draw makes per view), or false.
bool ControllerListener(void* controller, Vec3& out) noexcept
{
    const void* table = platform::ReadPointer(controller, 0);
    const auto  fn    = reinterpret_cast<ListenerAtFn>(platform::ReadPointer(table, facts::kControllerAudioListenerSlot));
    float       front[3] = {};
    float       right[3] = {};
    return fn != nullptr && platform::CallGuarded(fn, controller, out.data(), front, right);
}

float Widen(float gap, float value) noexcept
{
    return std::max(gap, value);
}

std::wstring Metres(float cm)
{
    return cm < 0.0f || !std::isfinite(cm) ? std::wstring(L"-") : std::format(L"{:.1f} m", cm * kMetres);
}

std::wstring ListenerName(int k)
{
    return k == 0 ? std::wstring(L"player 1") : std::wstring(players::SeatName(k));
}

/// The game-thread listeners' count and entries (the viewport's SetListener copy), or -1.
int GameListeners(std::array<Vec3, kColumns>& entries) noexcept
{
    void* const   device = g_device.load(std::memory_order_relaxed);
    const void*   data   = platform::ReadPointer(device, facts::kAudioDeviceGameListeners);
    std::int32_t  count  = -1;
    if (data == nullptr ||
        !platform::Read(static_cast<const std::uint8_t*>(device) + facts::kAudioDeviceGameListenersNum, count))
    {
        return -1;
    }
    for (std::int32_t i = 0; i < kColumns && i < count; ++i)
    {
        bool ok    = false;
        entries[static_cast<std::size_t>(i)] =
            Read3Guarded(static_cast<const std::uint8_t*>(data) + static_cast<std::size_t>(facts::kAudioDeviceGameListenerStride) * i,
                         facts::kTransformTranslation, ok);
        if (!ok)
        {
            entries[static_cast<std::size_t>(i)].fill(std::nanf(""));  // never valid, never widens a window's largest
        }
    }
    return count;
}

void LogThreads() noexcept
{
    for (int hook = 0; hook < kHooks; ++hook)
    {
        const DWORD id = g_threads[static_cast<std::size_t>(hook)].load(std::memory_order_relaxed);
        const auto i = static_cast<std::size_t>(hook);
        if (id != 0 && !g_threadLogged[i])
        {
            g_threadLogged[i] = true;
            platform::LogDebug(L"sound listeners: {} runs on {} (thread {})", kHookNames[hook],
                               id == g_gameThread.load(std::memory_order_relaxed) ? L"the game thread" : L"another thread",
                               id);
        }
        if (g_otherThread[i].load(std::memory_order_relaxed) && !g_otherLogged[i])
        {
            g_otherLogged[i] = true;
            platform::LogDebug(L"sound listeners: {} also ran on a second thread; there it counts only and every sound is "
                               L"player 1's",
                               kHookNames[hook]);
        }
    }
}

// Why a seat's listener is off (game thread), for the line on change.
enum SeatState : int
{
    kSeatOn,
    kSeatOut,          // no player in the seat
    kSeatNoColumn,     // no view column
    kSeatNoEntry,      // the engine has no listener at the column yet
    kSeatNotFollowing  // the engine's listener at the column stopped following the seat's camera
};

/// Game thread: each seat's listener from the engine's listener at its column, on while that entry follows the seat's
/// own camera; logged on change.
void TickSeats(const std::array<Vec3, kColumns>& entries, int count) noexcept
{
    g_at[0]   = entries[0];
    g_live[0] = count >= 1 && Valid(entries[0]);
    std::array<float, 3 * kListeners> checked{};
    checked.fill(std::nanf(""));
    for (int seat = 1; seat <= kSeats; ++seat)
    {
        const auto  i          = static_cast<std::size_t>(seat);
        void* const controller = g_controllers[i];
        const int   column     = controller != nullptr ? hud::overlay_pass::ColumnOf(seat) : -1;
        SeatState   state      = kSeatOn;
        if (controller == nullptr)
        {
            state = kSeatOut;
        }
        else if (column < 1 || column >= kColumns)
        {
            state = kSeatNoColumn;
        }
        else if (column >= count)
        {
            state = kSeatNoEntry;
        }
        if (column != g_columnSeen[i] || state != kSeatOn)
        {
            g_stale[i] = kStaleTicks + 1;  // a new or regained column holds another view's place until the engine sets it
        }
        g_columnSeen[i] = state == kSeatOn ? column : -1;
        Vec3 own{};
        if (state == kSeatOn && ControllerListener(controller, own) && Valid(entries[static_cast<std::size_t>(column)]))
        {
            const float gap    = listener_rules::Distance(own, entries[static_cast<std::size_t>(column)]);
            g_controllerGap[i] = Widen(g_controllerGap[i], gap);
            g_stale[i]         = gap > kFreshCm ? g_stale[i] + 1 : 0;
        }
        else if (state == kSeatOn)
        {
            ++g_stale[i];  // its camera or the entry could not be read
        }
        if (state == kSeatOn && g_stale[i] > kStaleTicks)
        {
            state = kSeatNotFollowing;
        }
        g_live[i] = state == kSeatOn;
        g_at[i]   = state == kSeatOn ? entries[static_cast<std::size_t>(column)] : Vec3{};
        g_seatColumn[i].store(state == kSeatOn ? column : -1, std::memory_order_relaxed);
        if (state == kSeatOn)
        {
            std::copy(g_at[i].begin(), g_at[i].end(), checked.begin() + 3 * static_cast<std::ptrdiff_t>(seat));
        }
        if (state != g_stateLogged[i])
        {
            g_stateLogged[i] = state;
            constexpr const wchar_t* kWhy[] = {L"", L"no player", L"no view column", L"the engine has no listener at its column yet",
                                               L"the engine's listener at its column does not follow its camera"};
            if (state == kSeatOn)
            {
                platform::Log(L"sound listeners: {}'s listener follows its view (column {}); sounds near it are "
                              L"measured from it",
                              players::SeatName(seat), column + 1);
            }
            else
            {
                platform::Log(L"sound listeners: {}'s listener off ({}); its sounds go to the nearest listener on",
                              players::SeatName(seat), kWhy[state]);
            }
        }
        const int on = g_onSeen[i].load(std::memory_order_relaxed) ? 1 : 0;
        if (on != g_onLogged[i])
        {
            g_onLogged[i] = on;
            platform::Log(L"sound listeners: SEAD {} {}'s listener", on != 0 ? L"now sets" : L"no longer sets",
                          players::SeatName(seat));
        }
    }
    g_checked.Publish(checked);
}

std::wstring Summary()
{
    std::array<float, kListenerFields> seen{};
    const bool read = g_listeners.Read(seen);
    std::wstring text = std::format(L"{} pushes; the engine has {} listener(s); the push sent SEAD a position up to {} "
                                    L"from the engine's first",
                                    Take(g_counts.pushes), read ? std::to_wstring(static_cast<int>(seen[kCount])) : L"no",
                                    Metres(g_sentGap));
    if (const unsigned overrides = Take(g_counts.overrides); overrides != 0)
    {
        text += std::format(L" ({} pushes sent the device's override instead)", overrides);
    }
    if (const unsigned other = Take(g_counts.otherDevicePushes); other != 0)
    {
        text += std::format(L" ({} pushes of another audio device not read)", other);
    }
    for (int seat = 1; seat <= kSeats; ++seat)
    {
        const auto i      = static_cast<std::size_t>(seat);
        const int  column = hud::overlay_pass::ColumnOf(seat);
        if (g_controllers[i] == nullptr)
        {
            continue;
        }
        const float apart = g_live[i] && g_live[0] ? listener_rules::Distance(g_at[i], g_at[0]) : -1.0f;
        text += std::format(L"; {}: column {}, {} from player 1, listener {} (its view's listener up to {} from its "
                            L"controller's)",
                            players::SeatName(seat), column + 1, Metres(apart),
                            g_onSeen[i].load(std::memory_order_relaxed) ? L"on" : L"off", Metres(g_controllerGap[i]));
    }
    for (int column = 1; column < kColumns; ++column)
    {
        if (g_mirrorGap[static_cast<std::size_t>(column)] >= 0.0f)
        {
            text += std::format(L"; mirror column {} up to {} from player 1's listener", column + 1,
                                Metres(g_mirrorGap[static_cast<std::size_t>(column)]));
        }
    }
    if (const unsigned failures = Take(g_counts.setFailures); failures != 0)
    {
        text += std::format(L"; the game's setter refused a seat's listener {} time(s)", failures);
    }
    if (const unsigned full = Take(g_counts.tableFull); full != 0)
    {
        text += std::format(L"; {} sound(s) kept on player 1's listener for want of a choice entry", full);
    }
    text += L" | sounds:";
    for (int kind = 0; kind < kKinds; ++kind)
    {
        KindCounts&    counts  = g_kinds[static_cast<std::size_t>(kind)];
        const unsigned checked = Take(counts.checked);
        const unsigned rel     = Take(counts.relative);
        const unsigned fromOne = Take(counts.measured[0]);
        const unsigned heard   = Take(counts.heard[0]);
        std::wstring   detail;
        for (int seat = 1; seat <= kSeats; ++seat)
        {
            const auto     i        = static_cast<std::size_t>(seat);
            const unsigned measured = Take(counts.measured[i]);
            const unsigned heardBy  = Take(counts.heard[i]);
            if (measured != 0)
            {
                detail += std::format(L", {} measured from {}'s listener ({} heard)", measured, players::SeatName(seat),
                                      heardBy);
            }
        }
        std::wstring missed;
        for (int k = 0; k < kListeners; ++k)
        {
            if (const unsigned cut = Take(counts.missed[static_cast<std::size_t>(k)]); cut != 0)
            {
                missed += std::format(L", {} cut within {}'s reach", cut, ListenerName(k));
            }
        }
        if (checked != 0)
        {
            text += std::format(L" {} {} checked, {} heard from player 1's listener (of {}){}{}{}{};", kKindNames[kind],
                                checked, heard, fromOne, detail,
                                rel != 0 ? std::format(L", {} on player 1's listener (listener-relative)", rel) : std::wstring(),
                                missed, kind >= kChosenKinds && !missed.empty() ? L" (an area: player 1's, approximate)" : L"");
        }
    }
    text += std::format(L" {} started", Take(g_counts.starts));
    if (const unsigned untested = Take(g_counts.untested); untested != 0)
    {
        text += std::format(L"; {} updates of a sound not tested this frame or refused a choice entry (player 1's "
                            L"listener)",
                            untested);
    }
    for (int seat = 1; seat <= kSeats; ++seat)
    {
        if (const unsigned updates = Take(g_counts.updates[static_cast<std::size_t>(seat)]); updates != 0)
        {
            text += std::format(L"; {} updates from {}'s listener", updates, players::SeatName(seat));
        }
    }
    text += std::format(L" | footsteps: {} from animations, {} AutoSe steps (+{} other AutoSe sounds); AutoSe clip: {} reads, "
                        L"{} beyond player 1",
                        Take(g_counts.notifySteps), Take(g_counts.autoSteps), Take(g_counts.autoOther),
                        Take(g_counts.clipReads), Take(g_counts.clipCuts));
    for (int seat = 1; seat <= kSeats; ++seat)
    {
        if (const unsigned kept = Take(g_counts.clipKept[static_cast<std::size_t>(seat)]); kept != 0)
        {
            text += std::format(L", {} of them kept by {}'s listener", kept, players::SeatName(seat));
        }
    }
    text += std::format(L"; rustle and swing analyses: {}", Take(g_counts.analyses));
    for (int seat = 1; seat <= kSeats; ++seat)
    {
        if (const unsigned from = Take(g_counts.analysisFrom[static_cast<std::size_t>(seat)]); from != 0)
        {
            text += std::format(L", {} measured from {}'s listener", from, players::SeatName(seat));
        }
    }
    const unsigned examples = g_counts.examples.load(std::memory_order_relaxed);
    std::array<float, kExampleFields> example{};
    if (examples != g_exampleSeen && g_example.Read(example))
    {
        g_exampleSeen = examples;
        const int kind   = std::clamp(static_cast<int>(example[kExampleKind]), 0, kKinds - 1);
        const int owner  = std::clamp(static_cast<int>(example[kExampleOwner]), 0, kListeners - 1);
        const int chosen = std::clamp(static_cast<int>(example[kExampleChosen]), 0, kListeners - 1);
        text += std::format(L" | last sound cut within another player's reach: a {} sound at ({:.0f}, {:.0f}, {:.0f}), "
                            L"range {}, measured from {}'s listener at {}, {} from {}'s",
                            kKindNames[kind], example[kExampleX], example[kExampleX + 1], example[kExampleX + 2],
                            Metres(example[kExampleRange]), ListenerName(chosen), Metres(example[kExampleToChosen]),
                            Metres(example[kExampleToOwner]), ListenerName(owner));
    }
    return text;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    for (auto& column : g_seatColumn)
    {
        column.store(-1, std::memory_order_relaxed);
    }
    g_stateLogged.fill(-1);
    g_columnSeen.fill(-1);
    g_lastChecked.fill(std::nanf(""));  // no place checked yet: no seat is set before the game thread's first check
    g_controllerGap.fill(-1.0f);
    g_mirrorGap.fill(-1.0f);
    g_windowStart = std::chrono::steady_clock::now();
    const auto global    = image.Rebase(facts::kSeadListenerGlobal, sizeof(void*));
    const auto converter = image.Rebase(facts::kAutoSeConverter, sizeof(void*));
    if (!global || !converter)
    {
        platform::LogWarning(L"sound listeners: SEAD's listener or AutoSe's converter is outside the image; off");
        return false;
    }
    g_listenerGlobal = reinterpret_cast<const void* const*>(*global);
    g_converter      = reinterpret_cast<ConvertFn const*>(*converter);

    int placed = 0;
    int tried  = 0;
    const auto slot = [&](const platform::SlotFact& fact, auto* hook, platform::VTableEntry& entry) {
        ++tried;
        const bool ok = platform::ReplaceSlot(image, fact, reinterpret_cast<std::uintptr_t>(hook), entry);
        placed += ok ? 1 : 0;
        return ok;
    };
    const auto call = [&](const platform::FunctionFact& target, std::uintptr_t site, auto* hook,
                          platform::CallRedirect& redirect) {
        ++tried;
        const auto live = platform::FindFunction(image, target);
        const auto at   = image.Rebase(site, 5);
        if (live && at && redirect.Install(*at, *live, reinterpret_cast<std::uintptr_t>(hook)))
        {
            ++placed;
            return true;
        }
        platform::LogWarning(L"sound listeners: the call to {} is not the one the research read; left alone", target.what);
        return false;
    };

    // The getter first: until the mod sets a thread's listener it answers exactly as the game's.
    ++tried;
    const auto getter  = platform::FindFunction(image, facts::kSeadListenerGet);
    const bool getting = getter && g_getter.InstallWithoutOriginal(
                                       *getter, std::span(facts::kSeadListenerGetEntry).first(facts::kSeadListenerGetFirst),
                                       reinterpret_cast<std::uintptr_t>(&Getter));
    placed += getting ? 1 : 0;
    const auto set   = platform::FindFunction(image, facts::kSeadListenerSet);
    const auto speed = platform::FindFunction(image, facts::kSeadListenerUpdateVelocity);
    g_set            = set ? reinterpret_cast<SetFn>(*set) : nullptr;
    g_speed          = speed ? reinterpret_cast<SpeedFn>(*speed) : nullptr;
    const bool seats = getting && g_set != nullptr && g_speed != nullptr;

    // The sound classes: each test (counted always), and for the kinds that choose, the update with it.
    std::array<bool, kChosenKinds> paired{};
    for (int kind = 0; kind < kKinds; ++kind)
    {
        const bool test = slot(*kAudibleSlots[kind], kAudibleHooks[kind], g_audibleSlots[static_cast<std::size_t>(kind)]);
        if (kind < kChosenKinds && test && seats)
        {
            paired[static_cast<std::size_t>(kind)] =
                slot(*kUpdateSlots[kind], kUpdateHooks[kind], g_updateSlots[static_cast<std::size_t>(kind)]);
            g_paired[static_cast<std::size_t>(kind)].store(paired[static_cast<std::size_t>(kind)], std::memory_order_relaxed);
        }
    }
    const bool push = slot(facts::kSeadPushListener, &Push, g_pushSlot);
    slot(facts::kSeadSourceInit, &Start, g_initSlot);
    slot(facts::kFootNotify, &FootNotify, g_notifySlot);
    slot(facts::kAutoSeSoundForEvent, &SoundForEvent, g_eventSlot);
    const bool clip    = call(facts::kAutoSeBeyondClip, facts::kAutoSeBeyondClipCall, &Clip, g_clipCall);
    const bool analyze = call(facts::kAutoSeAnalyze, facts::kAutoSeAnalyzeCall, &Analyze, g_analyzeCall);
    g_autoSeAllowed.store(getting, std::memory_order_relaxed);

    // Last, the seats' listeners themselves: the SEAD frame that sets them turns the choice on.
    g_seatsAllowed.store(seats && push, std::memory_order_relaxed);
    const bool frame = seats && push &&
                       call(facts::kSeadListenerSpeedOfSingleton, facts::kSeadListenerSpeedCall, &Velocity, g_velocityCall);
    if (placed == 0)
    {
        platform::LogWarning(L"sound listeners: no hook could be placed; off");
        return false;
    }
    g_installed.store(true);
    control::Register({.name  = L"sound",
                       .shape = control::Shape::Text,
                       .help  = L"the sound listeners' last 10 s: each player's listener, the sounds measured from each, "
                                L"those cut within another player's reach, the footsteps (audio/listeners.h)",
                       .get   = [] {
                           std::wstring text;
                           try
                           {
                               const std::lock_guard lock(g_readingLock);
                               text = g_reading;
                           }
                           catch (...)
                           {
                           }
                           return control::Value{text};
                       }});
    const auto onOff = [](bool on) { return on ? L"on" : L"OFF"; };
    platform::Log(L"sound listeners: {} of {} hooks; each sound from the nearest player's listener {} (moving {}, placed "
                  L"{}, wedge {}); footsteps: the distance clip from every player {}, the rustle and swing range from the "
                  L"nearest {}; area sounds stay on player 1's listener (counted); a summary every 10 s at log level "
                  L"debug, reading sound",
                  placed, tried, onOff(frame), onOff(frame && paired[kMoving]), onOff(frame && paired[kPlaced]),
                  onOff(frame && paired[kWedge]), onOff(getting && clip), onOff(getting && analyze));
    return true;
}

void Remove() noexcept
{
    if (!g_installed.exchange(false))
    {
        return;
    }
    // The seats first: from here every sound and every AutoSe read is player 1's again, then the hooks go.
    g_seatsAllowed.store(false, std::memory_order_relaxed);
    g_autoSeAllowed.store(false, std::memory_order_relaxed);
    bool kept = false;
    for (platform::CallRedirect* redirect : {&g_velocityCall, &g_clipCall, &g_analyzeCall})
    {
        kept = (redirect->IsInstalled() && !redirect->Restore()) || kept;
    }
    for (platform::VTableEntry* entry : {&g_pushSlot, &g_initSlot, &g_notifySlot, &g_eventSlot})
    {
        kept = (entry->Active() && !entry->Restore()) || kept;
    }
    for (auto& entry : g_updateSlots)
    {
        kept = (entry.Active() && !entry.Restore()) || kept;
    }
    for (auto& entry : g_audibleSlots)
    {
        kept = (entry.Active() && !entry.Restore()) || kept;
    }
    kept = (g_getter.IsInstalled() && !g_getter.Restore()) || kept;
    if (kept)
    {
        g_leftBehind.store(true, std::memory_order_relaxed);  // a sound's last choice may never be renewed now
        platform::LogWarning(L"sound listeners: a hook was changed after ours; it is left as found (it answers as the "
                             L"game's)");
    }
}

void TickSeat(int seat, void* controller) noexcept
{
    if (players::SeatBuilt(seat))
    {
        g_controllers[static_cast<std::size_t>(seat)] = controller;
    }
}

void Tick(int viewColumns) noexcept
try
{
    if (!g_installed.load(std::memory_order_relaxed))
    {
        return;
    }
    g_gameThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    LogThreads();

    // The game thread's listeners (the viewport's SetListener copy): the seats' listeners, and the mirrors' gaps.
    std::array<Vec3, kColumns> entries{};
    for (auto& entry : entries)
    {
        entry.fill(std::nanf(""));
    }
    const int count = GameListeners(entries);
    if (count != g_gameCountLogged)
    {
        g_gameCountLogged = count;
        if (count >= 0)
        {
            platform::Log(L"sound listeners: the engine keeps {} listener(s) on the game thread ({} column(s) filled)",
                          count, viewColumns);
        }
    }
    TickSeats(entries, count);
    for (int column = 1; column < std::min({viewColumns, count, kColumns}); ++column)
    {
        if (hud::overlay_pass::SeatOf(column) == hud::overlay_pass::kNoSeat && Valid(entries[0]) &&
            Valid(entries[static_cast<std::size_t>(column)]))
        {
            g_mirrorGap[static_cast<std::size_t>(column)] =
                Widen(g_mirrorGap[static_cast<std::size_t>(column)],
                      listener_rules::Distance(entries[static_cast<std::size_t>(column)], entries[0]));
        }
    }
    std::array<float, kListenerFields> seen{};
    if (g_listeners.Read(seen) && seen[kCount] >= 1.0f)
    {
        const Vec3 sent  = At(seen, kSentX);
        const Vec3 first = At(seen, kColumnX);
        if (Valid(sent) && Valid(first))
        {
            g_sentGap = Widen(g_sentGap, listener_rules::Distance(sent, first));
        }
        if (platform::Enabled(platform::Level::Trace))
        {
            const std::size_t one = kColumnX + kPerColumn, two = kColumnX + 2 * kPerColumn;
            platform::LogTrace(L"sound listeners: sent SEAD ({:.0f}, {:.0f}, {:.0f}); engine {} listener(s): [0] ({:.0f}, "
                               L"{:.0f}, {:.0f}) [1] ({:.0f}, {:.0f}, {:.0f}) [2] ({:.0f}, {:.0f}, {:.0f})",
                               seen[kSentX], seen[kSentX + 1], seen[kSentX + 2], static_cast<int>(seen[kCount]),
                               seen[kColumnX], seen[kColumnX + 1], seen[kColumnX + 2], seen[one], seen[one + 1],
                               seen[one + 2], seen[two], seen[two + 1], seen[two + 2]);
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - g_windowStart < kSummaryEvery)
    {
        return;
    }
    g_windowStart = now;
    const std::wstring summary = Summary();
    g_sentGap                  = -1.0f;
    g_controllerGap.fill(-1.0f);
    g_mirrorGap.fill(-1.0f);
    platform::LogDebug(L"sound listeners (10 s): {}", summary);
    const std::lock_guard lock(g_readingLock);
    g_reading = summary;
}
catch (...)
{
}

}  // namespace lss::audio::listeners
