#include "hud/image_entries.h"

#include "engine_facts.h"
#include "hud/image_code.h"
#include "hud/overlay_pass.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/game_globals.h"

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
// The further columns' tables (column c's is g_tables[c - 1]): read by the render-thread code; entries written by the
// game thread, the buffer pairs by the render thread before g_ready.
alignas(16) image_code::EntryTables g_tables;
std::uintptr_t g_columnZero[4] = {};          // column 0's pool-2 entries (game thread writes, render thread reads)
bool                    g_installed = false;
bool                    g_ungatedLogged = false;  // a column beyond the tables, told once
constexpr int           kFurther = static_cast<int>(image_code::kTables);  // further columns with a table
static_assert(kFurther + 1 == overlay_pass::kMaxColumns, "a table per further column");

// Live addresses.
void* const*   g_malloc       = nullptr;  // &GMalloc
std::uintptr_t g_vertexVtable = 0;
std::uintptr_t g_indexVtable  = 0;

std::atomic<bool> g_requested{false};
std::atomic<bool> g_ready{false};
std::atomic<bool> g_failed{false};

// Render thread only.
int  g_view         = -1;     // the column whose view's UI pass is drawing now
bool g_sawFurther   = false;  // a further column's entry has been seen in this view's pass (the slot holds every column's)
std::uint32_t g_skipped[overlay_pass::kMaxColumns] = {};  // draws skipped per view, for the log

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
        !platform::CopyGuarded(&malloc, reinterpret_cast<const std::uint8_t*>(table) + facts::kMallocSlotMalloc, sizeof(malloc)))
    {
        return nullptr;
    }
    void* buffer = nullptr;
    if (!platform::CallGuarded(reinterpret_cast<MallocFn>(malloc), buffer, allocator, std::size_t{facts::kRenderBufferBytes}, std::uint32_t{0}) ||
        buffer == nullptr)
    {
        return nullptr;
    }
    auto* bytes = static_cast<std::uint8_t*>(buffer);
    std::memset(bytes, 0, facts::kRenderBufferBytes);
    std::memcpy(bytes, &vtable, sizeof(vtable));
    const std::uint32_t four = 4;
    std::memcpy(bytes + facts::kRenderBufferWord8, &four, sizeof(four));
    std::memcpy(bytes + facts::kRenderBufferCapacity, &capacity, sizeof(capacity));
    std::uintptr_t init = 0;
    std::memcpy(&init, reinterpret_cast<const void*>(vtable + facts::kRenderBufferInitSlot), sizeof(init));
    return platform::CallGuarded(reinterpret_cast<InitFn>(init), buffer) ? buffer : nullptr;
}

/// Every further column's pair, at once. Column 1's failing stops every further image; a later column's failing keeps
/// the columns before it (that column's entries are then never recorded: its view draws as the image-less view).
void MakeBuffers() noexcept
{
    for (int t = 0; t < kFurther; ++t)
    {
        image_code::EntryTable& table = g_tables[static_cast<std::size_t>(t)];
        if (table.vertexBuffer != 0 && table.indexBuffer != 0)
        {
            continue;
        }
        void* vb = MakeBuffer(g_vertexVtable, 1024);
        void* ib = vb != nullptr ? MakeBuffer(g_indexVtable, 1536) : nullptr;
        if (vb == nullptr || ib == nullptr)
        {
            if (t == 0)
            {
                g_failed.store(true, std::memory_order_relaxed);
                platform::Log(L"HUD image: column 2's vertex/index buffers could not be made; the further players' images stay off");
                return;
            }
            platform::Log(L"HUD image: column {}'s vertex/index buffers could not be made; that column's image stays off", t + 2);
            break;
        }
        void* rhiVertex = nullptr;
        void* rhiIndex  = nullptr;
        std::memcpy(&rhiVertex, static_cast<std::uint8_t*>(vb) + 0x30, sizeof(void*));
        std::memcpy(&rhiIndex, static_cast<std::uint8_t*>(ib) + 0x30, sizeof(void*));
        table.vertexBuffer = reinterpret_cast<std::uintptr_t>(vb);
        table.indexBuffer  = reinterpret_cast<std::uintptr_t>(ib);
        platform::Log(L"HUD image: column {}'s vertex/index buffers made on the render thread (RHI buffers {} / {})", t + 2,
                      rhiVertex, rhiIndex);
    }
    g_ready.store(true, std::memory_order_release);  // after the tables: the game thread records entries only then
}

