#include "render/bloom_origin.h"

#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "render/bloom_reduce_shader.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::render::bloom_origin {

namespace {

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

/// The corrected shader's code blob in the engine's framing: the original's resource tables, the corrected DXBC,
/// the original's optional data and footer (the bindings are the original's, so is everything around the DXBC).
constexpr auto kFixedCode = [] {
    const auto& dxbc = bloom_reduce_shader::kBloomReduceL0Fixed;
    std::array<std::uint8_t, facts::kBloomL0CodePrefix.size() + bloom_reduce_shader::kBloomReduceL0Fixed.size() +
                                 facts::kBloomL0CodeTrailer.size()>
        code{};
    auto out = std::copy(facts::kBloomL0CodePrefix.begin(), facts::kBloomL0CodePrefix.end(), code.begin());
    out      = std::copy(dxbc.begin(), dxbc.end(), out);
    std::copy(facts::kBloomL0CodeTrailer.begin(), facts::kBloomL0CodeTrailer.end(), out);
    return code;
}();

// Set once at install, on the game thread, before any redirect is live; read on the render thread afterwards.
// The redirects' atomic 16-byte code swaps order the writes before any read (x64).
platform::CallRedirect g_bindCall;
platform::CallRedirect g_bindExactCall;
platform::CallRedirect g_pipelineCall;
platform::CallRedirect g_pipelineExactCall;
GetPipelineFn          g_getPipeline   = nullptr;
CreatePsRenderFn       g_createShader  = nullptr;  // the running RHI's slot 144
void*                  g_rhi           = nullptr;  // GDynamicRHI
std::uintptr_t         g_immediateList = 0;        // &GRHICommandList_Immediate
std::uintptr_t         g_pixelVtable   = 0;        // the running RHI's pixel-shader class
const wchar_t*         g_rhiName       = L"";

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
    t_view             = view;
    void* const result = reinterpret_cast<BindFn>(call.Original())(context, vertexShaderOut, sourceRect, destRect);
    t_view             = nullptr;
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

/// True if `pixelShader` is the running RHI's pixel shader carrying the original level-0 reduce shader's hash.
/// `hash` receives whatever hash it carries, when readable.
bool IsOriginal(const std::uint8_t* pixelShader, std::array<std::uint8_t, 20>& hash) noexcept
{
    std::uintptr_t vtable = 0;
    return pixelShader != nullptr && platform::CopyGuarded(&vtable, pixelShader, sizeof(vtable)) &&
           vtable == g_pixelVtable &&
           platform::CopyGuarded(hash.data(), pixelShader + facts::kRhiShaderHash, hash.size()) &&
           hash == facts::kBloomL0ShaderHash;
}

/// Bytes as hex for the log (empty if out of memory: this runs inside engine calls, where nothing may throw).
std::wstring Hex(const std::uint8_t* bytes, std::size_t count) noexcept
{
    std::wstring text;
    try
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            text += std::format(L"{:02X}", bytes[i]);
        }
    }
    catch (...)
    {
        text.clear();
    }
    return text;
}

