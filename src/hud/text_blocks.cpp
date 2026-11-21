#include "hud/text_blocks.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/sparse_array.h"
#include "platform/verified_code.h"

#include <bit>
#include <cstddef>
#include <cstring>
#include <intrin.h>

namespace lss::hud::text_blocks {

namespace {

using label_states::Parked;

using RelayoutFn  = void (*)(void* block);
using FreeArrayFn = void (*)(void* array);
using ReleaseFn   = void (*)(void* controller);

constexpr std::int32_t kMaxLiveBits = 1 << 16;  // the bound for the live set's bit count (hundreds of blocks): a bad read

// Set once at install.
bool           g_installed        = false;
std::uintptr_t g_textBlockVTable  = 0;
std::uintptr_t g_paintVTable      = 0;
std::uintptr_t g_gcObjectVTable   = 0;
std::uintptr_t g_controllerVTable = 0;
std::uintptr_t g_liveSet          = 0;
RelayoutFn     g_relayout         = nullptr;
FreeArrayFn    g_freeArray        = nullptr;
ReleaseFn      g_releaseWeak      = nullptr;

template <class T>
bool ReadAt(const void* base, std::uint32_t offset, T& out) noexcept
{
    return platform::Read(static_cast<const std::byte*>(base) + offset, out);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (g_installed)
    {
        return true;
    }
    const auto relayout   = platform::FindFunction(image, facts::kSlateTextRelayout);
    const auto freeArray  = platform::FindFunction(image, facts::kFreeNestedArray);
    const auto release    = platform::FindFunction(image, facts::kReleaseWeak);
    const auto textBlock  = image.Rebase(facts::kSlateTextVTable, sizeof(void*));
    const auto paint      = image.Rebase(facts::kSlateTextPaintVTable, sizeof(void*));
    const auto gcObject   = image.Rebase(facts::kSlateTextGcVTable, sizeof(void*));
    const auto controller = image.Rebase(facts::kSlateTextControllerVTable, sizeof(void*));
    const auto liveSet    = image.Rebase(facts::kSlateTextLiveSet, platform::kSparseArrayNumBits + sizeof(std::int32_t));
    if (!relayout || !freeArray || !release || !textBlock || !paint || !gcObject || !controller || !liveSet)
    {
        platform::LogWarning(L"text blocks: the text block's class, relayout or live set is not what the research read");
        return false;
    }
    g_textBlockVTable  = *textBlock;
    g_paintVTable      = *paint;
    g_gcObjectVTable   = *gcObject;
    g_controllerVTable = *controller;
    g_liveSet          = *liveSet;
    g_relayout         = reinterpret_cast<RelayoutFn>(*relayout);
    g_freeArray        = reinterpret_cast<FreeArrayFn>(*freeArray);
    g_releaseWeak      = reinterpret_cast<ReleaseFn>(*release);
    g_installed        = true;
    return true;
}

void* ControllerOf(void* block) noexcept
{
    return static_cast<std::byte*>(block) - facts::kSlateTextControllerBefore;
}

const wchar_t* Unswappable(void* block) noexcept
{
    const void*    controller = ControllerOf(block);
    std::uintptr_t vtable = 0, paint = 0, gc = 0, weakObject = 0, weakCounter = 0, controllerVTable = 0;
    if (!g_installed || !ReadAt(block, 0, vtable) || vtable != g_textBlockVTable ||
        !ReadAt(block, facts::kSlateTextPaintInterface, paint) || paint != g_paintVTable ||
        !ReadAt(block, facts::kSlateTextGcObject, gc) || gc != g_gcObjectVTable)
    {
        return L"not an SEndTextBlock";
    }
    if (!ReadAt(block, facts::kSlateTextWeakThis, weakObject) || weakObject != reinterpret_cast<std::uintptr_t>(block) ||
        !ReadAt(block, facts::kSlateTextWeakThis + sizeof(void*), weakCounter) ||
        weakCounter != reinterpret_cast<std::uintptr_t>(controller) || !ReadAt(controller, 0, controllerVTable) ||
        controllerVTable != g_controllerVTable)
    {
        return L"not in its own MakeShared block";
    }
    if (!Alive(ControllerOf(block)))
    {
        return L"no shared reference";
    }
    return nullptr;
}

bool Alive(void* controller) noexcept
{
    std::int32_t shared = 0;
    return ReadAt(controller, facts::kSharedReferenceCount, shared) && shared > 0;
}

bool SwapLayout(void* block, Parked& parked) noexcept
{
    auto* const w    = static_cast<std::byte*>(block);
    Parked      held = parked;
    if (!platform::CopyGuarded(held.runs, w + label_states::kRunsOffset, label_states::kRunsSize) ||
        !platform::CopyGuarded(held.flags, w + label_states::kFlagsOffset, label_states::kFlagsSize) ||
        !platform::CopyGuarded(held.quads, w + label_states::kQuadsOffset, label_states::kQuadsSize))
    {
        return false;
    }
    // The writes go to the addresses just read.
    platform::WriteGuarded(w + label_states::kRunsOffset, parked.runs, label_states::kRunsSize);
    platform::WriteGuarded(w + label_states::kFlagsOffset, parked.flags, label_states::kFlagsSize);
    platform::WriteGuarded(w + label_states::kQuadsOffset, parked.quads, label_states::kQuadsSize);
    parked = held;
    return true;
}

std::uint64_t FontStamp(void* block) noexcept
{
    std::uint64_t font = 0, secondFont = 0;
    ReadAt(block, facts::kSlateTextFont, font);
    ReadAt(block, facts::kSlateTextFontTwo, secondFont);
    return font ^ std::rotl(secondFont, 21);
}

bool HoldWeak(void* controller) noexcept
{
    std::int32_t weak = 0;
    if (!ReadAt(controller, facts::kWeakReferenceCount, weak) || weak <= 0)
    {
        return false;  // the widget's own WeakThis holds one: zero would be a block already going away
    }
    // Atomic, as the game's own release is.
    _InterlockedIncrement(reinterpret_cast<volatile long*>(static_cast<std::byte*>(controller) + facts::kWeakReferenceCount));
    return true;
}

void ReleaseWeak(void* controller) noexcept
{
    platform::CallGuarded(g_releaseWeak, controller);
}

void FreeParked(Parked& parked) noexcept
{
    bool freed = true;
    // As the destructor: the quad arrays from the last, then the glyph page arrays from the last.
    for (std::uint32_t i = facts::kSlateTextQuadArrays; i-- > 0;)
    {
        freed = platform::CallGuarded(g_freeArray, static_cast<void*>(parked.quads + i * facts::kArrayHeaderBytes)) && freed;
    }
    for (std::uint32_t i = facts::kSlateTextRunArrays; i-- > 0;)
    {
        freed = platform::CallGuarded(g_freeArray, static_cast<void*>(parked.runs + i * facts::kArrayHeaderBytes)) && freed;
    }
    if (!freed)
    {
        platform::Log(L"text blocks: freeing a set-aside state raised an exception");
    }
    parked = label_states::EmptyState();
}

bool Relayout(void* block) noexcept
{
    return g_installed && platform::CallGuarded(g_relayout, block);
}

void ForEachLive(bool (*visit)(void* block, void* context) noexcept, void* context) noexcept
{
    if (!g_installed)
    {
        return;
    }
    try
    {
        platform::ForEachSparseElement(g_liveSet, facts::kSlateTextLiveSetElement, kMaxLiveBits, [&](std::uintptr_t element) {
            void* block = nullptr;
            return !platform::Read(reinterpret_cast<const void*>(element), block) || block == nullptr || visit(block, context);
        });
    }
    catch (...)
    {
    }
}

}  // namespace lss::hud::text_blocks