/// Whether `batch` is one of `entries`' batches: the draw call is given [entry+0x80] + 0x50 (RE-hud-two-images.md).
bool BatchOf(const std::uintptr_t (&entries)[4], const void* batch) noexcept
{
    for (const std::uintptr_t& slot : entries)
    {
        const std::uintptr_t entry = reinterpret_cast<const std::atomic<std::uintptr_t>&>(slot).load(std::memory_order_acquire);
        std::uintptr_t       own   = 0;
        if (entry != 0 && platform::CopyGuarded(&own, reinterpret_cast<const void*>(entry + facts::kLayerEntryBatches), sizeof(own)) &&
            own != 0 && own + facts::kBatchesDrawn == reinterpret_cast<std::uintptr_t>(batch))
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
            platform::LogDebug(L"HUD image: a layer entry {:#x} recorded ({})", entry,
                               &entries == &g_columnZero ? L"player 1's column" : L"a further column");
            return true;
        }
    }
    return false;
}

/// Replaces DrawLayerEntry's call to FEndMenuRenderer__DrawEntryBatchToTarget (render thread).
void DrawBatch(void* renderer, void* commandList, void* target, void* batch, void* a5, void* a6, void* vb, void* ib,
               void* params)
{
    // Only the columns' pool-2 entries are gated: the same draw serves pool 0 (the navi map's texture) and pool 1
    // (the offscreen overlay), whose entries every view needs.
    if (g_view >= 0)
    {
        int owner = BatchOf(g_columnZero, batch) ? 0 : -1;
        for (int t = 0; owner < 0 && t < kFurther; ++t)
        {
            owner = BatchOf(g_tables[static_cast<std::size_t>(t)].entries, batch) ? t + 1 : -1;
        }
        g_sawFurther = g_sawFurther || owner >= 1;
        if (overlay_pass::DrawSkipped(g_view, owner, g_sawFurther))
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
    const auto malloc   = players::game_globals::Address(players::game_globals::Global::Malloc);
    const auto vertex   = image.Rebase(facts::kMenuVertexBufferVtable, 0x30);
    const auto index    = image.Rebase(facts::kMenuIndexBufferVtable, 0x30);
    if (!site || !back || !drawCall || !drawFn || !malloc || !vertex || !index ||
        !image.BytesMatch(image_code::kBuffersCheckStart, image_code::kBuffersBytes))
    {
        platform::Log(L"HUD image: the layer draw is not what the research read; the further players' images stay off");
        return false;
    }
    g_malloc       = reinterpret_cast<void* const*>(malloc);
    g_vertexVtable = *vertex;
    g_indexVtable  = *index;
    if (!g_drawCall.Install(*drawCall, *drawFn, reinterpret_cast<std::uintptr_t>(&DrawBatch)))
    {
        platform::Log(L"HUD image: the layer draw's call could not be redirected; the further players' images stay off");
        return false;
    }
    if (!g_buffersCode.Install(*site, image_code::kBuffersLoad, image_code::BuffersCode(g_tables), *back))
    {
        g_drawCall.Restore();
        platform::Log(L"HUD image: the layer draw's buffer load could not be redirected; the further players' images stay off");
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
    // The tables empty first: whatever stays in place then treats every entry as column 0's.
    for (auto& table : g_tables)
    {
        for (auto& entry : table.entries)
        {
            entry = 0;
        }
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

void RecordEntry(int column, void* entry) noexcept
{
    const auto value = reinterpret_cast<std::uintptr_t>(entry);
    if (value == 0 || !BuffersReady() || column < 0)
    {
        return;
    }
    if (column > kFurther)
    {
        if (!g_ungatedLogged)
        {
            g_ungatedLogged = true;
            platform::LogWarning(L"HUD image: column {}'s layer entry ({}) has no buffer pair of its own (only {} further columns "
                                 L"are built); it is not gated by view",
                                 column, entry, kFurther);
        }
        return;
    }
    if (column >= 1 && g_tables[static_cast<std::size_t>(column - 1)].vertexBuffer == 0)
    {
        return;  // the column has no pair (its making failed, logged): its entry is never drawn from another's
    }
    if (!Record(column == 0 ? g_columnZero : g_tables[static_cast<std::size_t>(column - 1)].entries, value))
    {
        platform::LogWarning(L"HUD image: a fifth layer entry for column {} ({}); it is not gated by view", column, entry);
    }
}

void BeginUiPass(int column) noexcept
{
    if (g_requested.load(std::memory_order_relaxed) && !g_ready.load(std::memory_order_relaxed) &&
        !g_failed.load(std::memory_order_relaxed))
    {
        MakeBuffers();
    }
    // A table per further column; a view beyond them (none while the columns built match) draws as the last one's.
    g_view       = column > kFurther ? kFurther : column;
    g_sawFurther = false;
}

void EndUiPasses() noexcept
{
    g_view = -1;
}

}  // namespace lss::hud::image_entries
