#include "render/pass_marks.h"

#include "control/registry.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "render/family_boundary.h"
#include "render/view_marks.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <variant>

namespace lss::render::pass_marks {

namespace {

enum class Kind : std::uint8_t
{
    Wrap,        // the pass is the call: its event begins before the call and ends after it
    BeginAfter,  // the pass follows the call: its event begins when the call returns
    EndBefore,   // the pass precedes the call: its event ends before the call
};

/// A pass's call in the scene renderer: its name, the call's address, the callee, and the 11 bytes around the call
/// (three before the E8, the call, three after), as read in the database (research/.ida-mcp/pass-marks/_sites.tsv,
/// generated from it; checked against the exe 2026-11-02).
struct PassSite
{
    const wchar_t*               name;
    std::uintptr_t               site;
    std::uintptr_t               target;
    Kind                         kind;
    std::array<std::uint8_t, 11> bytes;
};
constexpr std::uintptr_t kSiteLead = 3;  // the bytes before the E8
constexpr std::uintptr_t kCallSize = 5;  // E8 rel32

constexpr auto kSites = std::to_array<PassSite>({    {L"PrePass", 0x1421E6A20, 0x1421ED010, Kind::Wrap,
     {0x89, 0x45, 0x10, 0xE8, 0xEB, 0x65, 0x00, 0x00, 0x0F, 0xB6, 0x54}},  // 1
    {L"PrePass (depth clear only)", 0x1421E69FC, 0x1421ED9A0, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x9F, 0x6F, 0x00, 0x00, 0xEB, 0x22, 0x48}},  // 1b
    {L"Light grid", 0x1421E6ACF, 0x14221EA50, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x7C, 0x7F, 0x03, 0x00, 0xEB, 0x35, 0x48}},  // 2
    {L"SE glare uniforms", 0x1421E6B11, 0x1421E54A0, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x8A, 0xE9, 0xFF, 0xFF, 0x44, 0x0F, 0xB6}},  // 3
    {L"Occlusion", 0x1421E6B9A, 0x1425C00C0, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x21, 0x95, 0x3D, 0x00, 0x48, 0x8B, 0xD6}},  // 4
    {L"HZB", 0x1421E6BA5, 0x1421E5240, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x96, 0xE6, 0xFF, 0xFF, 0x84, 0xC0, 0x75}},  // 5
    {L"Shadow depths", 0x1421E6BCD, 0x1425E4660, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x8E, 0xDA, 0x3F, 0x00, 0xE8, 0x69, 0xF8}},  // 6
    {L"LPV clear", 0x1421E6BE6, 0x142226BF0, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x05, 0x00, 0x04, 0x00, 0xE8, 0x50, 0xF8}},  // 7
    {L"Custom depth", 0x1421E6C02, 0x1425C7BA0, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x99, 0x0F, 0x3E, 0x00, 0x45, 0x84, 0xED}},  // 8
    {L"Volumetric fog", 0x1421E6C12, 0x14264D410, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0xF9, 0x67, 0x46, 0x00, 0x8B, 0x8F, 0x50}},  // 9
    {L"DBuffer decals", 0x1421E6C81, 0x14219EB10, Kind::Wrap,
     {0x00, 0x00, 0x00, 0xE8, 0x8A, 0x7E, 0xFB, 0xFF, 0xFF, 0xC3, 0x3B}},  // 10
    {L"Translucent volume clear", 0x1421E6DD2, 0x142626080, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0xA9, 0xF2, 0x43, 0x00, 0x8B, 0x5C, 0x24}},  // 11
    {L"GBuffer clear", 0x1421E6DF7, 0x1421E51C0, Kind::Wrap,
     {0x48, 0x8B, 0xCE, 0xE8, 0xC4, 0xE3, 0xFF, 0xFF, 0xE8, 0x3F, 0xF6}},  // 12
    {L"Base pass", 0x1421E6E5F, 0x142198010, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0xAC, 0x11, 0xFB, 0xFF, 0x0F, 0xB6, 0x54}},  // 13
    {L"SE after-base clear", 0x1421E6E77, 0x142198D60, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0xE4, 0x1E, 0xFB, 0xFF, 0x48, 0x8B, 0xD6}},  // 14a
    {L"SE extra base pass 2", 0x1421E6E82, 0x142198DE0, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x59, 0x1F, 0xFB, 0xFF, 0x48, 0x8B, 0xD6}},  // 14b
    {L"SE fade-alpha depth", 0x1421E6E8D, 0x1421ED460, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0xCE, 0x65, 0x00, 0x00, 0x48, 0x8B, 0xD6}},  // 14c
    {L"SE extra base pass 3", 0x1421E6E98, 0x1421990B0, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x13, 0x22, 0xFB, 0xFF, 0x48, 0x8B, 0xD6}},  // 14d
    {L"SE mesh list A", 0x1421E6EA3, 0x142199390, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0xE8, 0x24, 0xFB, 0xFF, 0xE8, 0x93, 0xF5}},  // 14e
    {L"GBuffer clear at max Z", 0x1421E6F30, 0x1421E4B00, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0xCB, 0xDB, 0xFF, 0xFF, 0xE8, 0x06, 0xF5}},  // 15
    {L"Occlusion", 0x1421E6F7E, 0x1421E60F0, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x6D, 0xF1, 0xFF, 0xFF, 0x48, 0x8B, 0xD6}},  // 5L
    {L"HZB", 0x1421E6F89, 0x1421E5240, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0xB2, 0xE2, 0xFF, 0xFF, 0x84, 0xC0, 0x75}},  // 5L2
    {L"Shadow depths", 0x1421E6FC5, 0x1425E4660, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x96, 0xD6, 0x3F, 0x00, 0x48, 0x8B, 0xD6}},  // 6L
    {L"Volumetric fog", 0x1421E6FD0, 0x14264D410, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x3B, 0x64, 0x46, 0x00, 0xE8, 0x66, 0xF4}},  // 9L
    {L"Custom depth", 0x1421E6FF3, 0x1425C7BA0, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0xA8, 0x0B, 0x3E, 0x00, 0xE8, 0x43, 0xF4}},  // 8L
    {L"GPU particles", 0x1421E6FF8, 0x1421E6440, Kind::BeginAfter,
     {0x0B, 0x3E, 0x00, 0xE8, 0x43, 0xF4, 0xFF, 0xFF, 0x48, 0x8B, 0x47}},  // 16a
    {L"GPU particles", 0x1421E7161, 0x1421E6440, Kind::EndBefore,
     {0xCB, 0xFF, 0x10, 0xE8, 0xDA, 0xF2, 0xFF, 0xFF, 0x0F, 0xB6, 0x44}},  // 16b
    {L"Velocities", 0x1421E71D0, 0x142630960, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x8B, 0x97, 0x44, 0x00, 0x0F, 0xB6, 0x54}},  // 17
    {L"Lighting channels", 0x1421E71ED, 0x1421E9310, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x1E, 0x21, 0x00, 0x00, 0x48, 0x8B, 0xD6}},  // 18
    {L"Decals + SSAO", 0x1421E7290, 0x14219ED80, Kind::Wrap,
     {0x00, 0x00, 0x00, 0xE8, 0xEB, 0x7A, 0xFB, 0xFF, 0xFF, 0xC3, 0x3B}},  // 19
    {L"SE mesh list B", 0x1421E72AF, 0x1421E2170, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0xBC, 0xAE, 0xFF, 0xFF, 0x48, 0x8D, 0x0D}},  // 20
    {L"Capsule shadows", 0x1421E7314, 0x14219D050, Kind::Wrap,
     {0x4C, 0x8B, 0x00, 0xE8, 0x37, 0x5D, 0xFB, 0xFF, 0x80, 0x3D, 0xA9}},  // 21
    {L"Translucent volume clear", 0x1421E74E4, 0x142625E00, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x17, 0xE9, 0x43, 0x00, 0x0F, 0xB6, 0x54}},  // 22
    {L"Lights", 0x1421E74FC, 0x142234F50, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x4F, 0xDA, 0x04, 0x00, 0x0F, 0xB6, 0x54}},  // 23
    {L"Translucent volume inject", 0x1421E752C, 0x142626A10, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0xDF, 0xF4, 0x43, 0x00, 0xE8, 0x0A, 0xEF}},  // 24
    {L"Translucent volume filter", 0x1421E753C, 0x142628D30, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0xEF, 0x17, 0x44, 0x00, 0xE8, 0xFA, 0xEE}},  // 25
    {L"LPV indirect", 0x1421E757C, 0x14219F2C0, Kind::Wrap,
     {0x48, 0x8B, 0xD6, 0xE8, 0x3F, 0x7D, 0xFB, 0xFF, 0xE8, 0xBA, 0xEE}},  // 26
    {L"Sky lighting + DFAO", 0x1421E75A1, 0x1421EDEB0, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x0A, 0x69, 0x00, 0x00, 0xE8, 0x95, 0xEE}},  // 27
    {L"Scene colour resolve", 0x1421E75B1, 0x1425CAD10, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x5A, 0x37, 0x3E, 0x00, 0x4C, 0x8D, 0x4C}},  // 28
    {L"Reflections", 0x1421E75C5, 0x1425682F0, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x26, 0x0D, 0x38, 0x00, 0xE8, 0x71, 0xEE}},  // 29
    {L"SE mesh list A again", 0x1421E767B, 0x1425689D0, Kind::Wrap,
     {0x89, 0x78, 0x18, 0xE8, 0x50, 0x13, 0x38, 0x00, 0xE8, 0xBB, 0xED}},  // 30
    {L"Scene colour resolve", 0x1421E768B, 0x1425CAD10, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x80, 0x36, 0x3E, 0x00, 0x45, 0x33, 0xFF}},  // 31
    {L"Subsurface scattering", 0x1421E76BB, 0x14219F5F0, Kind::Wrap,
     {0x00, 0x00, 0x00, 0xE8, 0x30, 0x7F, 0xFB, 0xFF, 0xFF, 0xC3, 0x3B}},  // 32
    {L"Light shaft occlusion", 0x1421E76FC, 0x142239900, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0xFF, 0x21, 0x05, 0x00, 0xE8, 0x3A, 0xED}},  // 33
    {L"Atmosphere", 0x1421E775D, 0x142195570, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x0E, 0xDE, 0xFA, 0xFF, 0xE8, 0xD9, 0xEC}},  // 34
    {L"SE water", 0x1421E77A9, 0x142653D60, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0xB2, 0xC5, 0x46, 0x00, 0x48, 0x81, 0xC3}},  // 35
    {L"Scene colour resolve (history)", 0x1421E78F6, 0x1425CAD10, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x15, 0x34, 0x3E, 0x00, 0x66, 0x0F, 0x6F}},  // 36
    {L"Fog", 0x1421E7B23, 0x142206470, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x48, 0xE9, 0x01, 0x00, 0xE8, 0x13, 0xE9}},  // 37
    {L"Translucency setup", 0x1421E7CD9, 0x14262C5D0, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0xF2, 0x48, 0x44, 0x00, 0x49, 0x8B, 0xCE}},  // 38a
    {L"Translucency", 0x1421E7CF6, 0x14262C9E0, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0xE5, 0x4C, 0x44, 0x00, 0x41, 0xB8, 0x01}},  // 38b
    {L"Translucency", 0x1421E7D12, 0x14262C9E0, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0xC9, 0x4C, 0x44, 0x00, 0xFF, 0xC3, 0x83}},  // 38c
    {L"Distortion", 0x1421E7DBF, 0x142202E90, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0xCC, 0xB0, 0x01, 0x00, 0xE8, 0x77, 0xE6}},  // 39
    {L"Velocities after translucency", 0x1421E7E0F, 0x142630CA0, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x8C, 0x8E, 0x44, 0x00, 0x8B, 0x87, 0xD8}},  // 40
    {L"SE glare", 0x1421E7F1C, 0x14262D250, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x2F, 0x53, 0x44, 0x00, 0x8B, 0x87, 0xD8}},  // 41
    {L"Scene colour resolve (final)", 0x1421E804E, 0x1425CAD10, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0xBD, 0x2C, 0x3E, 0x00, 0xE8, 0xC8, 0x7C}},  // 42
    {L"Scene capture copy", 0x1421E8075, 0x1425B5740, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0xC6, 0xD6, 0x3C, 0x00, 0x48, 0x8D, 0x4C}},  // 43
    {L"SE in-game UI", 0x1421E8330, 0x14262D140, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x0B, 0x4E, 0x44, 0x00, 0x80, 0x3D, 0x29}},  // 44
    {L"Post-processing", 0x1421E8400, 0x14251C230, Kind::Wrap,
     {0x00, 0x00, 0x00, 0xE8, 0x2B, 0x3E, 0x33, 0x00, 0xFF, 0xC3, 0x3B}},  // 45
    {L"Render finish", 0x1421E845F, 0x1421E5190, Kind::Wrap,
     {0x48, 0x8B, 0xCF, 0xE8, 0x2C, 0xCD, 0xFF, 0xFF, 0x0F, 0xB6, 0x54}},  // 46
});
constexpr std::size_t kCount = kSites.size();