/// Creates the corrected shader through the engine's own render-thread creation, once. Render thread; the caller
/// holds g_createMutex and has seen neither a shader nor a failure.
void CreateFixed() noexcept
{
    const ByteArray code{kFixedCode.data(), static_cast<std::int32_t>(kFixedCode.size()),
                         static_cast<std::int32_t>(kFixedCode.size())};
    void*           shaderRef = nullptr;
    void**          returned  = nullptr;  // the engine returns `out` itself; the shader is what it wrote there
    if (!platform::CallGuarded(g_createShader, returned, g_rhi, &shaderRef, reinterpret_cast<void*>(g_immediateList),
                               static_cast<const void*>(&code)) ||
        shaderRef == nullptr)
    {
        g_createFailed.store(true);
        platform::Log(L"bloom origin: the engine did not create the corrected shader; player 2's bloom keeps player 1's rows");
        return;
    }
    g_fixedShader.store(shaderRef);  // the creation's reference: kept, since pipeline-cache keys hold the raw pointer
    platform::Log(L"bloom origin: corrected level-0 bloom shader created ({}, {} bytes of code, thread {})", g_rhiName,
                  kFixedCode.size(), GetCurrentThreadId());
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
    const std::uint8_t*          pixelShader = nullptr;
    std::array<std::uint8_t, 20> hash{};
    platform::CopyGuarded(&pixelShader, initializer + facts::kPsoInitPixelShader, sizeof(pixelShader));
    if (!IsOriginal(pixelShader, hash))
    {
        if (offset && !g_loggedOther.exchange(true))
        {
            platform::Log(L"bloom origin: a level-0 reduce for a view at ({}, {}) uses a shader this fix has no copy of "
                          L"(hash {}); left as it is",
                          min[0], min[1], Hex(hash.data(), hash.size()));
        }
        return g_getPipeline(commandList, initializer, applyFlags);
    }
    if (g_fixedShader.load() == nullptr && !g_createFailed.load())
    {
        const std::lock_guard lock(g_createMutex);
        if (g_fixedShader.load() == nullptr && !g_createFailed.load())
        {
            CreateFixed();
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

/// The running RHI: its slot-144 creation (verified) and its pixel-shader class. False (logged) for any other RHI.
bool ResolveRhi(const platform::GameImage& image, std::uintptr_t rhiGlobal) noexcept
{
    struct Known
    {
        const wchar_t*                  name;
        std::uintptr_t                  rhiVtable;
        std::uintptr_t                  pixelVtable;
        const platform::FunctionFact&   create;
    };
    const Known known[]{
        {L"D3D12", facts::kVtblD3D12DynamicRHI, facts::kVtblD3D12PixelShader, facts::kD3D12CreatePixelShaderRT},
        {L"D3D11", facts::kVtblD3D11DynamicRHI, facts::kVtblD3D11PixelShader, facts::kD3D11CreatePixelShaderRT},
    };
    void*          rhi       = nullptr;
    std::uintptr_t rhiVtable = 0;
    if (!platform::CopyGuarded(&rhi, reinterpret_cast<const void*>(rhiGlobal), sizeof(rhi)) || rhi == nullptr ||
        !platform::CopyGuarded(&rhiVtable, rhi, sizeof(rhiVtable)))
    {
        platform::Log(L"bloom origin: the renderer is not up yet; bloom stays off in split screen");
        return false;
    }
    for (const Known& rhiKind : known)
    {
        const auto vtable      = image.Rebase(rhiKind.rhiVtable);
        const auto pixelVtable = image.Rebase(rhiKind.pixelVtable);
        if (!vtable || *vtable != rhiVtable || !pixelVtable)
        {
            continue;
        }
        const auto slot   = image.ReadPointer(rhiKind.rhiVtable + facts::kRhiSlotCreatePixelShaderRT * 8);
        const auto create = platform::FindFunction(image, rhiKind.create);
        if (!slot || !create || *slot != *create)
        {
            platform::Log(L"bloom origin: {}'s shader creation is not the one the research read; bloom stays off in split screen",
                          rhiKind.name);
            return false;
        }
        g_rhi          = rhi;
        g_createShader = reinterpret_cast<CreatePsRenderFn>(*create);
        g_pixelVtable  = *pixelVtable;
        g_rhiName      = rhiKind.name;
        return true;
    }
    platform::Log(L"bloom origin: the renderer is neither D3D12 nor D3D11; bloom stays off in split screen");
    return false;
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
    if (!getPipeline || !bind || !bindExact || !bindCall || !bindExactCall || !pipeline || !pipelineExact ||
        !rhiGlobal || !immediateList)
    {
        platform::Log(L"bloom origin: an engine check failed; bloom stays off in split screen");
        return false;
    }
    if (!ResolveRhi(image, *rhiGlobal))
    {
        return false;
    }
    g_getPipeline   = reinterpret_cast<GetPipelineFn>(*getPipeline);
    g_immediateList = *immediateList;
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
    platform::Log(L"bloom origin: installed ({}); player 2's bloom will read its own rows", g_rhiName);
    return true;
}

bool Installed() noexcept
{
    return g_installed.load();
}

void SetEnabled(bool enabled) noexcept
{
    g_enabled.store(enabled, std::memory_order_relaxed);
    g_loggedSwap.store(false);
    g_loggedOther.store(false);
}

bool Enabled() noexcept
{
    return g_enabled.load(std::memory_order_relaxed);
}

}  // namespace lss::render::bloom_origin
