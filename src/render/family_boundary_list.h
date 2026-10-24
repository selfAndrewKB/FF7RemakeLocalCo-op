#pragma once

// The RHI command list and context layout the family boundary relies on (render/family_boundary.h; RE-haze-occlusion.md
// Pass 10 I.1.4, I.1.5). Pure logic over the game's memory (tests/family_boundary_list_test.cpp).
//
// FRHICommandListBase (stock 4.18 roles, this build's offsets):
//   +0x00 Root            the first node
//   +0x08 CommandLink     where the next node's address goes: &Root when empty, else the last node (its Next is at +0)
//   +0x10 bExecuting      set while the list is walked in place (only without an RHI thread)
//   +0x14 NumCommands
//   +0x20 Context         the command context the walk executes on (RHIGetDefaultContext)
//   +0x30 / +0x38         the arena: cursor, end
// A node is {Next, ExecuteAndDestruct}: 4.18 nodes hold a function pointer, called as fn(list, node), with Next read
// before the call (FRHICommandListExecutor::ExecuteInner 0x1426E0560). Rebirth's 4.26 nodes hold a vtable instead.
//
// The game appends a node inline in every command (there is no callable "append"); AppendNode is that sequence written
// out once, byte for byte as the game's own mid-family submit node in FenceOcclusionTests (0x1421E6280..0x1421E62EA):
// align the cursor to 8; when the node does not fit (or the arena is empty), grow the arena through the game's
// allocator (0x141CB8DA0, passed the node size + 8 as the game passes it) and align again; then advance the cursor,
// count the command, link the node and fill it.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss::render::boundary {

namespace list {
inline constexpr std::size_t kCommandLink = 0x08;
inline constexpr std::size_t kExecuting   = 0x10;
inline constexpr std::size_t kNumCommands = 0x14;
inline constexpr std::size_t kContext     = 0x20;
inline constexpr std::size_t kArena       = 0x30;  // the cursor; the end follows at +8
}  // namespace list

namespace context {
inline constexpr std::size_t kDevice         = 0x08;    // FD3D12Device*
inline constexpr std::size_t kIsAsyncCompute = 0x2AEC;  // bIsAsyncComputeContext (byte)
/// device: the direct contexts (a TArray's data); [0] is the default, the one RHIEndFrame clears.
inline constexpr std::size_t kDeviceDirect = 0x690;
}  // namespace context

inline constexpr std::size_t  kNodeSize    = 0x10;  // {Next, ExecuteAndDestruct}
inline constexpr std::int32_t kGrowRequest = kNodeSize + 8;

/// The arena's grow (0x141CB8DA0): gives the arena a new block, rewriting its cursor and end.
using GrowFn = void (*)(void* arena, std::int32_t bytes);
/// A node's ExecuteAndDestruct(list, node).
using NodeFn = void (*)(void* commandList, void* node);

/// A field, unguarded: the callers run inside family_boundary.cpp's guards.
template <typename T>
T Load(const std::byte* at, std::size_t offset) noexcept
{
    T value{};
    std::memcpy(&value, at + offset, sizeof(value));
    return value;
}

template <typename T>
void Write(std::byte* at, std::size_t offset, T value) noexcept
{
    std::memcpy(at + offset, &value, sizeof(value));
}

inline bool IsExecuting(const std::byte* commandList) noexcept
{
    return Load<std::uint8_t>(commandList, list::kExecuting) != 0;
}

/// Appends a node holding `fn` to the end of `commandList`; returns the node's address.
inline std::uintptr_t AppendNode(std::byte* commandList, NodeFn fn, GrowFn grow) noexcept
{
    const auto aligned = [commandList] {
        return (Load<std::uint64_t>(commandList, list::kArena) + 7) & ~std::uint64_t{7};
    };
    std::uint64_t node = aligned();
    if (node == 0 || node + kNodeSize > Load<std::uint64_t>(commandList, list::kArena + 8))
    {
        grow(commandList + list::kArena, kGrowRequest);
        node = aligned();
    }
    Write<std::uint64_t>(commandList, list::kArena, node + kNodeSize);
    Write<std::uint32_t>(commandList, list::kNumCommands, Load<std::uint32_t>(commandList, list::kNumCommands) + 1);
    auto* const link = reinterpret_cast<std::byte*>(Load<std::uint64_t>(commandList, list::kCommandLink));
    Write<std::uint64_t>(link, 0, node);
    Write<std::uint64_t>(commandList, list::kCommandLink, node);
    auto* const bytes = reinterpret_cast<std::byte*>(node);
    Write<std::uint64_t>(bytes, 0, 0);  // Next
    Write<NodeFn>(bytes, 8, fn);
    return node;
}

/// True if `ctx` is the device's default direct context (the one RHIEndFrame clears): an FD3D12CommandContext
/// (`contextVtable`, not the multi-GPU redirector), not async compute, and the first of its device's direct contexts.
inline bool IsDefaultDirectContext(const std::byte* ctx, std::uintptr_t contextVtable) noexcept
{
    if (ctx == nullptr || Load<std::uintptr_t>(ctx, 0) != contextVtable ||
        Load<std::uint8_t>(ctx, context::kIsAsyncCompute) != 0)
    {
        return false;
    }
    const auto* device = reinterpret_cast<const std::byte*>(Load<std::uintptr_t>(ctx, context::kDevice));
    const auto* direct = device != nullptr
                             ? reinterpret_cast<const std::byte*>(Load<std::uintptr_t>(device, context::kDeviceDirect))
                             : nullptr;
    return direct != nullptr && Load<const std::byte*>(direct, 0) == ctx;
}

}  // namespace lss::render::boundary