/// Every site's callee: four integer or pointer arguments at most, the result in rax (none takes floats or stack
/// arguments, none returns a struct: _sites.tsv), so the wrapper forwards the four registers and rax.
using PassFn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

// Set once by Install, then only read.
std::array<std::uintptr_t, kCount> g_site{};  // live addresses
std::atomic<bool>                  g_installed{false};

std::atomic<bool> g_wanted{false};  // the switch (game thread writes)

// The render thread (the thread that runs the scene renderer).
std::array<platform::CallRedirect, kCount> g_redirects;
/// Where each call went when its redirect was placed: the game's pass, or another module's redirect of the same call
/// (the fog volume watch's two volumetric fog calls, the UI targets' post-processing call, both placed at start and
/// kept), which the wrapper then calls in turn, so that module keeps working while marked.
std::array<std::uintptr_t, kCount> g_target{};
bool g_placed      = false;  // the redirects are in place
bool g_bracketOpen = false;  // a BeginAfter event (the particles) waits for its EndBefore

std::atomic<std::uint32_t> g_wrapped{0};   // sites redirected the last time they were placed
std::atomic<std::uint32_t> g_refused{0};   // sites that could not be
std::atomic<std::uint64_t> g_marked{0};    // passes marked (nodes put, begin and end)
std::atomic<std::uint64_t> g_unmarked{0};  // nodes the family boundary could not take

