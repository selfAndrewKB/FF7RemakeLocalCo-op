#include "render/ribbon_fill.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/perf_counter.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

namespace lss::render::ribbon_fill {

namespace {

// The largest fill worth mirroring: a count past it is left to the game's own path rather than grown into.
constexpr std::size_t kLargestMirrored = 64u << 20;
// Room past the counted vertices, its head filled with a known pattern: a fill that writes past its count lands there,
// is reported once, and turns the mirroring off (an overrun longer than the slack reaches past it: the guard bounds
// what is caught, not what is written). The counted bytes are valid either way and are copied first.
constexpr std::size_t  kSlackBytes = 4096;
constexpr std::size_t  kGuardBytes = 64;
constexpr std::uint8_t kGuardByte  = 0xA5;

using FillVertexData = std::int64_t(__fastcall*)(void* emitterData, std::uint8_t* fillData);

platform::VTableEntry      g_slot;
std::atomic<bool>          g_installed{false};
std::atomic<bool>          g_mirrored{true};  // `ribbon-fill`: fill in the mod's memory, then copy
std::atomic<std::uint64_t> g_ticks{0};
std::atomic<std::uint64_t> g_vertices{0};
std::atomic<std::uint64_t> g_fills{0};

/// The thread's scratch, at least `bytes` long; null if it cannot grow (the caller then lets the game fill directly).
std::uint8_t* Scratch(std::size_t bytes) noexcept
{
    thread_local std::vector<std::uint8_t> scratch;
    if (scratch.size() < bytes)
    {
        try
        {
            scratch.resize(bytes);
        }
        catch (const std::bad_alloc&)
        {
            return nullptr;
        }
    }
    return scratch.data();
}

template <class T>
T Field(const std::uint8_t* fillData, std::uint32_t offset) noexcept
{
    T value{};
    std::memcpy(&value, fillData + offset, sizeof(T));
    return value;
}

/// Whether the guard after `bytes` still holds its pattern.
bool GuardHolds(const std::uint8_t* scratch, std::size_t bytes) noexcept
{
    for (std::size_t i = 0; i < kGuardBytes; ++i)
    {
        if (scratch[bytes + i] != kGuardByte)
        {
            return false;
        }
    }
    return true;
}

/// The slot's replacement, on the render thread (the game's DoBufferFill calls it only with a count, a vertex buffer and
/// an index buffer). The fill writes each point's vertices in order and reads back only the positions it has just
/// written, so a fill into the scratch followed by one copy leaves the game's buffer with the bytes its own fill would.
/// The switch is read once a call, so turning it mid-frame changes only the fills after.
std::int64_t __fastcall Fill(void* emitterData, std::uint8_t* fillData)
{
    const auto         original = reinterpret_cast<FillVertexData>(g_slot.Original());
    const std::int64_t start    = platform::Ticks();
    const auto         count    = Field<std::int32_t>(fillData, facts::kBufferFillVertexCount);
    const auto         size     = Field<std::int32_t>(fillData, facts::kBufferFillVertexSize);
    auto* const        target   = Field<std::uint8_t*>(fillData, facts::kBufferFillVertexData);
    const std::size_t  bytes =
        count > 0 && size > 0 ? static_cast<std::size_t>(count) * static_cast<std::size_t>(size) : 0;
    std::uint8_t* scratch = g_mirrored.load(std::memory_order_relaxed) && target != nullptr && bytes != 0 &&
                                    bytes <= kLargestMirrored
                                ? Scratch(bytes + kSlackBytes)
                                : nullptr;
    std::int64_t result = 0;
    if (scratch != nullptr)
    {
        std::memset(scratch + bytes, kGuardByte, kGuardBytes);
        std::memcpy(fillData + facts::kBufferFillVertexData, &scratch, sizeof(scratch));
        result = original(emitterData, fillData);
        std::memcpy(fillData + facts::kBufferFillVertexData, &target, sizeof(target));
        std::memcpy(target, scratch, bytes);
        if (!GuardHolds(scratch, bytes) && g_mirrored.exchange(false, std::memory_order_relaxed))
        {
            platform::LogWarning(L"ribbon fill: a fill of {} vertices wrote past its count; the game fills its buffer "
                                 L"directly from now on (ribbon-fill off)",
                                 count);
        }
    }
    else
    {
        result = original(emitterData, fillData);
    }
    g_ticks.fetch_add(static_cast<std::uint64_t>(platform::Ticks() - start), std::memory_order_relaxed);
    g_vertices.fetch_add(count > 0 ? static_cast<std::uint64_t>(count) : 0, std::memory_order_relaxed);
    g_fills.fetch_add(1, std::memory_order_relaxed);
    return result;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
try
{
    if (!platform::ReplaceSlot(image, facts::kRibbonFillVertexSlot, reinterpret_cast<std::uintptr_t>(&Fill), g_slot))
    {
        platform::LogWarning(L"ribbon fill: not installed (the vertex fill's slot is not as read); ribbon trails stay the game's");
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    control::Register({.name  = L"ribbon-fill",
                       .shape = control::Shape::OnOff,
                       .help  = L"whether Square Enix's ribbon trails are filled in the mod's memory and copied to the GPU in "
                                L"one pass (on at start; off: the game fills its GPU buffer directly, reading each point back)",
                       .set   = [](const control::Value& v) {
                           g_mirrored.store(std::get<bool>(v), std::memory_order_relaxed);
                           return control::Outcome{true, std::get<bool>(v) ? L"ribbon trails filled in the mod's memory, then copied"
                                                                           : L"ribbon trails filled by the game, in place"};
                       },
                       .get = [] { return control::Value{g_mirrored.load(std::memory_order_relaxed)}; }});
    platform::Log(L"ribbon fill: installed; ribbon trails are filled in the mod's memory and copied to the GPU in one pass "
                  L"(ribbon-fill off: the game's own fill), each fill counted for the frame watch");
    return true;
}
catch (...)
{
    platform::LogWarning(L"ribbon fill: could not be installed (out of memory)");
    return false;
}

bool Installed() noexcept
{
    return g_installed.load(std::memory_order_acquire);
}

Totals Read() noexcept
{
    return Totals{g_ticks.load(std::memory_order_relaxed), g_vertices.load(std::memory_order_relaxed),
                  g_fills.load(std::memory_order_relaxed)};
}

}  // namespace lss::render::ribbon_fill
