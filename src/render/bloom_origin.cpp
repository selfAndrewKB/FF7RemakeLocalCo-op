#include "render/bloom_origin.h"

#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "render/bloom_reduce_shader.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::render::bloom_origin {

namespace {

namespace shader = bloom_reduce_shader;

using BindFn           = void* (*)(void* context, void* vertexShaderOut, const float* sourceRect, const float* destRect);
using GetPipelineFn    = void* (*)(void* commandList, const void* initializer, std::uint32_t applyFlags);
using CreatePsRenderFn = void** (*)(void* rhi, void** out, void* immediateList, const void* code);

/// TArray<uint8> as the engine lays it out.
struct ByteArray
{
    const std::uint8_t* data;
    std::int32_t        num;
    std::int32_t        max;
};

// Set once at install, on the game thread, before any redirect is live; read on the render thread afterwards.
// The redirects' atomic 16-byte code swaps order the writes before any read (x64).
platform::CallRedirect g_bindCall;
platform::CallRedirect g_bindExactCall;
platform::CallRedirect g_pipelineCall;
platform::CallRedirect g_pipelineExactCall;
GetPipelineFn          g_getPipeline      = nullptr;
std::uintptr_t         g_rhiGlobal        = 0;  // &GDynamicRHI
std::uintptr_t         g_immediateList    = 0;  // &GRHICommandList_Immediate
std::uintptr_t         g_d3d12PixelVtable = 0;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_enabled{true};

// The binders and the pipeline calls run on the render thread [ASSUMPTION, strong: RE-impl-facts-bloom-swap.md §7;
// the first swap logs its thread]. The state is atomic and the creation locked all the same, so a second thread
// could at worst wait for the creation, never read a half-written shader pointer or create a second one.
std::atomic<void*> g_fixedShader{nullptr};  // FRHIPixelShader*, our reference, held for the rest of the process
std::atomic<bool>  g_createFailed{false};
std::atomic<bool>  g_loggedSwap{false};
std::atomic<bool>  g_loggedOther{false};
std::mutex         g_createMutex;

// The view whose level-0 reduce is being bound, on the thread binding it (the render thread).
thread_local const std::uint8_t* t_view = nullptr;

void* Bind(const platform::CallRedirect& call, void* context, void* vertexShaderOut, const float* sourceRect,
           const float* destRect)
{
    const std::uint8_t* view = nullptr;
    platform::CopyGuarded(&view, static_cast<const std::uint8_t*>(context) + facts::kPassContextView, sizeof(view));
    t_view            = view;
    void* const result = reinterpret_cast<BindFn>(call.Original())(context, vertexShaderOut, sourceRect, destRect);
    t_view            = nullptr;
    return result;
}

void* BindFirst(void* context, void* vertexShaderOut, const float* sourceRect, const float* destRect)
{
    return Bind(g_bindCall, context, vertexShaderOut, sourceRect, destRect);
}

void* BindFirstExact(void* context, void* vertexShaderOut, const float* sourceRect, const float* destRect)
{
    return Bind(g_bindExactCall, context, vertexShaderOut, sourceRect, destRect);
}

/// Where the original shader's DXBC lies in the code blob its D3D12 shader object keeps.
struct OriginalCode
{
    const std::uint8_t* blob   = nullptr;
    std::size_t         size   = 0;
    std::size_t         offset = 0;  // of the DXBC
    std::size_t         length = 0;  // of the DXBC
};

/// The D3D12 shader object's code blob if `pixelShader` is the original level-0 reduce shader (by its DXBC
/// checksum). `checksum` receives the DXBC checksum of whatever shader it is, when readable.
bool ReadOriginal(const std::uint8_t* pixelShader, OriginalCode& code, std::uint8_t (&checksum)[16]) noexcept
{
    std::uintptr_t      vtable   = 0;
    const std::uint8_t* bytecode = nullptr;
    std::size_t         length   = 0;
    ByteArray           blob{};
    char                magic[4]{};
    std::uint32_t       containerSize = 0;
    if (pixelShader == nullptr || !platform::CopyGuarded(&vtable, pixelShader, sizeof(vtable)) ||
        vtable != g_d3d12PixelVtable ||
        !platform::CopyGuarded(&bytecode, pixelShader + facts::kD3D12PsBytecode, sizeof(bytecode)) ||
        !platform::CopyGuarded(&length, pixelShader + facts::kD3D12PsBytecodeLen, sizeof(length)) ||
        !platform::CopyGuarded(&blob, pixelShader + facts::kD3D12PsCode, sizeof(blob)) || bytecode == nullptr ||
        !platform::CopyGuarded(magic, bytecode, sizeof(magic)) || !platform::CopyGuarded(checksum, bytecode + 4, 16) ||
        !platform::CopyGuarded(&containerSize, bytecode + 24, sizeof(containerSize)))
    {
        return false;
    }
    const bool isOriginal = std::memcmp(magic, "DXBC", 4) == 0 &&
                            std::equal(std::begin(checksum), std::end(checksum), shader::kOriginalChecksumBytes.begin()) &&
                            length == shader::kOriginalSize && containerSize == shader::kOriginalSize;
    // The DXBC must lie inside the blob the shader object keeps.
    if (!isOriginal || blob.data == nullptr || blob.num <= 0 || bytecode < blob.data ||
        static_cast<std::size_t>(bytecode - blob.data) + length > static_cast<std::size_t>(blob.num))
    {
        return false;
    }
    code = {blob.data, static_cast<std::size_t>(blob.num), static_cast<std::size_t>(bytecode - blob.data), length};
    return true;
}

/// Bytes as hex for the log (empty if out of memory: this runs inside engine calls, where nothing may throw).
std::wstring Hex(const std::uint8_t* bytes, std::size_t count) noexcept
{
    std::wstring text;
    try
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            text += std::format(L"{:02X} ", bytes[i]);
        }
    }
    catch (...)
    {
        text.clear();
    }
    return text;
}