/// RHI thread: submits, then begins site I's pass event on the queue.
template <std::size_t I>
void BeginNode(void* commandList, void* /*node*/) noexcept
{
    if (void* const context = family_boundary::SubmitForMark(commandList))
    {
        view_marks::BeginPass(context, kSites[I].name);
    }
}

/// RHI thread: submits (the pass's commands go to the queue first), then ends the innermost pass event.
void EndNode(void* commandList, void* /*node*/) noexcept
{
    if (family_boundary::SubmitForMark(commandList) != nullptr)
    {
        view_marks::EndPass();
    }
}

/// Render thread: puts a node on the immediate list.
void Put(boundary::NodeFn node) noexcept
{
    (family_boundary::Append(node) ? g_marked : g_unmarked).fetch_add(1, std::memory_order_relaxed);
}

void CloseBracket() noexcept
{
    if (g_bracketOpen)
    {
        g_bracketOpen = false;
        Put(&EndNode);  // the particles' closing call did not run: their event ends at the next pass
    }
}

/// The redirected call: marks around the game's own call, which is made with the caller's four registers.
template <std::size_t I>
std::uint64_t Wrapper(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d)
{
    constexpr Kind kind = kSites[I].kind;
    if constexpr (kind != Kind::BeginAfter)
    {
        CloseBracket();
    }
    if constexpr (kind == Kind::Wrap)
    {
        Put(&BeginNode<I>);
    }
    const std::uint64_t result = reinterpret_cast<PassFn>(g_target[I])(a, b, c, d);
    if constexpr (kind == Kind::Wrap)
    {
        Put(&EndNode);
    }
    else if constexpr (kind == Kind::BeginAfter)
    {
        Put(&BeginNode<I>);
        g_bracketOpen = true;
    }
    return result;
}

