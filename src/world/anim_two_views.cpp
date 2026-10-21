#include "world/anim_two_views.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "world/anim_choice.h"

#include <chrono>
#include <cstdint>
#include <format>
#include <string>

namespace lss::world::anim_two_views {

namespace {

using platform::Read;
using platform::ReadPointer;
using namespace anim_choice;

using JudgeFn  = void (*)(void* mesh, bool* forceOut, void** leadOut, bool cameraCut);
using UpdateFn = std::int64_t (*)(void* allocator, float dt, void* a3, void* a4);

platform::VTableEntry g_judgeSlot;
platform::VTableEntry g_judgeSwitchableSlot;
platform::VTableEntry g_updateSlot;
JudgeFn               g_judge  = nullptr;
UpdateFn              g_update = nullptr;

bool  g_enabled          = true;
void* g_cameraTwo        = nullptr;  // set by Tick, read at the pre-actor tick of the same frame
void* g_memberTwo        = nullptr;
void* g_memberTwoMesh    = nullptr;

History  g_history;            // player 2's previous camera, for the prediction
Snapshot g_two;                // this frame's snapshot for player 2
bool     g_twoReady = false;   // g_two belongs to this frame's Update
bool     g_twoCut   = false;

// The 10 s summary.
using Clock = std::chrono::steady_clock;
Clock::time_point g_windowStart = Clock::now();
std::uint64_t     g_updates = 0, g_judged = 0, g_fromTwo = 0, g_swapFailed = 0;
double            g_updateMicros = 0.0;
int               g_memberCamera = 0;  // 1 or 2: the camera player 2's character was judged from last
std::wstring      g_summary      = L"no summary yet";

/// The mesh owner's world location (the judge's own fallback measure point).
bool OwnerLocation(void* mesh, Vec& out) noexcept
{
    void* owner = ReadPointer(mesh, facts::kComponentOwner);
    void* root  = ReadPointer(owner, facts::kActorRootComponent);
    return root != nullptr && Read(static_cast<const std::uint8_t*>(root) + facts::kComponentWorldLocation, out);
}

void Judge(void* mesh, bool* forceOut, void** leadOut, bool cameraCut) noexcept
{
    if (!g_twoReady)
    {
        g_judge(mesh, forceOut, leadOut, cameraCut);
        return;
    }
    ++g_judged;
    auto*    allocator = static_cast<std::uint8_t*>(ReadPointer(mesh, facts::kMeshBudgetAllocator));
    Snapshot one;
    Vec      point;
    if (allocator == nullptr || !Read(allocator + facts::kAllocatorSnapshot, one) || !OwnerLocation(mesh, point) ||
        !PreferTwo(Measure(point, one.location, one.forward), Measure(point, g_two.location, g_two.forward)))
    {
        if (mesh == g_memberTwoMesh)
        {
            g_memberCamera = 1;
        }
        g_judge(mesh, forceOut, leadOut, cameraCut);
        return;
    }
    // The game's judge, once, reading player 2's snapshot; the block is player 1's again before the next mesh.
    if (!platform::WriteGuarded(allocator + facts::kAllocatorSnapshot, &g_two, sizeof(g_two)))
    {
        ++g_swapFailed;
        g_judge(mesh, forceOut, leadOut, cameraCut);
        return;
    }
    ++g_fromTwo;
    if (mesh == g_memberTwoMesh)
    {
        g_memberCamera = 2;
    }
    g_judge(mesh, forceOut, leadOut, cameraCut || g_twoCut);
    platform::WriteGuarded(allocator + facts::kAllocatorSnapshot, &one, sizeof(one));
}

/// Player 2's snapshot, read from its camera manager at the moment the game reads player 1's.
void MakeSnapshotTwo(float dt) noexcept
{
    g_twoReady = false;
    const auto* manager = static_cast<const std::uint8_t*>(g_cameraTwo);
    Vec         location;
    float       rotation[3]{};
    std::uint32_t flags = 0;
    if (!g_enabled || manager == nullptr || !Read(manager + facts::kCameraManagerPovLocation, location) ||
        !Read(manager + facts::kCameraManagerPovRotation, rotation) || !Read(manager + facts::kCameraManagerCutFlags, flags))
    {
        g_history.valid = false;
        return;
    }
    g_twoCut   = (flags & facts::kCameraManagerCutBit) != 0;
    g_two      = Predict(g_history, location, ForwardOf(rotation[0], rotation[1]), g_twoCut, dt);
    g_twoReady = true;
}

std::int64_t Update(void* allocator, float dt, void* a3, void* a4) noexcept
{
    MakeSnapshotTwo(dt);
    const auto         start  = Clock::now();
    const std::int64_t result = g_update(allocator, dt, a3, a4);
    g_updateMicros += std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    ++g_updates;
    g_twoReady = false;  // the snapshot is used only inside this frame's loop
    return result;
}

std::uint8_t ClothMode(void* member) noexcept
{
    void*        kbd     = ReadPointer(member, facts::kCharacterKbd);
    void*        first   = ReadPointer(ReadPointer(kbd, facts::kKbdBonamiks), 0);
    std::int32_t mode    = -1;
    Read(static_cast<const std::uint8_t*>(first) + facts::kBonamikUpdateMode, mode);
    return static_cast<std::uint8_t>(mode);
}

void Summarise() noexcept
try
{
    const auto now = Clock::now();
    if (now - g_windowStart < std::chrono::seconds(10))
    {
        return;
    }
    g_windowStart = now;
    if (g_updates == 0)
    {
        return;
    }
    std::wstring member = L"player 2's character not judged";
    if (g_memberTwoMesh != nullptr && g_memberCamera != 0)
    {
        float distance = 0.0f, angle = 0.0f;
        Read(static_cast<const std::uint8_t*>(g_memberTwoMesh) + facts::kMeshAnimDistance, distance);
        Read(static_cast<const std::uint8_t*>(g_memberTwoMesh) + facts::kMeshAnimAngle, angle);
        member = std::format(L"player 2's character judged from player {}'s camera ({:.1f} m, {:.0f} deg, cloth mode {})",
                             g_memberCamera, distance / 100.0f, angle * 57.29578f, ClothMode(g_memberTwo));
    }
    g_summary = std::format(L"{:.0f} meshes judged per frame, {:.0f} from player 2's camera ({:.0f} %), judging {:.3f} ms per "
                            L"frame, {} snapshot write(s) failed; {}",
                            static_cast<double>(g_judged) / g_updates, static_cast<double>(g_fromTwo) / g_updates,
                            g_judged ? 100.0 * g_fromTwo / g_judged : 0.0, g_updateMicros / g_updates / 1000.0, g_swapFailed,
                            member);
    platform::LogDebug(L"anim two views: {}", g_summary);
    g_updates = g_judged = g_fromTwo = g_swapFailed = 0;
    g_updateMicros = 0.0;
}
catch (...)
{
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto judge = reinterpret_cast<std::uintptr_t>(&Judge);
    if (!platform::ReplaceSlot(image, facts::kAnimJudgeSlot, judge, g_judgeSlot))
    {
        platform::LogWarning(L"anim two views: the judge's slot differs; characters are judged from player 1's camera only");
        return false;
    }
    if (!platform::ReplaceSlot(image, facts::kAnimJudgeSwitchableSlot, judge, g_judgeSwitchableSlot) ||
        !platform::ReplaceSlot(image, facts::kAnimBudgetUpdateSlot, reinterpret_cast<std::uintptr_t>(&Update), g_updateSlot))
    {
        g_judgeSwitchableSlot.Restore();
        g_judgeSlot.Restore();
        platform::LogWarning(L"anim two views: a slot differs; characters are judged from player 1's camera only");
        return false;
    }
    g_judge  = reinterpret_cast<JudgeFn>(g_judgeSlot.Original());
    g_update = reinterpret_cast<UpdateFn>(g_updateSlot.Original());
    control::Register({.name  = L"anim-two",
                       .shape = control::Shape::OnOff,
                       .help  = L"characters near player 2 are animated as if player 2's camera were the game's (world/anim_two_views.h; on)",
                       .set   = [](const control::Value& value) {
                           SetEnabled(std::get<bool>(value));
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled}; }});
    control::Register({.name = L"world.anim", .shape = control::Shape::Text,
                       .help = L"the last 10 s: meshes judged, the share judged from player 2's camera, the time, player 2's character",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"anim two views: installed; each character is judged from the camera that sees it better while player 2 is in");
    return true;
}

void Tick(void* cameraManagerTwo, void* memberTwo) noexcept
{
    if (g_judge == nullptr)
    {
        return;
    }
    g_cameraTwo = cameraManagerTwo;
    if (memberTwo != g_memberTwo)
    {
        g_memberTwo    = memberTwo;
        g_memberCamera = 0;
    }
    g_memberTwoMesh = ReadPointer(memberTwo, facts::kCharacterMesh);
    Summarise();
}

void SetEnabled(bool on) noexcept
{
    g_enabled = on;
    platform::Log(L"anim two views {}", on ? L"on" : L"off (every character judged from player 1's camera, as the game alone does)");
}

std::wstring Reading() noexcept
{
    return std::wstring(g_enabled ? L"on; " : L"off; ") + g_summary;
}

}  // namespace lss::world::anim_two_views
