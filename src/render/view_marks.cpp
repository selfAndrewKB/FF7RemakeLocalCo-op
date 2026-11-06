#include "render/view_marks.h"

#include "control/registry.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <variant>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d12.h>

namespace lss::render::view_marks {

namespace {

// The context-to-queue path, read from the database (decompiles in research/.ida-mcp/view-marks/): the
// game's own FlushCommands picks the context's command list manager, its ExecuteCommandLists reaches the queue.
constexpr std::uint32_t kContextDevice     = 0x08;    // FD3D12CommandContext -> FD3D12Device*
constexpr std::uint32_t kContextAsync      = 0x2AEC;  // its bIsAsyncComputeContext: 0 picks the direct manager
constexpr std::uint32_t kDeviceDirect      = 0x40;    // FD3D12Device -> the direct FD3D12CommandListManager* (type 0)
constexpr std::uint32_t kManagerQueue      = 0x28;    // the manager's ID3D12CommandQueue* (its TRefCountPtr's pointer)

/// A site the path depends on, by its instruction bytes.
struct Site
{
    const wchar_t*                what;
    std::uintptr_t                address;
    std::span<const std::uint8_t> bytes;
};
constexpr std::array<std::uint8_t, 7> kAsyncTest{0x80, 0xBF, 0xEC, 0x2A, 0x00, 0x00, 0x00};  // cmp byte [rdi+2AECh], 0
constexpr std::array<std::uint8_t, 4> kDeviceLoad{0x48, 0x8B, 0x4F, 0x08};                    // mov rcx, [rdi+8]
constexpr std::array<std::uint8_t, 4> kDirectLoad{0x48, 0x8B, 0x49, 0x40};                    // mov rcx, [rcx+40h]
/// mov rcx, [r14+28h] (the queue); ... mov r10, [rcx]; ...; call [r10+70h] (ID3D12CommandQueue::Signal, slot 14: the
/// vtable order d3d12.h gives, which puts BeginEvent and EndEvent at slots 12 and 13).
constexpr std::array<std::uint8_t, 32> kQueueSignal{0x49, 0x8B, 0x4E, 0x28, 0x49, 0x8B, 0x86, 0x58, 0x01, 0x00, 0x00,
                                                    0x49, 0x8B, 0xB6, 0x18, 0x01, 0x00, 0x00, 0x4C, 0x8B, 0xC6, 0x4C,
                                                    0x8B, 0x11, 0x48, 0x8B, 0x50, 0x10, 0x41, 0xFF, 0x52, 0x70};
const std::array<Site, 4> kSites{{
    {L"FlushCommands' async-context test", 0x141F625DD, kAsyncTest},
    {L"FlushCommands' device load", 0x141F625E4, kDeviceLoad},
    {L"FlushCommands' direct manager load", 0x141F625F0, kDirectLoad},
    {L"ExecuteCommandLists' queue load and Signal", 0x141F6B938, kQueueSignal},
}};

constexpr UINT kPixUnicode = 0;  // PIX's event metadata for a plain wide string (WINPIX_EVENT_UNICODE_VERSION)

/// The events' names, one per marked view (the family boundary marks views 0..3).
constexpr std::array<const wchar_t*, 4> kNames{L"LSS view 1", L"LSS view 2", L"LSS view 3", L"LSS view 4"};

std::atomic<bool> g_installed{false};
std::atomic<bool> g_on{false};

// RHI thread only (the render thread without one): the events open on the queue they were begun on, a view's at the
// bottom and passes' (render/pass_marks.h) above it.
ID3D12CommandQueue* g_openOn    = nullptr;  // null: nothing open
bool                g_viewOpen  = false;
int                 g_passDepth = 0;
std::atomic<bool>   g_open{false};  // g_openOn != nullptr, for Open() on the thread that appends the nodes

std::atomic<std::uint64_t> g_begun{0};    // views begun
std::atomic<std::uint64_t> g_ended{0};    // views ended
std::atomic<std::uint64_t> g_passes{0};   // pass events begun (render/pass_marks.h)
std::atomic<std::uint64_t> g_noQueue{0};  // marks dropped: the context's queue could not be read
std::atomic<std::uint64_t> g_faults{0};   // a queue call faulted
bool                       g_firstLogged = false;  // RHI thread
bool                       g_problemLogged = false;  // RHI thread

/// RHI thread: a mark that could not be made, counted, and logged the first time.
void Problem(std::atomic<std::uint64_t>& count, const wchar_t* what) noexcept
{
    count.fetch_add(1, std::memory_order_relaxed);
    if (!std::exchange(g_problemLogged, true))
    {
        platform::LogWarning(L"view marks: {}; that view goes unmarked (logged once; counts in get view-marks)", what);
    }
}

/// The queue the context submits to, read through the game's own pointers (guarded); null if any step is unreadable.
ID3D12CommandQueue* QueueOf(const void* context) noexcept
{
    std::uint8_t async = 1;
    if (!platform::Read(static_cast<const std::uint8_t*>(context) + kContextAsync, async) || async != 0)
    {
        return nullptr;  // the game's own test: only a direct context submits to the direct queue
    }
    void* const device  = platform::ReadPointer(context, kContextDevice);
    void* const manager = platform::ReadPointer(device, kDeviceDirect);
    return static_cast<ID3D12CommandQueue*>(platform::ReadPointer(manager, kManagerQueue));
}

bool BeginGuarded(ID3D12CommandQueue* queue, const wchar_t* name) noexcept
{
    __try
    {
        queue->BeginEvent(kPixUnicode, name, static_cast<UINT>((std::wcslen(name) + 1) * sizeof(wchar_t)));
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool EndGuarded(ID3D12CommandQueue* queue) noexcept
{
    __try
    {
        queue->EndEvent();
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

control::Outcome Set(const control::Value& value)
try
{
    const bool on = std::get<bool>(value);
    if (g_on.exchange(on, std::memory_order_relaxed) != on)
    {
        platform::Log(L"view marks: {}", on ? L"on (a GPU event around each player's view family, for a GPU trace)"
                                            : L"off");
    }
    return {};
}
catch (...)
{
    return {false, L"the switch could not be set"};
}

control::Value Get()
try
{
    return control::Value{std::format(L"{}; {} views begun, {} ended; {} pass events; {} marks without a queue, {} "
                                      L"faulted",
                                      g_on.load() ? L"on" : L"off", g_begun.load(), g_ended.load(), g_passes.load(),
                                      g_noQueue.load(), g_faults.load())};
}
catch (...)
{
    return control::Value{std::wstring{}};
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    for (const Site& site : kSites)
    {
        if (!image.BytesMatch(site.address, site.bytes))
        {
            platform::LogWarning(L"view marks: UNAVAILABLE ({} differs); no GPU view events", site.what);
            return false;
        }
    }
    g_installed.store(true, std::memory_order_release);
    control::Register({.name  = L"view-marks",
                       .shape = control::Shape::OnOff,
                       .help  = L"a named GPU event (\"LSS view 1\", ...) around each player's view family on the D3D12 "
                                L"queue, for a GPU trace (off at start; on submits once more before the first family and "
                                L"after the last) (render/view_marks.h)",
                       .set   = &Set,
                       .get   = &Get});
    platform::Log(L"view marks: installed (switch view-marks on | off; off)");
    return true;
}

bool On() noexcept
{
    return g_installed.load(std::memory_order_acquire) && g_on.load(std::memory_order_relaxed);
}

bool Open() noexcept
{
    return g_open.load(std::memory_order_relaxed);
}

namespace {

/// RHI thread: ends the event on top of the stack (a pass's, else the view's).
void EndTop() noexcept
{
    if (!EndGuarded(g_openOn))
    {
        Problem(g_faults, L"a queue's EndEvent faulted");
    }
    if (g_passDepth > 0)
    {
        --g_passDepth;
    }
    else
    {
        g_viewOpen = false;
        g_ended.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_passDepth == 0 && !g_viewOpen)
    {
        g_openOn = nullptr;
        g_open.store(false, std::memory_order_relaxed);
    }
}

/// RHI thread: begins `name` on the context's queue (the queue of the events already open, if any). False if not.
bool BeginOn(void* context, const wchar_t* name) noexcept
{
    ID3D12CommandQueue* const queue = g_openOn != nullptr ? g_openOn : QueueOf(context);
    if (queue == nullptr)
    {
        Problem(g_noQueue, L"the context's direct queue could not be read");
        return false;
    }
    if (!BeginGuarded(queue, name))
    {
        Problem(g_faults, L"a queue's BeginEvent faulted");
        return false;
    }
    g_openOn = queue;
    g_open.store(true, std::memory_order_relaxed);
    return true;
}

}  // namespace

void Mark(void* context, int index) noexcept
{
    if (!g_installed.load(std::memory_order_acquire))
    {
        return;
    }
    while (g_openOn != nullptr)
    {
        EndTop();  // a pass left open (its end mark lost) closes with its view
    }
    if (index < 0 || index >= static_cast<int>(kNames.size()) ||
        !BeginOn(context, kNames[static_cast<std::size_t>(index)]))
    {
        return;
    }
    g_viewOpen = true;
    g_begun.fetch_add(1, std::memory_order_relaxed);
    if (!std::exchange(g_firstLogged, true))
    {
        platform::LogDebug(L"view marks: the first view event begun on the direct queue");
    }
}

void BeginPass(void* context, const wchar_t* name) noexcept
{
    if (g_installed.load(std::memory_order_acquire) && BeginOn(context, name))
    {
        ++g_passDepth;
        g_passes.fetch_add(1, std::memory_order_relaxed);
    }
}

void EndPass() noexcept
{
    if (g_passDepth > 0)  // never a view's event: a pass begun before its marks were on has no begin to end
    {
        EndTop();
    }
}

}  // namespace lss::render::view_marks