template <std::size_t... I>
std::array<std::uintptr_t, kCount> WrapperAddresses(std::index_sequence<I...>) noexcept
{
    return {reinterpret_cast<std::uintptr_t>(&Wrapper<I>)...};
}

control::Outcome Set(const control::Value& value)
try
{
    const bool on = std::get<bool>(value);
    if (g_wanted.exchange(on, std::memory_order_relaxed) != on)
    {
        platform::Log(L"pass marks: {} (the render thread {} the redirects before the next split frame)",
                      on ? L"on" : L"off", on ? L"places" : L"removes");
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
    return control::Value{std::format(L"{}; {} of {} pass calls wrapped, {} refused; {} marks put, {} not taken",
                                      g_wanted.load() ? L"on" : L"off", g_wrapped.load(), kCount, g_refused.load(),
                                      g_marked.load(), g_unmarked.load())};
}
catch (...)
{
    return control::Value{std::wstring{}};
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    for (std::size_t i = 0; i < kCount; ++i)
    {
        // The bytes around the call, not its displacement: another module may already redirect it (it is then
        // wrapped in turn, BetweenFamilies).
        const PassSite& pass = kSites[i];
        const auto      site = image.Rebase(pass.site, kCallSize);
        const std::span<const std::uint8_t> bytes{pass.bytes};
        if (!site || !image.BytesMatch(pass.site - kSiteLead, bytes.first(kSiteLead + 1)) ||
            !image.BytesMatch(pass.site + kCallSize, bytes.last(pass.bytes.size() - kSiteLead - kCallSize)))
        {
            platform::LogWarning(L"pass marks: UNAVAILABLE (the call for {} at {:#x} differs); no pass events", pass.name,
                                 pass.site);
            return false;
        }
        g_site[i] = *site;
    }
    g_installed.store(true, std::memory_order_release);  // publishes the addresses
    control::Register({.name  = L"pass-marks",
                       .shape = control::Shape::OnOff,
                       .help  = L"a named GPU event around each render pass of a view (Base pass, Lights, Shadow depths, "
                                L"Post-processing, ...), nested in the view's, for a GPU trace (off at start; on adds two "
                                L"submits per pass; placed on the render thread before the next split frame) "
                                L"(render/pass_marks.h)",
                       .set   = &Set,
                       .get   = &Get});
    platform::Log(L"pass marks: installed ({} pass calls checked; switch pass-marks on | off; off)", kCount);
    return true;
}

void BetweenFamilies(bool firstFamily) noexcept
{
    g_bracketOpen = false;  // the view's mark closes every pass event still open
    if (!firstFamily || !g_installed.load(std::memory_order_acquire))
    {
        return;
    }
    const bool want = g_wanted.load(std::memory_order_relaxed);
    if (want == g_placed)
    {
        return;
    }
    if (want)
    {
        static const std::array<std::uintptr_t, kCount> wrappers = WrapperAddresses(std::make_index_sequence<kCount>{});
        std::uint32_t wrapped = 0;
        std::uint32_t chained = 0;
        for (std::size_t i = 0; i < kCount; ++i)
        {
            std::int32_t displacement = 0;
            if (!platform::Read(reinterpret_cast<const void*>(g_site[i] + 1), displacement))
            {
                continue;
            }
            g_target[i] = g_site[i] + kCallSize + static_cast<std::intptr_t>(displacement);
            if (g_redirects[i].InstallFromOwnThread(g_site[i], g_target[i], wrappers[i]))
            {
                ++wrapped;
                const std::uintptr_t game = g_site[i] - kSites[i].site;  // the image's slide
                chained += g_target[i] != kSites[i].target + game ? 1 : 0;
            }
        }
        g_wrapped.store(wrapped, std::memory_order_relaxed);
        g_refused.store(static_cast<std::uint32_t>(kCount) - wrapped, std::memory_order_relaxed);
        platform::Log(L"pass marks: {} of {} pass calls wrapped (on the render thread; {} through another module's "
                      L"redirect of the same call)", wrapped, kCount, chained);
    }
    else
    {
        std::uint32_t kept = 0;
        for (platform::CallRedirect& redirect : g_redirects)
        {
            if (redirect.IsInstalled() && !redirect.Restore())
            {
                ++kept;  // changed after us: left alone
            }
        }
        g_wrapped.store(0, std::memory_order_relaxed);
        platform::Log(L"pass marks: the pass calls are the game's again{}",
                      kept == 0 ? std::wstring{} : std::format(L" ({} changed by someone else, left)", kept));
    }
    g_placed = want;
}

}  // namespace lss::render::pass_marks