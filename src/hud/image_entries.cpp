#include "hud/image_entries.h"

#include "engine_facts.h"
#include "hud/image_code.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace lss::hud::image_entries {

namespace {

using DrawBatchFn = void (*)(void* renderer, void* commandList, void* target, void* batch, void* a5, void* a6, void* vb,
                             void* ib, void* params);
using MallocFn    = void* (*)(void* allocator, std::size_t size, std::uint32_t alignment);
using InitFn      = void (*)(void* resource);

platform::CallRedirect  g_drawCall;
platform::CodeRedirect  g_buffersCode;
alignas(16) image_code::EntryTable g_table;  // read by the render-thread code; entries written by the game thread
std::uintptr_t g_oneEntries[4] = {};          // player 1's pool-2 entries (game thread writes, render thread reads)
bool                    g_installed = false;

// Live addresses.
void* const*   g_malloc       = nullptr;  // &GMalloc
std::uintptr_t g_vertexVtable = 0;
std::uintptr_t g_indexVtable  = 0;

std::atomic<bool> g_requested{false};
std::atomic<bool> g_ready{false};
std::atomic<bool> g_failed{false};

// Render thread only.
int  g_view          = -1;     // the player whose view's UI pass is drawing now
bool g_sawPlayerTwo  = false;  // player 2's entry came first in this pass (the slot holds both)
std::uint32_t g_skipped[2] = {};  // draws skipped per view, for the log

/// One buffer the way FEndMenuRenderer's constructor makes pool 2's: 64 bytes, the game's vtable, +8 = 4, capacity
/// at +0x38, then InitResource (vt+0x28) on this, the render thread.
void* MakeBuffer(std::uintptr_t vtable, std::uint32_t capacity) noexcept
{
    void* allocator = nullptr;
    if (!platform::CopyGuarded(&allocator, g_malloc, sizeof(allocator)) || allocator == nullptr)
    {
        return nullptr;
    }
    std::uintptr_t malloc = 0;
    void**         table  = nullptr;
    if (!platform::CopyGuarded(&table, allocator, sizeof(table)) ||
        !platform::CopyGuarded(&malloc, reinterpret_cast<const std::uint8_t*>(table) + 0x10, sizeof(malloc)))
    {
        return nullptr;
    }
    void* buffer = nullptr;
    if (!platform::CallGuarded(reinterpret_cast<MallocFn>(malloc), buffer, allocator, std::size_t{64}, std::uint32_t{0}) ||
        buffer == nullptr)
    {
        return nullptr;
    }
    auto* bytes = static_cast<std::uint8_t*>(buffer);
    std::memset(bytes, 0, 64);
    std::memcpy(bytes, &vtable, sizeof(vtable));
    const std::uint32_t four = 4;
    std::memcpy(bytes + 8, &four, sizeof(four));
    std::memcpy(bytes + 0x38, &capacity, sizeof(capacity));
    std::uintptr_t init = 0;
    std::memcpy(&init, reinterpret_cast<const void*>(vtable + 0x28), sizeof(init));
    return platform::CallGuarded(reinterpret_cast<InitFn>(init), buffer) ? buffer : nullptr;
}

void MakeBuffers() noexcept
{
    void* vb = MakeBuffer(g_vertexVtable, 1024);
    void* ib = vb != nullptr ? MakeBuffer(g_indexVtable, 1536) : nullptr;
    if (vb == nullptr || ib == nullptr)
    {
        g_failed.store(true, std::memory_order_relaxed);
        platform::Log(L"HUD image: player 2's vertex/index buffers could not be made; player 2's image stays off");
        return;
    }
    void* rhiVertex = nullptr;
    void* rhiIndex  = nullptr;
    std::memcpy(&rhiVertex, static_cast<std::uint8_t*>(vb) + 0x30, sizeof(void*));
    std::memcpy(&rhiIndex, static_cast<std::uint8_t*>(ib) + 0x30, sizeof(void*));
    g_table.vertexBuffer = reinterpret_cast<std::uintptr_t>(vb);
    g_table.indexBuffer  = reinterpret_cast<std::uintptr_t>(ib);
    g_ready.store(true, std::memory_order_release);  // after the table: the game thread records entries only then
    platform::Log(L"HUD image: player 2's vertex/index buffers made on the render thread (RHI buffers {} / {})", rhiVertex,
                  rhiIndex);
}

/// Whether `batch` is one of `entries`' batches: the draw call is given [entry+0x80] + 0x50 (RE-hud-two-images.md §2.3).
bool BatchOf(const std::uintptr_t (&entries)[4], const void* batch) noexcept
{
    for (const std::uintptr_t& slot : entries)
    {
        const std::uintptr_t entry = reinterpret_cast<const std::atomic<std::uintptr_t>&>(slot).load(std::memory_order_acquire);
        std::uintptr_t       own   = 0;
        if (entry != 0 && platform::CopyGuarded(&own, reinterpret_cast<const void*>(entry + 0x80), sizeof(own)) &&
            own != 0 && own + 0x50 == reinterpret_cast<std::uintptr_t>(batch))
        {
            return true;
        }
    }
    return false;
}

/// Records `entry` in `entries` once (game thread). False when all four are taken by others.
bool Record(std::uintptr_t (&entries)[4], std::uintptr_t entry) noexcept
{
    for (const std::uintptr_t known : entries)
    {
        if (known == entry)
        {
            return true;
        }
    }
    for (auto& slot : entries)
    {
        if (slot == 0)
        {
            reinterpret_cast<std::atomic<std::uintptr_t>&>(slot).store(entry, std::memory_order_release);
            platform::LogDebug(L"HUD image: player {}'s layer entry {:#x} recorded", &entries == &g_table.entries ? 2 : 1, entry);
            return true;
        }
    }
    return false;
}

/// Replaces DrawLayerEntry's call to FEndMenuRenderer__DrawEntryBatchToTarget (render thread).
void DrawBatch(void* renderer, void* commandList, void* target, void* batch, void* a5, void* a6, void* vb, void* ib,
               void* params)
{
    // Only the two players' pool-2 entries are gated: the same draw serves pool 0 (the navi map's texture) and pool 1
    // (the offscreen overlay), whose entries every view needs.
    if (g_view >= 0)
    {
        const bool playerTwo = BatchOf(g_table.entries, batch);
        const bool playerOne = !playerTwo && BatchOf(g_oneEntries, batch);
        g_sawPlayerTwo       = g_sawPlayerTwo || playerTwo;
        // Each view draws its own player's entry; player 1's is drawn in player 2's view only when the slot holds no
        // entry of player 2's (a frame from before the image started).
        if ((g_view == 0 && playerTwo) || (g_view == 1 && playerOne && g_sawPlayerTwo))
        {
            ++g_skipped[g_view];
            return;
        }
    }
    reinterpret_cast<DrawBatchFn>(g_drawCall.Original())(renderer, commandList, target, batch, a5, a6, vb, ib, params);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto site     = image.Rebase(image_code::kBuffersSite, sizeof(image_code::kBuffersLoad));
    const auto back     = image.Rebase(image_code::kBuffersResume);
    const auto drawCall = image.Rebase(facts::kDrawEntryBatchCall, 5);
    const auto drawFn   = image.Rebase(facts::kDrawEntryBatchToTarget);
    const auto malloc   = image.Rebase(facts::kGMallocGlobal, sizeof(void*));
    const auto vertex   = image.Rebase(facts::kMenuVertexBufferVtable, 0x30);
    const auto index    = image.Rebase(facts::kMenuIndexBufferVtable, 0x30);
    if (!site || !back || !drawCall || !drawFn || !malloc || !vertex || !index ||
        !image.BytesMatch(image_code::kBuffersCheckStart, image_code::kBuffersBytes))
    {
        platform::Log(L"HUD image: the layer draw is not what the research read; player 2's image stays off");
        return false;
    }
    g_malloc       = reinterpret_cast<void* const*>(*malloc);
    g_vertexVtable = *vertex;
    g_indexVtable  = *index;
    if (!g_drawCall.Install(*drawCall, *drawFn, reinterpret_cast<std::uintptr_t>(&DrawBatch)))
    {
        platform::Log(L"HUD image: the layer draw's call could not be redirected; player 2's image stays off");
        return false;
    }
    if (!g_buffersCode.Install(*site, image_code::kBuffersLoad, image_code::BuffersCode(g_table), *back))
    {
        g_drawCall.Restore();
        platform::Log(L"HUD image: the layer draw's buffer load could not be redirected; player 2's image stays off");
        return false;
    }
    g_installed = true;
    return true;
}

void Remove() noexcept
{
    if (!g_installed)
    {
        return;
    }
    // The table empties first: whatever stays in place then treats every entry as player 1's.
    for (auto& entry : g_table.entries)
    {
        entry = 0;
    }
    const bool ok = g_buffersCode.Restore() & g_drawCall.Restore();
    if (!ok)
    {
        platform::Log(L"HUD image: a layer draw redirect was changed after us; left in place (it acts on no entry now)");
    }
    g_installed = !ok;
}

bool BuffersReady() noexcept
{
    return g_installed && g_ready.load(std::memory_order_acquire);
}

void RequestBuffers() noexcept
{
    if (g_installed && !g_failed.load(std::memory_order_relaxed))
    {
        g_requested.store(true, std::memory_order_relaxed);
    }
}

void RecordEntry(int player, void* entry) noexcept
{
    const auto value = reinterpret_cast<std::uintptr_t>(entry);
    if (value == 0 || !BuffersReady())
    {
        return;
    }
    if (!Record(player == 2 ? g_table.entries : g_oneEntries, value))
    {
        platform::LogWarning(L"HUD image: a fifth layer entry for player {} ({}); it is not gated by view", player, entry);
    }
}

void BeginUiPass(int player) noexcept
{
    if (g_requested.load(std::memory_order_relaxed) && !g_ready.load(std::memory_order_relaxed) &&
        !g_failed.load(std::memory_order_relaxed))
    {
        MakeBuffers();
    }
    g_view         = player;
    g_sawPlayerTwo = false;
}

void EndUiPasses() noexcept
{
    g_view = -1;
}

}  // namespace lss::hud::image_entries
