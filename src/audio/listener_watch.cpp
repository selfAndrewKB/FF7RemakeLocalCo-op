#include "audio/listener_watch.h"

#include "audio/listener_rules.h"
#include "control/registry.h"
#include "engine_facts.h"
#include "hud/overlay_pass.h"
#include "platform/call_redirect.h"
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
#include <mutex>
#include <string>

namespace lss::audio::listener_watch {

namespace {

using listener_rules::Vec3;

constexpr int  kSeats       = players::kSeatsBuilt;            // the further players, seats 1..kSeats
constexpr int  kColumns     = hud::overlay_pass::kMaxColumns;  // the engine listeners read: one per column
constexpr auto kSummaryEvery = std::chrono::seconds(10);
constexpr float kMetres     = 0.01f;

using PushFn       = std::uint64_t (*)(void* device);
using AudibleFn    = std::uint64_t (*)(void* object);
using InitFn       = std::uint8_t (*)(void* source, void* waveInstance);
using ClipFn       = std::uint8_t (*)(const void* character, float distance);
using AnalyzeFn    = void (*)(void* record, void* analysis);
using NotifyFn     = void (*)(void* notify, void* mesh, void* animation, void* extra);
using EventFn      = std::uint8_t (*)(void* callback, void* out, const std::int32_t* event);
using ConvertFn    = float* (*)(float* out, const float* in);
using ListenerAtFn = void (*)(void* controller, float* location, float* front, float* right);

// The SEAD sound classes the watch counts, in the order of their slots below.
enum Kind : int
{
    kMoving,   // DynamicSound: every spatialized engine sound
    kPlaced,   // PointSound
    kWedge,    // SphericalWedgeSound
    kPolygon,  // PolygonSound (an area)
    kSector,   // CylindricalSectorSound (an area)
    kKinds
};
constexpr const wchar_t* kKindNames[kKinds] = {L"moving", L"placed", L"wedge", L"polygon area", L"sector area"};
constexpr const platform::SlotFact* kAudibleSlots[kKinds] = {&facts::kSeadDynamicAudibleSlot, &facts::kSeadPointAudibleSlot,
                                                             &facts::kSeadWedgeAudibleSlot, &facts::kSeadPolygonAudibleSlot,
                                                             &facts::kSeadSectorAudibleSlot};

// The hooks whose thread is noted once.
enum Hook : int
{
    kPushHook,
    kAudibleHook,
    kInitHook,
    kClipHook,
    kAnalyzeHook,
    kNotifyHook,
    kEventHook,
    kHooks
};
constexpr const wchar_t* kHookNames[kHooks] = {L"the listener push",     L"the sound classes' audibility tests",
                                               L"the sound start",        L"AutoSe's distance clip",
                                               L"AutoSe's frame analysis", L"the foot sound notify",
                                               L"AutoSe's event sounds"};

// The audio thread's view after each push: the engine's listener count, its first kColumns entries, the position the
// push sent SEAD, whether that was the override. A position that could not be read is NaN.
enum ListenerField : std::size_t
{
    kCount,
    kEntryX,                              // entry c at kEntryX + 3c
    kSentX          = kEntryX + 3 * kColumns,
    kOverride       = kSentX + 3,
    kListenerFields
};
static_assert(kColumns == 3, "the Trace line names three engine listeners");
// The latest sound a further player's listener would have heard but player 1's cut (SEAD thread).
enum ExampleField : std::size_t
{
    kExampleKind,
    kExampleSeat,
    kExampleRange,
    kExampleToOne,
    kExampleToSeat,
    kExampleX,
    kExampleFields = kExampleX + 3
};

platform::SeqFloats<kListenerFields> g_listeners;
platform::SeqFloats<kExampleFields>  g_example;

// Placed once at install on the game thread, before the hooks go live; read by the hooks afterwards.
platform::VTableEntry                       g_pushSlot, g_initSlot, g_notifySlot, g_eventSlot;
std::array<platform::VTableEntry, kKinds>   g_audibleSlots;
platform::CallRedirect                      g_clipCall, g_analyzeCall;
const void* const*                          g_listenerGlobal = nullptr;  // live address of SEAD's listener pointer
ConvertFn const*                            g_converter      = nullptr;  // live address of AutoSe's converter pointer
std::atomic<bool>                           g_installed{false};

std::atomic<void*> g_device{nullptr};  // the first audio device whose push ran (the game's one SEAD device)
std::array<std::atomic<DWORD>, kHooks> g_threads{};
std::array<std::atomic<bool>, kHooks>  g_otherThread{};  // a hook ran on a second thread
// Each seat's column while its player is in (-1: out or no view), written by the game thread.
std::array<std::atomic<int>, kSeats + 1> g_seatColumn{};

struct KindCounts
{
    std::atomic<unsigned>                         checked{0};
    std::atomic<unsigned>                         relative{0};    // listener-relative moving sounds (player 1's mix)
    std::atomic<unsigned>                         heardOne{0};    // heard from player 1's listener
    std::array<std::atomic<unsigned>, kSeats + 1> inReach{};      // within the seat's reach
    std::array<std::atomic<unsigned>, kSeats + 1> cutInReach{};   //   ... and cut by player 1's listener
};
std::array<KindCounts, kKinds> g_kinds;

struct Counts
{
    std::atomic<unsigned>                         pushes{0};
    std::atomic<unsigned>                         otherDevicePushes{0};  // a push of another device, not read
    std::atomic<unsigned>                         overrides{0};          // pushes that sent the override
    std::atomic<unsigned>                         starts{0};
    std::atomic<unsigned>                         examples{0};
    std::atomic<unsigned>                         clipReads{0};
    std::atomic<unsigned>                         clipCuts{0};      // beyond player 1's listener
    std::array<std::atomic<unsigned>, kSeats + 1> clipKept{};       //   ... but within the seat's
    std::atomic<unsigned>                         analyses{0};
    std::array<std::atomic<unsigned>, kSeats + 1> analysisNearer{}; // the seat's listener nearer the character
    std::atomic<unsigned>                         notifySteps{0};
    std::atomic<unsigned>                         autoSteps{0};
    std::atomic<unsigned>                         autoOther{0};
};
Counts g_counts;

// Game thread.
std::array<void*, kSeats + 1>         g_controllers{};
std::array<int, kSeats + 1>           g_columnLogged{};
int                                   g_gameCountLogged = -1;
DWORD                                 g_gameThread      = 0;
std::array<bool, kHooks>              g_threadLogged{};
std::array<bool, kHooks>              g_otherLogged{};
std::chrono::steady_clock::time_point g_windowStart{};
unsigned                              g_exampleSeen = 0;
// The window's largest gaps, cm: what the push sent SEAD against entry 0; each seat's entry against its controller's
// own listener; each mirror column's entry against entry 0. -1: not read this window.
float                                 g_sentGap = -1.0f;
std::array<float, kSeats + 1>         g_controllerGap{};
std::array<float, kColumns>           g_mirrorGap{};

std::mutex   g_readingLock;  // the reading (written and read on the game thread; the console reads it too)
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

/// SEAD's listener object (created and destroyed on SEAD's thread at its start and end only).
const void* SeadListener() noexcept
{
    return platform::ReadPointer(g_listenerGlobal, 0);
}

bool Valid(const Vec3& at) noexcept
{
    return !std::isnan(at[0]) && !std::isnan(at[1]) && !std::isnan(at[2]);
}

/// The engine listener the seat's column names, from the push's last view; false while the seat is out, has no
/// column, or the engine has no such entry.
bool SeatListener(const std::array<float, kListenerFields>& seen, int seat, Vec3& out) noexcept
{
    const int column = g_seatColumn[static_cast<std::size_t>(seat)].load(std::memory_order_relaxed);
    if (column < 1 || column >= kColumns || static_cast<float>(column) >= seen[kCount])
    {
        return false;
    }
    const std::size_t at = kEntryX + 3 * static_cast<std::size_t>(column);
    out                  = {seen[at], seen[at + 1], seen[at + 2]};
    return Valid(out);
}

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

/// Audio thread: the game's push, then what it read and what it sent SEAD. Only the first device's pushes are read
/// (the push itself works for the engine's main device only).
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
        bool       ok = false;
        const Vec3 at = Read3Guarded(data + static_cast<std::size_t>(facts::kAudioDeviceListenerStride) * i,
                                     facts::kTransformTranslation, ok);
        if (ok)
        {
            std::copy(at.begin(), at.end(), out.begin() + kEntryX + 3 * i);
        }
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

/// SEAD's thread, after the game's test of one sound: the counts of its kind.
void Count(Kind kind, const void* object) noexcept
{
    KindCounts& counts = g_kinds[kind];
    counts.checked.fetch_add(1, std::memory_order_relaxed);
    std::uint8_t relative = 0;  // a moving sound's field only
    if (kind == kMoving)
    {
        std::memcpy(&relative, static_cast<const std::uint8_t*>(object) + facts::kSeadDynamicRelative, 1);
    }
    if (relative == 1)
    {
        counts.relative.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::uint8_t heard = 0;
    float        range = 0.0f;
    std::memcpy(&heard, static_cast<const std::uint8_t*>(object) + facts::kSeadObjectAudible, 1);
    std::memcpy(&range, static_cast<const std::uint8_t*>(object) + facts::kSeadObjectRange, sizeof(range));
    if (heard != 0)
    {
        counts.heardOne.fetch_add(1, std::memory_order_relaxed);
    }
    std::array<float, kListenerFields> seen{};
    if (range <= 0.0f || !g_listeners.Read(seen))
    {
        return;
    }
    const std::uint32_t at     = kind == kMoving                       ? facts::kSeadDynamicSource
                                 : kind == kPlaced || kind == kWedge ? facts::kSeadPointSource
                                                                     : facts::kSeadObjectMeasured;
    const Vec3          source = Read3(object, at);
    for (int seat = 1; seat <= kSeats; ++seat)
    {
        Vec3 listener{};
        if (!SeatListener(seen, seat, listener) || !listener_rules::Within(range, listener_rules::Distance(listener, source)))
        {
            continue;
        }
        counts.inReach[static_cast<std::size_t>(seat)].fetch_add(1, std::memory_order_relaxed);
        if (heard != 0)
        {
            continue;
        }
        counts.cutInReach[static_cast<std::size_t>(seat)].fetch_add(1, std::memory_order_relaxed);
        bool       ok  = false;
        const Vec3 one = Read3Guarded(SeadListener(), facts::kSeadListenerPosition, ok);  // this thread sets it
        g_example.Publish({static_cast<float>(kind), static_cast<float>(seat), range,
                           ok ? listener_rules::Distance(one, source) : -1.0f, listener_rules::Distance(listener, source),
                           source[0], source[1], source[2]});
        g_counts.examples.fetch_add(1, std::memory_order_relaxed);
    }
}

template <int K>
std::uint64_t Audible(void* object) noexcept
{
    const auto result = reinterpret_cast<AudibleFn>(g_audibleSlots[K].Original())(object);
    Note(kAudibleHook);
    Count(static_cast<Kind>(K), object);
    return result;
}

constexpr AudibleFn kAudibleHooks[kKinds] = {&Audible<kMoving>, &Audible<kPlaced>, &Audible<kWedge>, &Audible<kPolygon>,
                                             &Audible<kSector>};

std::uint8_t Start(void* source, void* waveInstance) noexcept
{
    const auto result = reinterpret_cast<InitFn>(g_initSlot.Original())(source, waveInstance);
    Note(kInitHook);
    g_counts.starts.fetch_add(1, std::memory_order_relaxed);
    return result;
}

/// AutoSe's distance clip: the game's answer from player 1's listener, then whether each seat's would keep it.
std::uint8_t Clip(const void* character, float distance) noexcept
{
    const auto beyond = reinterpret_cast<ClipFn>(g_clipCall.Original())(character, distance);
    Note(kClipHook);
    g_counts.clipReads.fetch_add(1, std::memory_order_relaxed);
    std::array<float, kListenerFields> seen{};
    if (beyond == 0 || character == nullptr)
    {
        return beyond;
    }
    g_counts.clipCuts.fetch_add(1, std::memory_order_relaxed);
    if (!g_listeners.Read(seen))
    {
        return beyond;
    }
    const Vec3 at = Read3(character, 0);
    for (int seat = 1; seat <= kSeats; ++seat)
    {
        Vec3 listener{};
        if (SeatListener(seen, seat, listener) && !listener_rules::BeyondClip(at, ToAutoSe(listener), distance))
        {
            g_counts.clipKept[static_cast<std::size_t>(seat)].fetch_add(1, std::memory_order_relaxed);
        }
    }
    return beyond;
}

/// AutoSe's frame analysis (its rustle and swing range from player 1's listener): whether a seat is nearer the point
/// the game measures. Frames the game does not measure are not counted.
void Analyze(void* record, void* analysis) noexcept
{
    reinterpret_cast<AnalyzeFn>(g_analyzeCall.Original())(record, analysis);
    Note(kAnalyzeHook);
    const void*  frame = platform::ReadPointer(analysis, facts::kAutoSeAnalysisFrame);
    std::uint8_t flags = 0;
    if (frame == nullptr || !platform::Read(static_cast<const std::uint8_t*>(frame) + facts::kAutoSeFrameFlags, flags) ||
        (flags & facts::kAutoSeFrameUnmeasured) != 0)
    {
        return;
    }
    g_counts.analyses.fetch_add(1, std::memory_order_relaxed);
    std::array<float, kListenerFields> seen{};
    bool       ok = false;
    const Vec3 at = Read3Guarded(frame, facts::kAutoSeFramePoint, ok);
    if (!ok || !g_listeners.Read(seen))
    {
        return;
    }
    const Vec3 one{seen[kSentX], seen[kSentX + 1], seen[kSentX + 2]};
    if (!Valid(one))
    {
        return;
    }
    const float toOne = listener_rules::Distance(at, ToAutoSe(one));
    for (int seat = 1; seat <= kSeats; ++seat)
    {
        Vec3 listener{};
        if (SeatListener(seen, seat, listener) && listener_rules::Distance(at, ToAutoSe(listener)) < toOne)
        {
            g_counts.analysisNearer[static_cast<std::size_t>(seat)].fetch_add(1, std::memory_order_relaxed);
        }
    }
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
    return cm < 0.0f ? std::wstring(L"-") : std::format(L"{:.1f} m", cm * kMetres);
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
            entries[static_cast<std::size_t>(i)].fill(std::nanf(""));  // a NaN gap never widens a window's largest
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
            platform::LogDebug(L"sound watch: {} runs on {} (thread {})", kHookNames[hook],
                               id == g_gameThread ? L"the game thread" : L"another thread", id);
        }
        if (g_otherThread[i].load(std::memory_order_relaxed) && !g_otherLogged[i])
        {
            g_otherLogged[i] = true;
            platform::LogDebug(L"sound watch: {} also ran on a second thread; its counts may mix", kHookNames[hook]);
        }
    }
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
        const int column = g_seatColumn[static_cast<std::size_t>(seat)].load(std::memory_order_relaxed);
        if (column < 0)
        {
            continue;
        }
        Vec3        listener{};
        const Vec3  first{seen[kEntryX], seen[kEntryX + 1], seen[kEntryX + 2]};
        const float apart = read && SeatListener(seen, seat, listener) && Valid(first)
                                ? listener_rules::Distance(listener, first)
                                : -1.0f;
        text += std::format(L"; {}: column {}, {} from player 1, its view's listener up to {} from its controller's",
                            players::SeatName(seat), column, Metres(apart),
                            Metres(g_controllerGap[static_cast<std::size_t>(seat)]));
    }
    for (int column = 1; column < kColumns; ++column)
    {
        if (g_mirrorGap[static_cast<std::size_t>(column)] >= 0.0f)
        {
            text += std::format(L"; mirror column {} up to {} from player 1's listener", column,
                                Metres(g_mirrorGap[static_cast<std::size_t>(column)]));
        }
    }
    text += L" | sounds:";
    for (int kind = 0; kind < kKinds; ++kind)
    {
        KindCounts&    counts  = g_kinds[static_cast<std::size_t>(kind)];
        const unsigned checked = Take(counts.checked);
        const unsigned heard   = Take(counts.heardOne);
        const unsigned rel     = Take(counts.relative);
        std::wstring   reach;
        for (int seat = 1; seat <= kSeats; ++seat)
        {
            const unsigned in  = Take(counts.inReach[static_cast<std::size_t>(seat)]);
            const unsigned cut = Take(counts.cutInReach[static_cast<std::size_t>(seat)]);
            if (in != 0)
            {
                reach += std::format(L", {} within {}'s reach ({} of them cut)", in, players::SeatName(seat), cut);
            }
        }
        if (checked != 0)
        {
            text += std::format(L" {} {} checked, {} heard from player 1{}{}{};", kKindNames[kind], checked, heard,
                                rel != 0 ? std::format(L", {} on player 1's listener", rel) : std::wstring(), reach,
                                kind >= kPolygon && !reach.empty() ? L" (approximate)" : L"");
        }
    }
    text += std::format(L" {} started", Take(g_counts.starts));
    text += std::format(L" | footsteps: {} from animations, {} AutoSe steps (+{} other AutoSe sounds); AutoSe clip: {} reads, "
                        L"{} cut for player 1",
                        Take(g_counts.notifySteps), Take(g_counts.autoSteps), Take(g_counts.autoOther),
                        Take(g_counts.clipReads), Take(g_counts.clipCuts));
    for (int seat = 1; seat <= kSeats; ++seat)
    {
        const unsigned kept = Take(g_counts.clipKept[static_cast<std::size_t>(seat)]);
        if (kept != 0)
        {
            text += std::format(L", {} of them within {}'s", kept, players::SeatName(seat));
        }
    }
    text += std::format(L"; rustle and swing analyses: {}", Take(g_counts.analyses));
    for (int seat = 1; seat <= kSeats; ++seat)
    {
        const unsigned nearer = Take(g_counts.analysisNearer[static_cast<std::size_t>(seat)]);
        if (nearer != 0)
        {
            text += std::format(L", {} nearer {}", nearer, players::SeatName(seat));
        }
    }
    const unsigned examples = g_counts.examples.load(std::memory_order_relaxed);
    std::array<float, kExampleFields> example{};
    if (examples != g_exampleSeen && g_example.Read(example))
    {
        g_exampleSeen    = examples;
        const int kind   = std::clamp(static_cast<int>(example[kExampleKind]), 0, kKinds - 1);
        text += std::format(L" | last sound cut near a further player: a {} sound at ({:.0f}, {:.0f}, {:.0f}), range {}, "
                            L"{} from player 1's listener, {} from {}'s",
                            kKindNames[kind], example[kExampleX], example[kExampleX + 1], example[kExampleX + 2],
                            Metres(example[kExampleRange]), Metres(example[kExampleToOne]), Metres(example[kExampleToSeat]),
                            players::SeatName(static_cast<int>(example[kExampleSeat])));
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
    g_columnLogged.fill(-1);
    g_controllerGap.fill(-1.0f);
    g_mirrorGap.fill(-1.0f);
    g_windowStart = std::chrono::steady_clock::now();
    const auto global    = image.Rebase(facts::kSeadListenerGlobal, sizeof(void*));
    const auto converter = image.Rebase(facts::kAutoSeConverter, sizeof(void*));
    if (!global || !converter)
    {
        platform::LogWarning(L"sound watch: SEAD's listener or AutoSe's converter is outside the image; the watch is off");
        return false;
    }
    g_listenerGlobal = reinterpret_cast<const void* const*>(*global);
    g_converter      = reinterpret_cast<ConvertFn const*>(*converter);

    int placed = 0;
    int tried  = 0;
    const auto slot = [&](const platform::SlotFact& fact, auto* hook, platform::VTableEntry& entry) {
        ++tried;
        if (platform::ReplaceSlot(image, fact, reinterpret_cast<std::uintptr_t>(hook), entry))
        {
            ++placed;
        }
    };
    slot(facts::kSeadPushListener, &Push, g_pushSlot);
    for (int kind = 0; kind < kKinds; ++kind)
    {
        slot(*kAudibleSlots[kind], kAudibleHooks[kind], g_audibleSlots[static_cast<std::size_t>(kind)]);
    }
    slot(facts::kSeadSourceInit, &Start, g_initSlot);
    slot(facts::kFootNotify, &FootNotify, g_notifySlot);
    slot(facts::kAutoSeSoundForEvent, &SoundForEvent, g_eventSlot);
    const auto call = [&](const platform::FunctionFact& target, std::uintptr_t site, auto* hook,
                          platform::CallRedirect& redirect) {
        ++tried;
        const auto live = platform::FindFunction(image, target);
        const auto at   = image.Rebase(site, 5);
        if (live && at && redirect.Install(*at, *live, reinterpret_cast<std::uintptr_t>(hook)))
        {
            ++placed;
        }
        else
        {
            platform::LogWarning(L"sound watch: the call to {} is not the one the research read; not watched", target.what);
        }
    };
    call(facts::kAutoSeBeyondClip, facts::kAutoSeBeyondClipCall, &Clip, g_clipCall);
    call(facts::kAutoSeAnalyze, facts::kAutoSeAnalyzeCall, &Analyze, g_analyzeCall);
    if (placed == 0)
    {
        platform::LogWarning(L"sound watch: no hook could be placed; the watch is off");
        return false;
    }
    g_installed.store(true);
    control::Register({.name  = L"sound",
                       .shape = control::Shape::Text,
                       .help  = L"the sound watch's last 10 s: the listeners, the sounds cut near a further player, the "
                                L"footsteps (audio/listener_watch.h)",
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
    platform::Log(L"sound watch: installed, {} of {} hooks (logging only: the listeners, the sounds cut near a further "
                  L"player, the footsteps; a summary every 10 s at log level debug, reading sound)",
                  placed, tried);
    return true;
}

void Remove() noexcept
{
    if (!g_installed.exchange(false))
    {
        return;
    }
    bool kept = false;
    for (platform::VTableEntry* entry : {&g_pushSlot, &g_initSlot, &g_notifySlot, &g_eventSlot})
    {
        kept = (entry->Active() && !entry->Restore()) || kept;
    }
    for (auto& entry : g_audibleSlots)
    {
        kept = (entry.Active() && !entry.Restore()) || kept;
    }
    for (platform::CallRedirect* redirect : {&g_clipCall, &g_analyzeCall})
    {
        kept = (redirect->IsInstalled() && !redirect->Restore()) || kept;
    }
    if (kept)
    {
        platform::LogWarning(L"sound watch: a hook was changed after the watch; it is left as found");
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
    g_gameThread = GetCurrentThreadId();
    LogThreads();

    // Each seat's column, from the HUD's record of who fills each column this tick.
    for (int seat = 1; seat <= kSeats; ++seat)
    {
        const auto i      = static_cast<std::size_t>(seat);
        const int  column = g_controllers[i] != nullptr ? hud::overlay_pass::ColumnOf(seat) : -1;
        g_seatColumn[i].store(column, std::memory_order_relaxed);
        if (column != g_columnLogged[i])
        {
            g_columnLogged[i] = column;
            if (column >= 0)
            {
                platform::LogDebug(L"sound watch: {}'s view is column {} (the engine's listener {} if it numbers views as "
                                   L"columns)",
                                   players::SeatName(seat), column, column);
            }
            else
            {
                platform::LogDebug(L"sound watch: {} has no view column", players::SeatName(seat));
            }
        }
    }

    // The game thread's listeners (the viewport's SetListener copy) against each seat's controller and the mirrors.
    std::array<Vec3, kColumns> entries{};
    const int                  count = GameListeners(entries);
    if (count != g_gameCountLogged)
    {
        g_gameCountLogged = count;
        if (count >= 0)
        {
            platform::LogDebug(L"sound watch: the engine keeps {} listener(s) on the game thread ({} column(s) filled)",
                               count, viewColumns);
        }
    }
    for (int seat = 1; seat <= kSeats; ++seat)
    {
        const int column = g_seatColumn[static_cast<std::size_t>(seat)].load(std::memory_order_relaxed);
        Vec3      own{};
        if (column >= 0 && column < count && column < kColumns &&
            ControllerListener(g_controllers[static_cast<std::size_t>(seat)], own))
        {
            g_controllerGap[static_cast<std::size_t>(seat)] =
                Widen(g_controllerGap[static_cast<std::size_t>(seat)],
                      listener_rules::Distance(own, entries[static_cast<std::size_t>(column)]));
        }
    }
    for (int column = 1; column < std::min({viewColumns, count, kColumns}); ++column)
    {
        if (hud::overlay_pass::SeatOf(column) == hud::overlay_pass::kNoSeat)
        {
            g_mirrorGap[static_cast<std::size_t>(column)] =
                Widen(g_mirrorGap[static_cast<std::size_t>(column)],
                      listener_rules::Distance(entries[static_cast<std::size_t>(column)], entries[0]));
        }
    }
    std::array<float, kListenerFields> seen{};
    if (g_listeners.Read(seen) && seen[kCount] >= 1.0f)
    {
        const Vec3 sent{seen[kSentX], seen[kSentX + 1], seen[kSentX + 2]};
        const Vec3 first{seen[kEntryX], seen[kEntryX + 1], seen[kEntryX + 2]};
        if (Valid(sent) && Valid(first))
        {
            g_sentGap = Widen(g_sentGap, listener_rules::Distance(sent, first));
        }
        if (platform::Enabled(platform::Level::Trace))
        {
            platform::LogTrace(L"sound watch: sent SEAD ({:.0f}, {:.0f}, {:.0f}); engine {} listener(s): [0] ({:.0f}, "
                               L"{:.0f}, {:.0f}) [1] ({:.0f}, {:.0f}, {:.0f}) [2] ({:.0f}, {:.0f}, {:.0f})",
                               seen[kSentX], seen[kSentX + 1], seen[kSentX + 2], static_cast<int>(seen[kCount]),
                               seen[kEntryX], seen[kEntryX + 1], seen[kEntryX + 2], seen[kEntryX + 3], seen[kEntryX + 4],
                               seen[kEntryX + 5], seen[kEntryX + 6], seen[kEntryX + 7], seen[kEntryX + 8]);
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
    platform::LogDebug(L"sound watch (10 s): {}", summary);
    const std::lock_guard lock(g_readingLock);
    g_reading = summary;
}
catch (...)
{
}

}  // namespace lss::audio::listener_watch