/// The original's blob with the corrected DXBC in place of its own: the resource tables before the DXBC and the
/// optional data after it (packed resource counts, footer) stay the original's, as the shader's bindings do.
bool SpliceCode(const OriginalCode& original, std::vector<std::uint8_t>& code) noexcept
{
    try
    {
        std::vector<std::uint8_t> copy(original.size);
        if (!platform::CopyGuarded(copy.data(), original.blob, original.size))
        {
            return false;
        }
        const std::size_t trailer = original.size - original.offset - original.length;
        code.assign(copy.begin(), copy.begin() + static_cast<std::ptrdiff_t>(original.offset));
        code.insert(code.end(), shader::kBloomReduceL0Fixed.begin(), shader::kBloomReduceL0Fixed.end());
        code.insert(code.end(), copy.end() - static_cast<std::ptrdiff_t>(trailer), copy.end());
        // For baking the same framing on D3D11, which keeps no copy (RE-impl-facts-bloom-swap.md §4.3).
        platform::Log(L"bloom origin: original code blob {} bytes; before the DXBC ({} bytes): {}; after it ({} bytes): {}",
                      original.size, original.offset, Hex(copy.data(), original.offset), trailer,
                      Hex(copy.data() + original.offset + original.length, trailer));
        return true;
    }
    catch (...)
    {
        return false;
    }
}

/// Creates the corrected shader through the engine's own render-thread creation, once. Render thread; the caller
/// holds g_createMutex and has seen neither a shader nor a failure.
void CreateFixed(const OriginalCode& original) noexcept
{
    std::vector<std::uint8_t> code;
    void*                     rhi = nullptr;
    void*                     vtable = nullptr;
    CreatePsRenderFn          create = nullptr;
    if (!SpliceCode(original, code) || code.size() > INT32_MAX ||
        !platform::CopyGuarded(&rhi, reinterpret_cast<const void*>(g_rhiGlobal), sizeof(rhi)) || rhi == nullptr ||
        !platform::CopyGuarded(&vtable, rhi, sizeof(vtable)) ||
        !platform::CopyGuarded(&create, static_cast<const std::uint8_t*>(vtable) + facts::kRhiSlotCreatePixelShaderRT * 8,
                               sizeof(create)))
    {
        g_createFailed.store(true);
        platform::Log(L"bloom origin: the corrected shader's code could not be put together; player 2's bloom keeps player 1's rows");
        return;
    }
    const ByteArray array{code.data(), static_cast<std::int32_t>(code.size()), static_cast<std::int32_t>(code.size())};
    void*           shaderRef = nullptr;
    void**          returned  = nullptr;  // the engine returns `out` itself; the shader is what it wrote there
    if (!platform::CallGuarded(create, returned, rhi, &shaderRef, reinterpret_cast<void*>(g_immediateList),
                               static_cast<const void*>(&array)) ||
        shaderRef == nullptr)
    {
        g_createFailed.store(true);
        platform::Log(L"bloom origin: the engine did not create the corrected shader; player 2's bloom keeps player 1's rows");
        return;
    }
    g_fixedShader.store(shaderRef);  // the creation's reference: kept, since pipeline-cache keys hold the raw pointer
    platform::Log(L"bloom origin: corrected level-0 bloom shader created ({} bytes of code, thread {})", code.size(),
                  GetCurrentThreadId());
}

