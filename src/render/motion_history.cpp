#include "render/motion_history.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "render/view_family.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <variant>

namespace lss::render::motion_history {

namespace {

// Facts read from the database (research/RE-velocity-per-family.md).
/// FScene's vtable (written by FScene's constructor and destructor); its GetRenderScene (slot 0x2D8, which the
/// submission calls on the family's scene) is `mov rax, rcx; ret`.
constexpr std::uintptr_t kSceneVtable         = 0x144D63A58;
constexpr std::uint32_t  kGetRenderSceneSlot  = 0x2D8;
constexpr std::uintptr_t kGetRenderSceneSelf  = 0x14067ED80;
constexpr std::array<std::uint8_t, 4> kGetRenderSceneSelfBytes{0x48, 0x8B, 0xC1, 0xC3};

constexpr std::uint32_t kSceneStarted      = 0x6488;           // FScene::CurrentFrameUpdatedMotionBlurCache
constexpr std::uint32_t kScenePaused       = 0x6350 + 0x51;    // FScene::MotionBlurInfoData.bWorldIsPaused
constexpr std::uint32_t kFamilyWorldPaused = 0x84;             // FSceneViewFamily::bWorldIsPaused

constexpr std::array<std::uint8_t, 7> kEnsureGate{0x80, 0xB9, 0x88, 0x64, 0x00, 0x00, 0x00};        // cmp byte [rcx+6488h], 0
constexpr std::array<std::uint8_t, 9> kSubmitClear{0x41, 0xC6, 0x84, 0x24, 0x88, 0x64, 0x00, 0x00, 0x00};  // mov byte [r12+6488h], 0
constexpr std::array<std::uint8_t, 7> kEnsureArgument{0x0F, 0xB6, 0x97, 0x84, 0x00, 0x00, 0x00};    // movzx edx, byte [rdi+84h]
constexpr std::array<std::uint8_t, 4> kStartFrameWrite{0x41, 0x88, 0x41, 0x51};                     // mov [r9+51h], al
constexpr std::array<std::uint8_t, 4> kUpdateGate{0x80, 0x79, 0x51, 0x00};                          // cmp byte [rcx+51h], 0
constexpr std::array<std::uint8_t, 11> kUpdateCall{0x48, 0x8B, 0x56, 0x08, 0x48, 0x8D, 0x8A, 0x50, 0x63, 0x00, 0x00};  // the table at Scene+0x6350
const std::array<platform::CodeSite, 6> kSites{{
    {L"EnsureMotionBlurCacheIsUpToDate's started-flag test", 0x142590ED9, kEnsureGate},
    {L"the submission's clear of the started flag", 0x1425C9DB0, kSubmitClear},
    {L"the submission's bWorldIsPaused argument", 0x1425C9B11, kEnsureArgument},
    {L"StartFrame's write of the paused byte", 0x14259110C, kStartFrameWrite},
    {L"UpdateMotionBlurCache's paused test", 0x142591762, kUpdateGate},
    {L"the draw's UpdateMotionBlurCache call", 0x1425CA072, kUpdateCall},
}};

// Set once by Install, then only read.
std::uintptr_t    g_sceneVtable = 0;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_on{true};

// Game thread.
std::uint8_t* g_armedPaused = nullptr;  // the paused byte of the scene armed in this split frame; null: none yet
bool          g_keptLogged  = false;
bool          g_refusedLogged = false;

std::atomic<std::uint64_t> g_kept{0};     // split frames with a later family armed (counted when the restore is asked)
std::atomic<std::uint64_t> g_refused{0};  // families whose scene was not an FScene

/// The family's scene, if it is an FScene (GetRenderScene is then the scene itself); null otherwise.
std::uint8_t* SceneOf(const std::uint8_t* family) noexcept
{
    void* const scene = view_family::SceneOf(family);
    std::uintptr_t vtable = 0;
    if (scene == nullptr || !platform::Read(scene, vtable) || vtable != g_sceneVtable)
    {
        return nullptr;
    }
    return static_cast<std::uint8_t*>(scene);
}

control::Outcome Set(const control::Value& value)
try
{
    const bool on = std::get<bool>(value);
    if (g_on.exchange(on, std::memory_order_relaxed) != on)
    {
        platform::Log(L"motion history: {}", on ? L"on (every player's family reads last frame's places, as one player's)"
                                                 : L"off (the table steps once per player's family, for an A/B)");
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
    return control::Value{std::format(L"{}; {} split frames with the later families armed, {} families refused "
                                      L"(scene not an FScene)",
                                      g_on.load() ? L"on" : L"off", g_kept.load(), g_refused.load())};
}
catch (...)
{
    return control::Value{std::wstring{}};
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto vtable = image.Rebase(kSceneVtable, kGetRenderSceneSlot + sizeof(void*));
    const auto slot   = image.ReadPointer(kSceneVtable + kGetRenderSceneSlot);
    const auto self   = image.Rebase(kGetRenderSceneSelf, kGetRenderSceneSelfBytes.size());
    if (!vtable || !slot || !self || *slot != *self || !image.BytesMatch(kGetRenderSceneSelf, kGetRenderSceneSelfBytes))
    {
        platform::LogWarning(L"motion history: UNAVAILABLE (the scene's vtable differs); the table steps once per player's "
                             L"family");
        return false;
    }
    if (const platform::CodeSite* differs = platform::FirstDiffering(image, kSites))
    {
        platform::LogWarning(L"motion history: UNAVAILABLE ({} differs); the table steps once per player's family",
                             differs->what);
        return false;
    }
    g_sceneVtable = *vtable;
    g_installed.store(true, std::memory_order_release);  // publishes the vtable
    control::Register({.name  = L"motion-history",
                       .shape = control::Shape::OnOff,
                       .help  = L"every player's family reads last frame's places of moving things, as one player's frame "
                                L"(on, at start), or the table steps once per family (off, for an A/B) "
                                L"(render/motion_history.h)",
                       .set   = &Set,
                       .get   = &Get});
    platform::Log(L"motion history: installed (switch motion-history on | off; on)");
    return true;
}

Step For(const std::uint8_t* family, int k) noexcept
{
    if (!g_installed.load(std::memory_order_acquire))
    {
        return {};
    }
    if (k < 0)
    {
        // After the last family: the paused byte back to what the first family's StartFrame wrote, only if a family of
        // this frame set it.
        std::uint8_t* const paused = std::exchange(g_armedPaused, nullptr);
        std::uint8_t        value  = 0;
        if (paused == nullptr || !platform::Read(family + kFamilyWorldPaused, value))
        {
            return {};
        }
        g_kept.fetch_add(1, std::memory_order_relaxed);
        return {.render = {paused, value}};
    }
    if (k == 0 || !g_on.load(std::memory_order_relaxed))
    {
        return {};
    }
    std::uint8_t* const scene = SceneOf(family);
    if (scene == nullptr)
    {
        g_refused.fetch_add(1, std::memory_order_relaxed);
        if (!std::exchange(g_refusedLogged, true))
        {
            platform::LogWarning(L"motion history: a family's scene is not the game's FScene; that family's table steps as "
                                 L"before (logged once)");
        }
        return {};
    }
    return {.render = {scene + kScenePaused, 1}, .started = scene + kSceneStarted};
}

void Arm(const Step& step) noexcept
{
    if (step.started == nullptr)
    {
        return;
    }
    const std::uint8_t one = 1;
    if (!platform::WriteGuarded(step.started, &one, sizeof(one)))
    {
        return;  // the submission's StartFrame runs, after the queued paused byte, and rewrites it: the old stepping
    }
    g_armedPaused = step.render.at;
    if (!std::exchange(g_keptLogged, true))
    {
        platform::LogDebug(L"motion history: the first split frame whose later families read last frame's places as the "
                           L"first one does");
    }
}

}  // namespace lss::render::motion_history
