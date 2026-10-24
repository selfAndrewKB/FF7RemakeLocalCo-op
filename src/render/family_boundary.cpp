#include "render/family_boundary.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"
#include "platform/verified_code.h"
#include "render/family_boundary_list.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <new>

namespace lss::render::family_boundary {

namespace {

// Facts read from the database (RE-haze-occlusion.md Pass 10 I.1.5, checked). The entry bytes hold no relocated or
// RIP-relative operand.
constexpr std::array<std::uint8_t, 16> kClearStateBytes{0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x4C,
                                                        0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41};
constexpr platform::FunctionFact kClearState{L"FD3D12CommandContext::ClearState", 0x141F627C0, kClearStateBytes};
constexpr std::array<std::uint8_t, 16> kSubmitBytes{0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B,
                                                    0xD9, 0x48, 0x8B, 0x49, 0x08, 0x48, 0x81, 0xC1};
constexpr platform::FunctionFact kSubmit{L"FD3D12CommandContext::RHISubmitCommandsHint", 0x141F67100, kSubmitBytes};
constexpr std::array<std::uint8_t, 16> kGrowBytes{0x40, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC,
                                                  0x30, 0x83, 0xC2, 0x10, 0x4C, 0x8B, 0xF1, 0x48};
constexpr platform::FunctionFact kGrow{L"the RHI command arena's grow", 0x141CB8DA0, kGrowBytes};

constexpr std::uintptr_t kContextVtable    = 0x144D3AA68;  // FD3D12CommandContext (the redirector's is 0x144D3A690)
constexpr std::uint32_t  kSubmitSlot       = 0x90;         // its RHISubmitCommandsHint entry (slot 18)
constexpr std::uintptr_t kImmediateList    = 0x14593F4D8;  // GRHICommandList_Immediate (the object)
constexpr std::size_t    kImmediateListEnd = 0x40;         // the fields the boundary reads and writes
constexpr std::uintptr_t kTaskGraph        = 0x145978908;  // GTaskGraph: a pointer to the task graph
constexpr std::uintptr_t kRenderThread     = 0x1453A329C;  // ENamedThreads::RenderThread (a dword, set at runtime)
constexpr std::uintptr_t kThreadedRender   = 0x14582C4A9;  // GIsThreadedRendering (a byte)

constexpr std::uint32_t kAnyThread = 255;     // ENamedThreads::AnyThread: what the game's submit passes as its own thread
constexpr std::uint64_t kLogPeriod = 30'000;  // ms between the Debug lines

/// FTaskGraphInterface::QueueTask(this, task, threadToExecuteOn, currentThreadIfKnown): vt[0] of the task graph.
using QueueTaskFn = void (*)(void* taskGraph, void* task, std::uint32_t thread, std::uint32_t currentThread);
using ContextFn   = void (*)(void* context);

/// A render-thread task laid out as the game's render commands (TGraphTask, Pass 10 I.1.4): FBaseGraphTask's vtable,
/// the thread to run on and the prerequisites outstanding, the task's storage, TaskConstructed and Subsequents (none:
/// fire and forget). The task graph reads only the first three; the mod's own fields follow.
struct Task
{
    const void* const* vtable;
    std::uint32_t      thread        = kAnyThread;
    std::int32_t       prerequisites = 1;  // dropped to 0 just before the queue, as the game's submit does
    std::uint64_t      storage       = 0;
    std::uint8_t       constructed   = 1;
    std::uint8_t       padding[7]    = {};
    void*              subsequents   = nullptr;
    family_now::Family next;
    bool               reset = false;
};
static_assert(offsetof(Task, thread) == 0x08 && offsetof(Task, prerequisites) == 0x0C && offsetof(Task, storage) == 0x10 &&
              offsetof(Task, constructed) == 0x18 && offsetof(Task, subsequents) == 0x20);

// Set once by Install, then only read; published by g_installed's release store.
QueueTaskFn      g_queueTask      = nullptr;
boundary::GrowFn g_grow           = nullptr;
ContextFn        g_clearState     = nullptr;
ContextFn        g_submit         = nullptr;
std::uintptr_t   g_contextVtable  = 0;
std::byte*       g_list           = nullptr;
void* const*     g_taskGraph      = nullptr;
const std::uint32_t* g_renderThread = nullptr;
const std::uint8_t*  g_threaded     = nullptr;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_faulted{false};

// What happened (written on the game, render and RHI threads).
struct Counts
{
    std::atomic<std::uint64_t> queued{0};    // game thread: tasks queued
    std::atomic<std::uint64_t> direct{0};    // game thread: bodies run at once (rendering not threaded)
    std::atomic<std::uint64_t> appended{0};  // render thread: nodes appended to the list
    std::atomic<std::uint64_t> inlined{0};   // render thread: resets run at once (the list was being walked)
    std::atomic<std::uint64_t> reset{0};     // RHI thread: ClearState + submit run
    std::atomic<std::uint64_t> skipped{0};   // RHI thread: the list's context was not the default direct one
} g_counts;

/// Any thread: the boundary is off from now on, and no family's place stays set. A task queued before the fault that
/// runs after it clears the place again (Body), and so does every frame's Poll, so a stale place cannot outlive the
/// fault.
void Fault() noexcept
{
    g_faulted.store(true);
    family_now::Set({});
}

std::uint64_t g_logAt        = 0;  // game thread
std::uint64_t g_loggedQueued = 0;
bool          g_faultLogged  = false;

// --- RHI thread (or the render thread when there is none): the reset itself ----------------------------------------------

enum class ContextCheck
{
    DefaultDirect,
    Other,
    Faulted,
};

/// The list's context, checked to be the default direct one. The reads follow engine pointers, so they run under a guard.
ContextCheck CheckContext(const std::byte* commandList, std::byte*& context) noexcept
{
    __try
    {
        context = boundary::Load<std::byte*>(commandList, boundary::list::kContext);
        return boundary::IsDefaultDirectContext(context, g_contextVtable) ? ContextCheck::DefaultDirect : ContextCheck::Other;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return ContextCheck::Faulted;
    }
}

void RunReset(const std::byte* commandList) noexcept
{
    if (g_faulted.load())
    {
        return;
    }
    std::byte* context = nullptr;
    switch (CheckContext(commandList, context))
    {
        case ContextCheck::Faulted: Fault(); return;
        case ContextCheck::Other: g_counts.skipped.fetch_add(1, std::memory_order_relaxed); return;
        case ContextCheck::DefaultDirect: break;
    }
    // RHIEndFrame's pair on the same context: ClearState, then close, submit and reopen the list. The submit runs only
    // after a ClearState that returned.
    if (!platform::CallGuarded(g_clearState, static_cast<void*>(context)) ||
        !platform::CallGuarded(g_submit, static_cast<void*>(context)))
    {
        Fault();
        return;
    }
    g_counts.reset.fetch_add(1, std::memory_order_relaxed);
}

/// The node's ExecuteAndDestruct(list, node): the walk frees the arena, so there is nothing to destroy.
void NodeExecute(void* commandList, void* /*node*/) noexcept
{
    RunReset(static_cast<const std::byte*>(commandList));
}

// --- Render thread: the task's body ----------------------------------------------------------------------------------------

/// Appends the node; the arena's grow is the game's allocator, so it runs under a guard.
bool AppendGuarded() noexcept
{
    __try
    {
        boundary::AppendNode(g_list, &NodeExecute, g_grow);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool ExecutingGuarded(bool& executing) noexcept
{
    __try
    {
        executing = boundary::IsExecuting(g_list);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

void Body(family_now::Family next, bool reset) noexcept
{
    family_now::Set(next);
    if (g_faulted.load())
    {
        family_now::Set({});  // after the place: a fault on another thread between the two cannot leave it set
        return;
    }
    if (!reset)
    {
        return;
    }
    bool executing = false;
    if (!ExecutingGuarded(executing))
    {
        Fault();
    }
    else if (executing)
    {
        g_counts.inlined.fetch_add(1, std::memory_order_relaxed);  // walked in place: the game runs commands at once then
        RunReset(g_list);
    }
    else if (AppendGuarded())
    {
        g_counts.appended.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        Fault();
    }
}

/// The task's vtable[1]: ExecuteTask(this, NewTasks&, CurrentThread). The task is the mod's: freed here, never given to
/// the game's small-task free list (the caller does not touch the task after this returns, Pass 10 I.1.5 point 4).
void TaskExecute(Task* task, void* /*newTasks*/, std::uint32_t /*currentThread*/) noexcept
{
    Body(task->next, task->reset);
    delete task;
}

/// The task's vtable[0]: the scalar deleting destructor (this, flags). The task graph does not call it for a task that
/// frees itself in ExecuteTask; kept correct in case it does.
Task* TaskDestroy(Task* task, std::uint32_t flags) noexcept
{
    if ((flags & 1) != 0)
    {
        delete task;
    }
    return task;
}

const void* const kTaskVtable[] = {reinterpret_cast<const void*>(&TaskDestroy), reinterpret_cast<const void*>(&TaskExecute)};

enum class Queued
{
    Yes,
    No,       // nothing was called: the task is still the mod's
    Faulted,  // the call faulted: the task may be queued, so it is left
};

/// The task graph's QueueTask, through its vtable. Guarded.
Queued QueueGuarded(Task* task) noexcept
{
    __try
    {
        void* const graph = *g_taskGraph;
        if (graph == nullptr)
        {
            return Queued::No;
        }
        const QueueTaskFn queue = (*static_cast<const QueueTaskFn* const*>(graph))[0];
        if (queue == nullptr)
        {
            return Queued::No;
        }
        task->thread = *g_renderThread;
        // As the game's submit: the prerequisites outstanding drop from 1 to 0 (nothing else holds the task), then the
        // task is queued.
        task->prerequisites = 0;
        queue(graph, task, task->thread, kAnyThread);
        return Queued::Yes;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return Queued::Faulted;
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto clearState = platform::FindFunction(image, kClearState);
    const auto submit     = platform::FindFunction(image, kSubmit);
    const auto grow       = platform::FindFunction(image, kGrow);
    const auto vtable     = image.Rebase(kContextVtable, kSubmitSlot + sizeof(void*));
    const auto slot       = image.ReadPointer(kContextVtable + kSubmitSlot);
    const auto list       = image.Rebase(kImmediateList, kImmediateListEnd);
    const auto graph      = image.Rebase(kTaskGraph, sizeof(void*));
    const auto thread     = image.Rebase(kRenderThread, sizeof(std::uint32_t));
    const auto threaded   = image.Rebase(kThreadedRender, 1);
    if (!clearState || !submit || !grow || !vtable || !slot || *slot != *submit || !list || !graph || !thread || !threaded)
    {
        platform::LogWarning(L"family boundary: UNAVAILABLE (a check failed); every player stays in one view family");
        return false;
    }
    if (!platform::PinThisModule())
    {
        platform::LogWarning(L"family boundary: UNAVAILABLE (the DLL could not be pinned); every player stays in one view "
                             L"family");
        return false;
    }
    g_clearState    = reinterpret_cast<ContextFn>(*clearState);
    g_submit        = reinterpret_cast<ContextFn>(*submit);
    g_grow          = reinterpret_cast<boundary::GrowFn>(*grow);
    g_contextVtable = *vtable;
    g_list          = reinterpret_cast<std::byte*>(*list);
    g_taskGraph     = reinterpret_cast<void* const*>(*graph);
    g_renderThread  = reinterpret_cast<const std::uint32_t*>(*thread);
    g_threaded      = reinterpret_cast<const std::uint8_t*>(*threaded);
    g_installed.store(true, std::memory_order_release);  // publishes the facts above
    platform::Log(L"family boundary: installed (between two players' families the renderer's state is reset as at the "
                  L"end of a frame)");
    return true;
}

bool Ready() noexcept
{
    return g_installed.load(std::memory_order_acquire) && !g_faulted.load();
}

bool Queue(family_now::Family next, bool reset) noexcept
{
    if (!Ready())
    {
        return false;
    }
    std::uint8_t threaded = 0;
    if (!platform::CopyGuarded(&threaded, g_threaded, sizeof(threaded)))
    {
        Fault();
        return false;
    }
    if (threaded == 0)
    {
        Body(next, reset);  // the game's own rule: no rendering thread, the command runs now
        g_counts.direct.fetch_add(1, std::memory_order_relaxed);
        return Ready();     // the reset itself may have faulted
    }
    auto* const task = new (std::nothrow) Task{kTaskVtable};
    if (task == nullptr)
    {
        Fault();  // a family would go without its reset or its place
        return false;
    }
    task->next  = next;
    task->reset = reset;
    switch (QueueGuarded(task))
    {
        case Queued::Yes: break;
        case Queued::No:
            delete task;
            Fault();
            return false;
        case Queued::Faulted: Fault(); return false;
    }
    g_counts.queued.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void Poll() noexcept
{
    if (!g_installed.load(std::memory_order_acquire))
    {
        return;
    }
    if (g_faulted.load())
    {
        family_now::Set({});
    }
    if (g_faulted.load() && !g_faultLogged)
    {
        g_faultLogged = true;
        platform::LogWarning(L"family boundary: a call faulted; the reset is off for the rest of the session, and every "
                             L"player goes back to one view family");
    }
    const std::uint64_t now = GetTickCount64();
    if (now < g_logAt)
    {
        return;
    }
    g_logAt                    = now + kLogPeriod;
    const std::uint64_t queued = g_counts.queued.load(std::memory_order_relaxed) + g_counts.direct.load(std::memory_order_relaxed);
    if (queued == g_loggedQueued)
    {
        return;  // nothing new (no split frame)
    }
    g_loggedQueued = queued;
    try
    {
        platform::LogDebug(L"family boundary: {}", Describe());
    }
    catch (...)
    {
    }
}

std::wstring Describe()
{
    return std::format(L"{} commands queued, {} run at once; {} resets appended, {} run in place, {} done in stream order, "
                       L"{} skipped (not the default context){}",
                       g_counts.queued.load(std::memory_order_relaxed), g_counts.direct.load(std::memory_order_relaxed),
                       g_counts.appended.load(std::memory_order_relaxed), g_counts.inlined.load(std::memory_order_relaxed),
                       g_counts.reset.load(std::memory_order_relaxed), g_counts.skipped.load(std::memory_order_relaxed),
                       g_faulted.load() ? L"; FAULTED, off" : L"");
}

}  // namespace lss::render::family_boundary