/// Both level-0 pipeline calls: a view that does not start at (0,0) gets the corrected shader in a copy of the
/// pipeline description, when the description's shader is the original.
void* Pipeline(void* commandList, const std::uint8_t* initializer, std::uint32_t applyFlags)
{
    const std::uint8_t* view = t_view;
    std::int32_t        min[2]{};
    const bool          offset = view != nullptr && platform::CopyGuarded(min, view + facts::kViewRectMinX, sizeof(min)) &&
                                 (min[0] != 0 || min[1] != 0);
    // Checked for every offset view, and for any view until the shader exists (made early, off the split join).
    if (!g_enabled.load(std::memory_order_relaxed) || view == nullptr ||
        (!offset && (g_fixedShader.load() != nullptr || g_createFailed.load())))
    {
        return g_getPipeline(commandList, initializer, applyFlags);
    }
    const std::uint8_t* pixelShader = nullptr;
    OriginalCode        original;
    std::uint8_t        checksum[16]{};
    platform::CopyGuarded(&pixelShader, initializer + facts::kPsoInitPixelShader, sizeof(pixelShader));
    if (!ReadOriginal(pixelShader, original, checksum))
    {
        if (offset && !g_loggedOther.exchange(true))
        {
            platform::Log(L"bloom origin: a level-0 reduce for a view at ({}, {}) uses a shader this fix has no copy of "
                          L"(DXBC checksum {}); left as it is",
                          min[0], min[1], Hex(checksum, sizeof(checksum)));
        }
        return g_getPipeline(commandList, initializer, applyFlags);
    }
    if (g_fixedShader.load() == nullptr && !g_createFailed.load())
    {
        const std::lock_guard lock(g_createMutex);
        if (g_fixedShader.load() == nullptr && !g_createFailed.load())
        {
            CreateFixed(original);
        }
    }
    void* const fixedShader = g_fixedShader.load();
    if (!offset || fixedShader == nullptr)
    {
        return g_getPipeline(commandList, initializer, applyFlags);
    }
    alignas(16) std::uint8_t copy[facts::kPsoInitializerSize];
    std::memcpy(copy, initializer, sizeof(copy));
    std::memcpy(copy + facts::kPsoInitPixelShader, &fixedShader, sizeof(fixedShader));
    if (!g_loggedSwap.exchange(true))
    {
        // The binder's own spilled source-rect pointer (initializer - 0x10) as a cross-check of the view's origin.
        const float* sourceRect = nullptr;
        float        source[4]{};
        platform::CopyGuarded(&sourceRect, initializer - 0x10, sizeof(sourceRect));
        platform::CopyGuarded(source, sourceRect, sizeof(source));
        platform::Log(L"bloom origin: level-0 reduce for the view at ({}, {}) uses the corrected shader (source rect "
                      L"{}, {}, {} x {}; command list {} the immediate list; thread {})",
                      min[0], min[1], source[0], source[1], source[2], source[3],
                      reinterpret_cast<std::uintptr_t>(commandList) == g_immediateList ? L"is" : L"is NOT",
                      GetCurrentThreadId());
    }
    return g_getPipeline(commandList, copy, applyFlags);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto getPipeline   = platform::FindFunction(image, facts::kGetAndOrCreateGraphicsPso);
    const auto bind          = platform::FindFunction(image, facts::kBloomL0Bind);
    const auto bindExact     = platform::FindFunction(image, facts::kBloomL0BindExact);
    const auto bindCall      = image.Rebase(facts::kBloomL0BindCall, 5);
    const auto bindExactCall = image.Rebase(facts::kBloomL0BindExactCall, 5);
    const auto pipeline      = image.Rebase(facts::kBloomL0PipelineCall, 5);
    const auto pipelineExact = image.Rebase(facts::kBloomL0ExactPipelineCall, 5);
    const auto rhiGlobal     = image.Rebase(facts::kGDynamicRHI, sizeof(void*));
    const auto immediateList = image.Rebase(facts::kGRHICommandListImmediate);
    const auto d3d12Vtable   = image.Rebase(facts::kVtblD3D12DynamicRHI);
    const auto d3d11Vtable   = image.Rebase(facts::kVtblD3D11DynamicRHI);
    const auto pixelVtable   = image.Rebase(facts::kVtblD3D12PixelShader);
    const auto createSlot    = image.ReadPointer(facts::kVtblD3D12DynamicRHI + facts::kRhiSlotCreatePixelShaderRT * 8);
    const auto createRender  = platform::FindFunction(image, facts::kD3D12CreatePixelShaderRT);
    if (!getPipeline || !bind || !bindExact || !bindCall || !bindExactCall || !pipeline || !pipelineExact ||
        !rhiGlobal || !immediateList || !d3d12Vtable || !d3d11Vtable || !pixelVtable || !createSlot || !createRender ||
        *createSlot != *createRender)
    {
        platform::Log(L"bloom origin: an engine check failed; bloom stays off in split screen");
        return false;
    }
    const std::uint8_t* rhi       = nullptr;
    std::uintptr_t      rhiVtable = 0;
    if (!platform::CopyGuarded(&rhi, reinterpret_cast<const void*>(*rhiGlobal), sizeof(rhi)) || rhi == nullptr ||
        !platform::CopyGuarded(&rhiVtable, rhi, sizeof(rhiVtable)) || rhiVtable != *d3d12Vtable)
    {
        platform::Log(L"bloom origin: the renderer is {} (the fix needs D3D12's copy of the shader code); bloom stays off in split screen",
                      rhi == nullptr ? L"not up yet" : rhiVtable == *d3d11Vtable ? L"D3D11" : L"unknown");
        return false;
    }
    g_getPipeline      = reinterpret_cast<GetPipelineFn>(*getPipeline);
    g_rhiGlobal        = *rhiGlobal;
    g_immediateList    = *immediateList;
    g_d3d12PixelVtable = *pixelVtable;
    // The pipeline calls first: until a binder call is redirected no view is noted, so they pass straight through.
    // The two binder calls share one code block: the second install swaps in the block the first one left.
    const bool installed =
        g_pipelineCall.Install(*pipeline, *getPipeline, reinterpret_cast<std::uintptr_t>(&Pipeline)) &&
        g_pipelineExactCall.Install(*pipelineExact, *getPipeline, reinterpret_cast<std::uintptr_t>(&Pipeline)) &&
        g_bindExactCall.Install(*bindExactCall, *bindExact, reinterpret_cast<std::uintptr_t>(&BindFirstExact)) &&
        g_bindCall.Install(*bindCall, *bind, reinterpret_cast<std::uintptr_t>(&BindFirst));
    if (!installed)
    {
        g_bindCall.Restore();
        g_bindExactCall.Restore();
        g_pipelineExactCall.Restore();
        g_pipelineCall.Restore();
        platform::Log(L"bloom origin: a call site is not the call the research read; bloom stays off in split screen");
        return false;
    }
    g_installed.store(true);
    platform::Log(L"bloom origin: installed (D3D12); player 2's bloom will read its own rows");
    return true;
}

bool Installed() noexcept
{
    return g_installed.load();
}

void SetEnabled(bool enabled) noexcept
{
    g_enabled.store(enabled, std::memory_order_relaxed);
}

bool Enabled() noexcept
{
    return g_enabled.load(std::memory_order_relaxed);
}

}  // namespace lss::render::bloom_origin
