#include "render/ssr_origin.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/game_globals.h"
#include "render/dxbc_fix.h"
#include "render/family_now.h"

#include <array>
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

namespace lss::render::ssr_origin {

namespace {

using GetPipelineFn    = void* (*)(void* commandList, const void* initializer, std::uint32_t applyFlags);
using CreatePsRenderFn = void** (*)(void* rhi, void** out, void* immediateList, const void* code);

/// TArray<uint8> as the engine lays it out.
struct ByteArray
{
    const std::uint8_t* data;
    std::int32_t        num;
    std::int32_t        max;
};

using Hash = std::array<std::uint8_t, 20>;  // FRHIShader::Hash, the compiler's OutputHash

/// One SSR shader met at the pipeline calls: its corrected copy, or none (left as it is).
struct Shader
{
    const std::uint8_t* original  = nullptr;
    Hash                hash{};
    void*               corrected = nullptr;  // FRHIPixelShader*, our reference, held for the rest of the process
    const wchar_t*      outcome   = L"";
};

// The pass's variants (quality levels, PS0/PS1): a handful. Past this many, a new one is left as it is.
constexpr std::size_t kShadersMax = 16;

// Set once at install, on the game thread, before any redirect is live; read on the render thread afterwards.
// The redirects' atomic 16-byte code swaps order the writes before any read (x64).
platform::CallRedirect g_ps0Call;
platform::CallRedirect g_ps1Call;
GetPipelineFn          g_getPipeline   = nullptr;
CreatePsRenderFn       g_createShader  = nullptr;  // D3D12's slot 144
void*                  g_rhi           = nullptr;  // GDynamicRHI
std::uintptr_t         g_immediateList = 0;        // &GRHICommandList_Immediate
std::uintptr_t         g_pixelVtable   = 0;        // FD3D12PixelShader

std::atomic<bool> g_installed{false};
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_loggedSwap{false};
std::atomic<std::uint64_t> g_splitKept{0};  // pipeline calls inside a split frame, left with the game's shader

// The pipeline calls run on the render thread [ASSUMPTION, as bloom's: RE-impl-facts-bloom-swap.md; the creation
// logs its thread]. The table is locked all the same, so a second thread could only wait, never create twice.
std::mutex                         g_mutex;
std::array<Shader, kShadersMax>    g_shaders{};
std::size_t                        g_shaderCount = 0;
bool                               g_loggedFull  = false;
bool                               g_loggedOtherList = false;

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

const wchar_t* SwapText(dxbc_fix::Swap swap) noexcept
{
    switch (swap)
    {
    case dxbc_fix::Swap::Done: return L"corrected";
    case dxbc_fix::Swap::NotContainer: return L"its code is not a DXBC container";
    case dxbc_fix::Swap::BadChecksum: return L"its stored checksum is not the one this fix computes";
    case dxbc_fix::Swap::NoProgram: return L"its code has no readable shader program";
    case dxbc_fix::Swap::NotFound: return L"it has no doubled corner add";
    case dxbc_fix::Swap::MoreThanOnce: return L"it has the corner add more than once";
    }
    return L"?";
}

/// The corrected copy of one SSR shader, made through the engine's creation; null (with `outcome` saying why) if the
/// shader is not one this fix can correct. Render thread, under g_mutex.
void* Correct(const std::uint8_t* original, const wchar_t*& outcome) noexcept
{
    std::uintptr_t      vtable   = 0;
    const std::uint8_t* bytecode = nullptr;
    std::size_t         length   = 0;
    ByteArray           code{};
    if (!platform::Read(original, vtable) || vtable != g_pixelVtable)
    {
        outcome = L"not a D3D12 pixel shader";
        return nullptr;
    }
    if (!platform::Read(original + facts::kD3D12PsBytecode, bytecode) ||
        !platform::Read(original + facts::kD3D12PsBytecodeLength, length) || !platform::Read(original + facts::kD3D12PsCode, code))
    {
        outcome = L"its code copy is not readable";
        return nullptr;
    }
    // As integers: the pointers are game values, not yet known to point into one object.
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(code.data);
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(bytecode);
    if (begin == 0 || code.num <= 0 || code.num > (1 << 24) || start < begin ||
        start - begin > static_cast<std::uintptr_t>(code.num) || length < dxbc_fix::kHeaderSize ||
        length > static_cast<std::uintptr_t>(code.num) - (start - begin))
    {
        outcome = L"its code copy is not where the research read it";
        return nullptr;
    }
    std::vector<std::uint8_t> copy;
    try
    {
        copy.resize(static_cast<std::size_t>(code.num));
    }
    catch (...)
    {
        outcome = L"out of memory";
        return nullptr;
    }
    if (!platform::CopyGuarded(copy.data(), code.data, copy.size()))
    {
        outcome = L"its code copy is not readable";
        return nullptr;
    }
    const std::size_t     at   = static_cast<std::size_t>(start - begin);
    const dxbc_fix::Swap  swap = dxbc_fix::CorrectSsr(std::span<std::uint8_t>(copy.data() + at, length));
    if (swap != dxbc_fix::Swap::Done)
    {
        outcome = SwapText(swap);
        return nullptr;
    }
    const ByteArray fixed{copy.data(), code.num, code.num};
    void*           shaderRef = nullptr;
    void**          returned  = nullptr;  // the engine returns `out` itself; the shader is what it wrote there
    if (!platform::CallGuarded(g_createShader, returned, g_rhi, &shaderRef, reinterpret_cast<void*>(g_immediateList),
                               static_cast<const void*>(&fixed)) ||
        shaderRef == nullptr)
    {
        outcome = L"the engine did not create the corrected shader";
        return nullptr;
    }
    outcome = L"corrected";
    return shaderRef;  // the creation's reference: kept, since pipeline-cache keys hold the raw pointer
}

/// The corrected copy for this pixel shader, made on first sight; null to leave the pipeline as it is. A shader is
/// made only from the render thread's own list (the creation stalls the RHI thread through it).
void* CorrectedFor(const std::uint8_t* original, const void* commandList) noexcept
{
    Hash hash{};
    if (!platform::CopyGuarded(hash.data(), original + facts::kRhiShaderHash, hash.size()))
    {
        return nullptr;  // not a shader object: the pipeline as it is
    }
    const std::lock_guard lock(g_mutex);
    for (std::size_t i = 0; i < g_shaderCount; ++i)
    {
        if (g_shaders[i].original == original && g_shaders[i].hash == hash)
        {
            return g_shaders[i].corrected;
        }
    }
    if (reinterpret_cast<std::uintptr_t>(commandList) != g_immediateList)
    {
        if (!g_loggedOtherList)
        {
            g_loggedOtherList = true;
            platform::LogWarning(L"SSR origin: SSR shader {} met on a command list other than the render thread's (thread "
                                 L"{}); not corrected there",
                                 Hex(hash.data(), hash.size()), GetCurrentThreadId());
        }
        return nullptr;  // no entry: corrected when the render thread's own list meets it
    }
    if (g_shaderCount == g_shaders.size())
    {
        if (!g_loggedFull)
        {
            g_loggedFull = true;
            platform::LogWarning(L"SSR origin: more than {} SSR shaders met; shader {} and any later one left as they are",
                          kShadersMax, Hex(hash.data(), hash.size()));
        }
        return nullptr;
    }
    Shader& shader  = g_shaders[g_shaderCount++];
    shader.original = original;
    shader.hash     = hash;
    shader.corrected = Correct(original, shader.outcome);
    if (shader.corrected != nullptr)
    {
        platform::Log(L"SSR origin: SSR shader {} corrected (each view's reflections read its own pixels; thread {})",
                      Hex(hash.data(), hash.size()), GetCurrentThreadId());
    }
    else
    {
        platform::LogWarning(L"SSR origin: SSR shader {} left as it is: {}", Hex(hash.data(), hash.size()), shader.outcome);
    }
    return shader.corrected;
}

/// Both SSR pipeline calls: an SSR shader with a corrected copy is swapped for it in a copy of the description.
void* Pipeline(void* commandList, const std::uint8_t* initializer, std::uint32_t applyFlags)
{
    const std::uint8_t* pixelShader = nullptr;
    if (!g_enabled.load(std::memory_order_relaxed) ||
        !platform::Read(initializer + facts::kPsoInitPixelShader, pixelShader) || pixelShader == nullptr)
    {
        return g_getPipeline(commandList, initializer, applyFlags);
    }
    if (family_now::Current().index >= 0)
    {
        // Inside a split frame every view drawn (the player's, and any capture drawn from its submission) is first in its
        // own family with its rectangle at (0,0), where the game's shader reads the right pixels; so Current(), not For().
        g_splitKept.fetch_add(1, std::memory_order_relaxed);
        return g_getPipeline(commandList, initializer, applyFlags);
    }
    void* const corrected = CorrectedFor(pixelShader, commandList);
    if (corrected == nullptr)
    {
        return g_getPipeline(commandList, initializer, applyFlags);
    }
    alignas(16) std::uint8_t copy[facts::kPsoInitializerSize];
    std::memcpy(copy, initializer, sizeof(copy));
    std::memcpy(copy + facts::kPsoInitPixelShader, &corrected, sizeof(corrected));
    if (!g_loggedSwap.exchange(true))
    {
        platform::Log(L"SSR origin: the reflections use the corrected shader (command list {} the immediate list; thread {})",
                      reinterpret_cast<std::uintptr_t>(commandList) == g_immediateList ? L"is" : L"is NOT",
                      GetCurrentThreadId());
    }
    return g_getPipeline(commandList, copy, applyFlags);
}

std::wstring Reading()
{
    const std::lock_guard lock(g_mutex);
    std::wstring          text = g_enabled.load() ? L"on" : L"off";
    text += std::format(L"; {} pipeline call(s) in split frames (one family per player) kept the game's shader; {} SSR shader(s) met",
                        g_splitKept.load(std::memory_order_relaxed), g_shaderCount);
    for (std::size_t i = 0; i < g_shaderCount; ++i)
    {
        text += std::format(L"; {}: {}", Hex(g_shaders[i].hash.data(), 4), g_shaders[i].outcome);
    }
    return text;
}

/// D3D12's slot-144 creation (verified) and pixel-shader class. False (logged) for any other RHI.
bool ResolveRhi(const platform::GameImage& image, std::uintptr_t rhiGlobal) noexcept
{
    void*          rhi       = nullptr;
    std::uintptr_t rhiVtable = 0;
    if (!platform::Read(reinterpret_cast<const void*>(rhiGlobal), rhi) || rhi == nullptr || !platform::Read(rhi, rhiVtable))
    {
        platform::Log(L"SSR origin: the renderer is not up yet; the further views' reflections keep the next column's surfaces");
        return false;
    }
    const auto vtable      = image.Rebase(facts::kVtblD3D12DynamicRHI);
    const auto pixelVtable = image.Rebase(facts::kVtblD3D12PixelShader);
    if (!vtable || *vtable != rhiVtable || !pixelVtable)
    {
        platform::Log(L"SSR origin: the renderer is not D3D12 (D3D11 keeps no copy of a shader's code); the "
                      L"further views' reflections keep the next column's surfaces");
        return false;
    }
    const auto slot   = image.ReadPointer(facts::kVtblD3D12DynamicRHI + facts::kRhiSlotCreatePixelShaderRT * 8);
    const auto create = platform::FindFunction(image, facts::kD3D12CreatePixelShaderRT);
    if (!slot || !create || *slot != *create)
    {
        platform::Log(L"SSR origin: D3D12's shader creation is not the one the research read; the fix stays off");
        return false;
    }
    g_rhi          = rhi;
    g_createShader = reinterpret_cast<CreatePsRenderFn>(*create);
    g_pixelVtable  = *pixelVtable;
    return true;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto getPipeline   = platform::FindFunction(image, facts::kGetAndOrCreateGraphicsPso);
    const auto ps0           = image.Rebase(facts::kSsrPs0PipelineCall, 5);
    const auto ps1           = image.Rebase(facts::kSsrPs1PipelineCall, 5);
    const auto rhiGlobal     = players::game_globals::Address(players::game_globals::Global::DynamicRhi);
    const auto immediateList = players::game_globals::Address(players::game_globals::Global::RhiCommandListImmediate);
    if (!getPipeline || !ps0 || !ps1 || !rhiGlobal || !immediateList)
    {
        platform::Log(L"SSR origin: an engine check failed; the further views' reflections keep the next column's surfaces");
        return false;
    }
    if (!ResolveRhi(image, rhiGlobal))
    {
        return false;
    }
    g_getPipeline   = reinterpret_cast<GetPipelineFn>(*getPipeline);
    g_immediateList = immediateList;
    const bool installed =
        g_ps0Call.Install(*ps0, *getPipeline, reinterpret_cast<std::uintptr_t>(&Pipeline)) &&
        g_ps1Call.Install(*ps1, *getPipeline, reinterpret_cast<std::uintptr_t>(&Pipeline));
    if (!installed)
    {
        g_ps1Call.Restore();
        g_ps0Call.Restore();
        platform::Log(L"SSR origin: a call site is not the call the research read; the further views' reflections keep "
                      L"the next column's surfaces");
        return false;
    }
    g_installed.store(true);
    control::Register({.name  = L"ssr-origin",
                       .shape = control::Shape::OnOff,
                       .help  = L"screen-space reflections read each view's own pixels (render/ssr_origin.h; on)",
                       .set   = [](const control::Value& value) {
                           g_enabled.store(std::get<bool>(value), std::memory_order_relaxed);
                           g_loggedSwap.store(false);
                           platform::Log(L"SSR origin fix {}", g_enabled.load() ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled.load()}; }});
    control::Register({.name  = L"ssr-origin.shaders",
                       .shape = control::Shape::Text,
                       .help  = L"the switch and each SSR shader met: corrected, or why it was left as it is",
                       .get   = [] { return control::Value{Reading()}; }});
    platform::Log(L"SSR origin: installed (D3D12); each view's reflections will read its own pixels");
    return true;
}

void Remove() noexcept
{
    if (!g_installed.exchange(false))
    {
        return;
    }
    // The corrected shaders stay: pipelines the engine cached with them may still be referenced by its cache.
    const bool ps1 = g_ps1Call.Restore();
    const bool ps0 = g_ps0Call.Restore();
    if (!ps1 || !ps0)
    {
        platform::LogWarning(L"SSR origin: a pipeline call was changed after the fix; left as it is (PS0 {}, PS1 {})",
                             ps0 ? L"restored" : L"kept", ps1 ? L"restored" : L"kept");
    }
}

}  // namespace lss::render::ssr_origin
