#include "views/scene_camera_two.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace lss::views::scene_camera_two {

namespace {

using platform::Read;

using IsPlayingFn    = bool (*)(void* manager, std::int64_t name);
using SetSlotFn      = void (*)(void* manager, std::uint8_t slot, const void* pose);
using SetSlotBlendFn = std::int64_t (*)(std::uint8_t slot, const void* record);
using ResetSlotsFn   = std::int64_t (*)(void* manager);
using CutFn          = std::int64_t (*)();

constexpr std::uint32_t kSlots = facts::kCameraSlotCount;

/// The blend slots (an on switch and a record; the manager's own UpdateCamera runs and ends them) and, among them,
/// those seeded from the field camera's slot 24 (RE-cutscene-camera-two.md §3.1).
constexpr bool IsBlendSlot(std::uint32_t slot) noexcept
{
    return (slot >= 2 && slot <= 7) || (slot >= 9 && slot <= 13) || slot == 16 || slot == 17 || slot == 22;
}
constexpr bool SeededFromField(std::uint32_t slot) noexcept
{
    return slot == 6 || slot == 12 || slot == 16 || slot == 22;
}

// Set at install, then only read.
void* const*           g_cutsceneManager = nullptr;  // the global holding the manager's pointer
void* const*           g_root            = nullptr;  // the field root**
IsPlayingFn            g_isPlaying       = nullptr;
SetSlotFn              g_setSlot         = nullptr;
SetSlotBlendFn         g_setSlotBlend    = nullptr;
ResetSlotsFn           g_resetSlots      = nullptr;
CutFn                  g_primaryCut      = nullptr;
platform::CallRedirect g_resetCall;
bool                   g_mirrorReady = false;

/// One camera manager's slot state.
struct Slots
{
    bool          valid     = false;
    std::uint8_t  current   = 0;    // the slot shown
    std::uint32_t active    = 0;    // bit i = slot i active
    bool          blend     = false;
    bool          pending   = false;
    float         fov       = 0.0f;  // the shown slot's
    bool          constrain = false;  // the main camera's aspect constraint
    float         aspect    = 0.0f;

    bool operator==(const Slots&) const = default;
};

/// The game's scene signals.
struct Scene
{
    bool          valid    = false;  // the state manager was readable
    bool          manager  = false;  // the cutscene manager exists
    bool          playing  = false;  // its playing test (any scene)
    bool          skip     = false;  // its skip byte
    std::uint32_t lockMask = 0;      // the field control lock mask
    std::uint32_t lockCount = 0;     // and counter
    bool          skippable = false;  // on/off 12 ShowSkipPauseMenu
    bool          black     = false;  // on/off 39, 40 or 53
    bool          loading   = false;  // on/off 41

    bool operator==(const Scene&) const = default;

    /// The scene holds the players: the mirror's reason to run.
    bool Holds() const noexcept { return playing || lockMask != 0 || lockCount != 0; }
};

/// The mirror player's manager and its copy of every slot (views/mirror_player.h).
struct MirrorPlayer
{
    void*         controller = nullptr;
    void*         manager    = nullptr;
    void*         source     = nullptr;  // player 1's manager
    std::uint32_t mirrored   = 0;        // bit i = slot i set on the mirror's manager
    std::uint32_t edgesOn = 0, edgesOff = 0, poses = 0, cuts = 0, resets = 0;
};

// Game thread.
std::array<Slots, 3>                   g_slots;    // player 1's, player 2's, the first mirror's
std::array<MirrorPlayer, kMirrorSlots> g_mirrors;  // one copy record per mirror slot (views/mirror_player)
Scene                g_scene;
bool                 g_logged = false;
bool                 g_enabled = true;
std::uint32_t        g_mirrored   = 0;      // bit i = slot i of player 2's manager set by the mirror
bool                 g_cutNoted   = false;  // player 1's manager carried the cut bit at its update this frame
bool                 g_engaged    = false;
void*                g_managerTwo = nullptr;  // the manager the mirror last wrote, for the reset redirect
struct Counts
{
    std::uint32_t edgesOn = 0, edgesOff = 0, poses = 0, cuts = 0, resets = 0, releases = 0;
} g_counts;
bool g_cutBitStuck = false;  // player 2's manager still carried the bit at its next update (logged once)

const std::uint8_t* Bytes(const void* object) noexcept
{
    return static_cast<const std::uint8_t*>(object);
}

std::uint8_t* SlotAt(void* manager, std::uint32_t slot) noexcept
{
    return static_cast<std::uint8_t*>(manager) + facts::kCameraManagerSlots + slot * facts::kCameraSlotBytes;
}

const std::uint8_t* RecordAt(const void* manager, std::uint32_t slot) noexcept
{
    return Bytes(manager) + facts::kCameraSlotBlendRecords + slot * facts::kCameraSlotBlendBytes;
}

Slots ReadSlots(const void* manager) noexcept
{
    Slots now;
    if (manager == nullptr)
    {
        return now;
    }
    const std::uint8_t* m = Bytes(manager);
    std::uint8_t blend = 0, pending = 0;
    if (!Read(m + facts::kCameraManagerCurrentSlot, now.current) || now.current >= kSlots ||
        !Read(m + facts::kCameraManagerBlendRunning, blend) || !Read(m + facts::kCameraManagerPendingCut, pending))
    {
        return now;
    }
    now.blend   = blend != 0;
    now.pending = pending != 0;
    for (std::uint32_t i = 0; i < kSlots; ++i)
    {
        std::uint8_t active = 0;
        if (!Read(m + facts::kCameraManagerSlots + i * facts::kCameraSlotBytes + facts::kCameraSlotActive, active))
        {
            return now;
        }
        now.active |= (active != 0 ? 1u : 0u) << i;
    }
    if (!Read(m + facts::kCameraManagerSlots + now.current * facts::kCameraSlotBytes + facts::kCameraSlotFov, now.fov))
    {
        return now;
    }
    // The main camera's constraint and aspect (the picture's shape inside the view).
    const auto* camera    = Bytes(platform::ReadPointer(manager, facts::kCameraManagerMainCamera));
    const auto* component = Bytes(platform::ReadPointer(camera, facts::kCameraActorComponent));
    std::uint32_t flags   = 0;
    if (component != nullptr && Read(component + facts::kCameraComponentFlags, flags) &&
        Read(component + facts::kCameraComponentAspect, now.aspect))
    {
        now.constrain = (flags & 1u) != 0;
    }
    now.valid = true;
    return now;
}

bool OnOff(const std::uint8_t* stateManager, std::uint32_t id) noexcept
{
    std::uint32_t count = 0;
    return id < facts::kOnOffCount && Read(stateManager + facts::kOnOffTable + 4 * id, count) && count != 0;
}

Scene ReadScene() noexcept
{
    Scene now;
    void* manager = nullptr;
    if (Read(g_cutsceneManager, manager) && manager != nullptr)
    {
        now.manager = true;
        std::uint8_t skip = 0;
        now.skip = Read(Bytes(manager) + facts::kCutsceneSkip, skip) && skip != 0;
        bool playing = false;
        if (g_isPlaying != nullptr && platform::CallGuarded(g_isPlaying, playing, manager, std::int64_t{0}))
        {
            now.playing = playing;
        }
    }
    void* root = nullptr;
    const std::uint8_t* stateManager = nullptr;
    if (Read(g_root, root) && root != nullptr && Read(Bytes(root) + facts::kRootStateManager, stateManager) && stateManager != nullptr &&
        Read(stateManager + facts::kFieldControlLockMask, now.lockMask) && Read(stateManager + facts::kFieldControlLockCounter, now.lockCount))
    {
        now.valid     = true;
        now.skippable = OnOff(stateManager, 12);
        now.black     = OnOff(stateManager, 39) || OnOff(stateManager, 40) || OnOff(stateManager, 53);
        now.loading   = OnOff(stateManager, 41);
    }
    return now;
}

std::wstring ActiveText(std::uint32_t active)
{
    std::wstring text;
    for (std::uint32_t i = 0; i < kSlots; ++i)
    {
        if ((active >> i) & 1u)
        {
            text += text.empty() ? std::format(L"{}", i) : std::format(L" {}", i);
        }
    }
    return text.empty() ? std::wstring(L"none") : text;
}

std::wstring SlotsText(const Slots& slots)
{
    if (!slots.valid)
    {
        return L"unreadable";
    }
    return std::format(L"slot {} (active {}; blend {}; cut pending {}; fov {:.1f}; main camera {}{:.3f})", slots.current,
                       ActiveText(slots.active), slots.blend ? L"on" : L"off", slots.pending ? L"yes" : L"no", slots.fov,
                       slots.constrain ? L"constrained to " : L"free, aspect ", slots.aspect);
}

std::wstring SceneText(const Scene& scene)
{
    return std::format(L"cutscene {}{}; control lock {:#x} (count {}); skippable scene {}; held black {}; loading screen {}{}",
                       !scene.manager ? L"no manager" : scene.playing ? L"playing" : L"not playing", scene.skip ? L" (skipping)" : L"",
                       scene.lockMask, scene.lockCount, scene.skippable ? L"on" : L"off", scene.black ? L"on" : L"off",
                       scene.loading ? L"on" : L"off", scene.valid ? L"" : L" (state manager unreadable)");
}

std::wstring MirrorText()
{
    return std::format(L"{}{}; slots mirrored now: {}; switched on {}, off {}, poses copied {}, cuts raised {}, resets {}, releases {}",
                       g_enabled ? L"on" : L"off", !g_mirrorReady ? L" (stood aside: an engine check failed)" : g_engaged ? L", engaged" : L", idle",
                       ActiveText(g_mirrored), g_counts.edgesOn, g_counts.edgesOff, g_counts.poses, g_counts.cuts, g_counts.resets,
                       g_counts.releases);
}

/// One slot of player 2's manager set from a 48-byte pose through the game's setter.
bool SetSlot(void* manager, std::uint32_t slot, const std::uint8_t (&pose)[facts::kCameraSlotBytes]) noexcept
{
    return platform::CallGuarded(g_setSlot, manager, static_cast<std::uint8_t>(slot), static_cast<const void*>(pose));
}

/// Player 2's copied pose slots switched off (a scene's release, the mod's switch, a leave).
void Release(void* managerTwo) noexcept
{
    if (managerTwo != nullptr && g_mirrored != 0)
    {
        for (std::uint32_t i = 0; i < kSlots; ++i)
        {
            if (((g_mirrored >> i) & 1u) == 0 || IsBlendSlot(i))
            {
                continue;  // a blend still running on player 2's manager ends by itself
            }
            std::uint8_t pose[facts::kCameraSlotBytes]{};
            if (platform::CopyGuarded(pose, SlotAt(managerTwo, i), sizeof(pose)))
            {
                pose[facts::kCameraSlotActive] = 0;
                SetSlot(managerTwo, i, pose);
            }
        }
        ++g_counts.releases;
    }
    g_mirrored = 0;
    g_engaged  = false;
}

/// The scene slots of player 1's manager onto player 2's (the caller holds player 2's primary scope).
void Mirror(void* managerOne, void* managerTwo) noexcept
{
    const Slots one = ReadSlots(managerOne);
    if (!one.valid)
    {
        return;
    }
    const bool holds = g_scene.Holds();
    if (!g_engaged && holds)
    {
        g_engaged = true;
        platform::Log(L"scene camera: the scene holds the players; player 2's view takes the scene camera (player 1's manager {})",
                      SlotsText(one));
    }
    if (g_engaged && !holds)
    {
        platform::Log(L"scene camera: the scene released the players; player 2's view back to its own camera (mirrored slots {}: the pose slots switched off, a blend finishes by itself)",
                      ActiveText(g_mirrored));
        Release(managerTwo);
        return;
    }
    if (!g_engaged)
    {
        return;
    }
    g_managerTwo = managerTwo;
    for (std::uint32_t i = 0; i < kSlots; ++i)
    {
        if (i == 24)
        {
            continue;  // player 2's own field camera
        }
        const bool activeOne = ((one.active >> i) & 1u) != 0;
        const bool mirrored  = ((g_mirrored >> i) & 1u) != 0;
        std::uint8_t pose[facts::kCameraSlotBytes]{};
        if (IsBlendSlot(i))
        {
            if (activeOne && !mirrored)
            {
                // The on switch: the record, then the seed (player 2's own field camera for a blend from the field).
                std::uint8_t record[facts::kCameraSlotBlendBytes]{};
                const std::uint8_t* seed = SeededFromField(i) ? SlotAt(managerTwo, 24) : SlotAt(managerOne, i);
                if (platform::CopyGuarded(record, RecordAt(managerOne, i), sizeof(record)) && platform::CopyGuarded(pose, seed, sizeof(pose)))
                {
                    std::int64_t unused = 0;
                    platform::CallGuarded(g_setSlotBlend, unused, static_cast<std::uint8_t>(i), static_cast<const void*>(record));
                    pose[facts::kCameraSlotActive] = 1;
                    if (SetSlot(managerTwo, i, pose))
                    {
                        g_mirrored |= 1u << i;
                        ++g_counts.edgesOn;
                        platform::LogDebug(L"scene camera: blend slot {} switched on for player 2's manager (seed {}; record kind {}, {} ms)", i,
                                           SeededFromField(i) ? L"player 2's own field camera" : L"player 1's slot", record[0],
                                           *reinterpret_cast<const std::int32_t*>(record + 4));
                    }
                }
            }
            else if (!activeOne && mirrored)
            {
                g_mirrored &= ~(1u << i);  // player 2's manager ends its own blend
                ++g_counts.edgesOff;
                platform::LogDebug(L"scene camera: blend slot {} off for player 1; player 2's ends by itself", i);
            }
            continue;
        }
        // A pose slot: copied every frame while the scene holds, the active byte with it.
        if (!activeOne && !mirrored)
        {
            continue;
        }
        if (!platform::CopyGuarded(pose, SlotAt(managerOne, i), sizeof(pose)) || !SetSlot(managerTwo, i, pose))
        {
            continue;
        }
        ++g_counts.poses;
        if (activeOne && !mirrored)
        {
            g_mirrored |= 1u << i;
            ++g_counts.edgesOn;
            platform::LogDebug(L"scene camera: pose slot {} copied to player 2's manager (fov {:.1f})", i,
                               *reinterpret_cast<const float*>(pose + facts::kCameraSlotFov));
        }
        else if (!activeOne && mirrored)
        {
            g_mirrored &= ~(1u << i);
            ++g_counts.edgesOff;
            platform::LogDebug(L"scene camera: pose slot {} off on both managers", i);
        }
    }
    // The cut: player 1's manager carried the bit at its update; the game's primary-scoped cut raises player 2's.
    if (g_cutNoted && g_mirrored != 0 && g_primaryCut != nullptr)
    {
        std::int64_t unused = 0;
        if (platform::CallGuarded(g_primaryCut, unused))
        {
            ++g_counts.cuts;
            platform::LogDebug(L"scene camera: camera cut raised on player 2's manager");
        }
    }
    g_cutNoted = false;
}

/// Every slot of player 1's manager onto the mirror player's (the caller holds the mirror's primary scope): a blend
/// slot set once as it turns on for player 1 (its record, and player 1's slot as the seed: the same seed player 1's
/// manager was given), a pose slot copied every frame while on for either, the field camera's slot 24 with them.
/// Every slot of player 1's manager onto mirror `slot`'s, as above.
void MirrorAll(int slot) noexcept
{
    MirrorPlayer& mirror = g_mirrors[static_cast<std::size_t>(slot)];
    const Slots   one    = ReadSlots(mirror.source);
    if (!one.valid || mirror.manager == nullptr)
    {
        return;
    }
    void* target = mirror.manager;
    for (std::uint32_t i = 0; i < kSlots; ++i)
    {
        const bool   activeOne = ((one.active >> i) & 1u) != 0;
        const bool   mirrored  = ((mirror.mirrored >> i) & 1u) != 0;
        std::uint8_t pose[facts::kCameraSlotBytes]{};
        if (IsBlendSlot(i))
        {
            if (activeOne && !mirrored)
            {
                std::uint8_t record[facts::kCameraSlotBlendBytes]{};
                if (platform::CopyGuarded(record, RecordAt(mirror.source, i), sizeof(record)) &&
                    platform::CopyGuarded(pose, SlotAt(mirror.source, i), sizeof(pose)))
                {
                    std::int64_t unused = 0;
                    platform::CallGuarded(g_setSlotBlend, unused, static_cast<std::uint8_t>(i), static_cast<const void*>(record));
                    pose[facts::kCameraSlotActive] = 1;
                    if (SetSlot(target, i, pose))
                    {
                        mirror.mirrored |= 1u << i;
                        ++mirror.edgesOn;
                        platform::LogDebug(L"mirror camera {}: blend slot {} switched on for the mirror's manager (record kind {}, {} ms)",
                                           slot + 1, i, record[0], *reinterpret_cast<const std::int32_t*>(record + 4));
                    }
                }
            }
            else if (!activeOne && mirrored)
            {
                mirror.mirrored &= ~(1u << i);  // the mirror's manager ends its own blend
                ++mirror.edgesOff;
                platform::LogDebug(L"mirror camera {}: blend slot {} off for player 1; the mirror's ends by itself", slot + 1, i);
            }
            continue;
        }
        if (!activeOne && !mirrored)
        {
            continue;
        }
        if (!platform::CopyGuarded(pose, SlotAt(mirror.source, i), sizeof(pose)) || !SetSlot(target, i, pose))
        {
            continue;
        }
        ++mirror.poses;
        if (activeOne && !mirrored)
        {
            mirror.mirrored |= 1u << i;
            ++mirror.edgesOn;
            platform::LogDebug(L"mirror camera {}: pose slot {} copied to the mirror's manager (fov {:.1f})", slot + 1, i,
                               *reinterpret_cast<const float*>(pose + facts::kCameraSlotFov));
        }
        else if (!activeOne && mirrored)
        {
            mirror.mirrored &= ~(1u << i);
            ++mirror.edgesOff;
            platform::LogDebug(L"mirror camera {}: pose slot {} off on both managers", slot + 1, i);
        }
    }
    // The cut: player 1's manager carried the bit at its update; the game's primary-scoped cut raises the mirror's.
    if (g_cutNoted && g_primaryCut != nullptr)
    {
        std::int64_t unused = 0;
        if (platform::CallGuarded(g_primaryCut, unused))
        {
            ++mirror.cuts;
            platform::LogDebug(L"mirror camera {}: camera cut raised on the mirror's manager", slot + 1);
        }
    }
}

/// The cutscene end's reset, redirected: the game's call for the primary manager, then the same on player 2's and
/// on the mirror player's.
std::int64_t ResetSlots(void* manager)
{
    const auto original = reinterpret_cast<ResetSlotsFn>(g_resetCall.Original());
    const std::int64_t result = original(manager);
    if (g_enabled && g_managerTwo != nullptr && g_managerTwo != manager && g_mirrored != 0)
    {
        original(g_managerTwo);
        g_mirrored = 0;
        ++g_counts.resets;
        platform::Log(L"scene camera: the cutscene's end reset ran on player 2's manager too (every slot off, the field camera on)");
    }
    for (int slot = 0; slot < kMirrorSlots; ++slot)
    {
        MirrorPlayer& mirror = g_mirrors[static_cast<std::size_t>(slot)];
        if (mirror.manager != nullptr && mirror.manager != manager)
        {
            original(mirror.manager);
            mirror.mirrored = 0;  // set again from player 1's slots at the next update
            ++mirror.resets;
            platform::Log(L"mirror camera {}: the cutscene's end reset ran on the mirror's manager too", slot + 1);
        }
    }
    return result;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto manager   = image.Rebase(facts::kCutsceneManagerGlobal, sizeof(void*));
    const auto root      = image.Rebase(facts::kEndGameRootGlobal, sizeof(void*));
    const auto isPlaying = platform::FindFunction(image, facts::kCutsceneIsPlaying);
    if (!manager || !root || !isPlaying)
    {
        platform::Log(L"scene camera: an engine check failed; the scene readings stay unknown and the mirror is stood aside");
        return false;
    }
    g_cutsceneManager = reinterpret_cast<void* const*>(*manager);
    g_root            = reinterpret_cast<void* const*>(*root);
    g_isPlaying       = reinterpret_cast<IsPlayingFn>(*isPlaying);

    const auto setSlot      = platform::FindFunction(image, facts::kCameraManagerSetSlot);
    const auto setSlotBlend = platform::FindFunction(image, facts::kPrimarySetSlotBlend);
    const auto resetSlots   = platform::FindFunction(image, facts::kCameraManagerResetSlots);
    const auto primaryCut   = platform::FindFunction(image, facts::kSeqCutPrimary);
    const auto resetSite    = image.Rebase(facts::kCutsceneEndResetCall, 5);
    if (setSlot && setSlotBlend && resetSlots && primaryCut && resetSite)
    {
        g_setSlot      = reinterpret_cast<SetSlotFn>(*setSlot);
        g_setSlotBlend = reinterpret_cast<SetSlotBlendFn>(*setSlotBlend);
        g_resetSlots   = reinterpret_cast<ResetSlotsFn>(*resetSlots);
        g_primaryCut   = reinterpret_cast<CutFn>(*primaryCut);
        g_mirrorReady  = g_resetCall.Install(*resetSite, *resetSlots, reinterpret_cast<std::uintptr_t>(&ResetSlots));
        if (!g_mirrorReady)
        {
            platform::Log(L"scene camera: the cutscene end's reset call could not be redirected; the mirror is stood aside");
        }
    }
    else
    {
        platform::Log(L"scene camera: a slot setter, the reset or the cut differs from the research; the mirror is stood aside");
    }

    using control::Register;
    using control::Shape;
    using control::Value;
    Register({.name = L"scene.playing", .shape = Shape::OnOff, .help = L"the game's cutscene manager reports a scene playing",
              .get = [] { return Value{g_scene.playing}; }});
    Register({.name = L"scene.skip", .shape = Shape::OnOff, .help = L"the cutscene manager's skip byte (the scene fast-forwards under black)",
              .get = [] { return Value{g_scene.skip}; }});
    Register({.name = L"scene.state", .shape = Shape::Text,
              .help = L"the scene signals: the cutscene manager, the field control lock, the on/off ids 12, 39/40/53, 41",
              .get  = [] { return Value{SceneText(g_scene)}; }});
    Register({.name = L"camera-slots.one", .shape = Shape::Text, .help = L"player 1's camera manager: the slot shown, the active slots, blend, cut, FOV, aspect",
              .get = [] { return Value{SlotsText(g_slots[0])}; }});
    Register({.name = L"camera-slots.two", .shape = Shape::Text, .help = L"player 2's camera manager, the same",
              .get = [] { return Value{SlotsText(g_slots[1])}; }});
    Register({.name = L"camera-slots.mirror", .shape = Shape::Text, .help = L"the mirror player's camera manager, the same",
              .get = [] { return Value{SlotsText(g_slots[2])}; }});
    Register({.name = L"scene-camera-two", .shape = Shape::OnOff,
              .help = L"the scene camera (a cutscene, a talk or event camera) on player 2's view too, while the scene holds the players",
              .set  = [](const Value& v) {
                  g_enabled = std::get<bool>(v);
                  if (!g_enabled)
                  {
                      Release(g_managerTwo);
                  }
                  platform::Log(L"scene camera: player 2's view {} the scene camera", g_enabled ? L"takes" : L"keeps its own camera through");
                  return control::Outcome{};
              },
              .get = [] { return Value{g_enabled}; }});
    Register({.name = L"scene-camera-two.state", .shape = Shape::Text, .help = L"the mirror: engaged, the slots mirrored now, its counts",
              .get = [] { return Value{MirrorText()}; }});
    platform::Log(L"scene camera: readings installed (scene.playing, scene.skip, scene.state, camera-slots.one, camera-slots.two); "
                  L"the scene camera on player 2's view {}",
                  g_mirrorReady ? L"ready (switch scene-camera-two)" : L"stood aside");
    return g_mirrorReady;
}

void Remove() noexcept
{
    Release(g_managerTwo);
    g_resetCall.Restore();
    g_mirrorReady = false;
}

void Tick(void* managerOne, void* managerTwo) noexcept
{
    if (g_cutsceneManager == nullptr)
    {
        return;
    }
    const Scene scene = ReadScene();
    if (!g_logged || !(scene == g_scene))
    {
        platform::Log(L"scene camera: {}", SceneText(scene));
    }
    const std::array<Slots, 3> slots{ReadSlots(managerOne), ReadSlots(managerTwo), ReadSlots(g_mirrors[0].manager)};
    for (int player = 0; player < 3; ++player)
    {
        const Slots&   now  = slots[static_cast<std::size_t>(player)];
        const Slots&   was  = g_slots[static_cast<std::size_t>(player)];
        const wchar_t* name = player == 0 ? L"player 1's" : player == 1 ? L"player 2's" : L"the mirror's";
        if (!now.valid && !was.valid)
        {
            continue;
        }
        // The slot and the active set at Info (a few times per scene); the blend and cut edges at Debug; the FOV and
        // the aspect, which a scene's camera cuts and the field camera's own easing move every frame, at Trace.
        if (!g_logged || now.valid != was.valid || now.current != was.current || now.active != was.active ||
            now.constrain != was.constrain)
        {
            platform::Log(L"scene camera: {} camera manager {}", name, SlotsText(now));
        }
        else if (now.blend != was.blend || now.pending != was.pending)
        {
            platform::LogDebug(L"scene camera: {} camera manager {}", name, SlotsText(now));
        }
        else if (now.fov != was.fov || now.aspect != was.aspect)
        {
            platform::LogTrace(L"scene camera: {} camera manager {}", name, SlotsText(now));
        }
    }
    if (managerTwo == nullptr && (g_mirrored != 0 || g_engaged))
    {
        g_mirrored = 0;  // player 2 left: its manager is gone with it
        g_engaged  = false;
        g_managerTwo = nullptr;
    }
    g_slots  = slots;
    g_scene  = scene;
    g_logged = true;
}

void BeforeUpdateCamera(void* managerOne, void* managerTwo) noexcept
{
    if (!g_mirrorReady || managerOne == nullptr)
    {
        return;
    }
    std::uint32_t flags = 0;
    if (managerTwo == nullptr)
    {
        // Player 1's update: the cut bit as the cut instance left it this frame (the game clears it in the update).
        // Noted whatever the switch, for the mirror player's copy too.
        g_cutNoted = Read(Bytes(managerOne) + facts::kCameraManagerCutFlags, flags) && (flags & facts::kCameraManagerCutBit) != 0;
        return;
    }
    if (!g_enabled)
    {
        return;
    }
    if (!g_cutBitStuck && g_mirrored != 0 && Read(Bytes(managerTwo) + facts::kCameraManagerCutFlags, flags) &&
        (flags & facts::kCameraManagerCutBit) != 0 && !g_cutNoted)
    {
        g_cutBitStuck = true;
        platform::Log(L"scene camera: player 2's manager still carries the cut bit at its next update (the game did not clear it there)");
    }
    Mirror(managerOne, managerTwo);
}

void SetMirror(int slot, void* controller, void* manager, void* managerOne) noexcept
{
    if (slot < 0 || slot >= kMirrorSlots)
    {
        return;
    }
    MirrorPlayer& mirror = g_mirrors[static_cast<std::size_t>(slot)];
    if (manager != mirror.manager)
    {
        mirror.mirrored = 0;  // a new manager starts from nothing; a gone one is not touched
    }
    mirror.controller = controller;
    mirror.manager    = manager;
    mirror.source     = managerOne;
}

int MirrorSlotOf(const void* manager) noexcept
{
    for (int slot = 0; manager != nullptr && slot < kMirrorSlots; ++slot)
    {
        if (g_mirrors[static_cast<std::size_t>(slot)].manager == manager)
        {
            return slot;
        }
    }
    return -1;
}

void* MirrorController(int slot) noexcept
{
    return slot >= 0 && slot < kMirrorSlots ? g_mirrors[static_cast<std::size_t>(slot)].controller : nullptr;
}

void* MirrorSource() noexcept
{
    for (const MirrorPlayer& mirror : g_mirrors)
    {
        if (mirror.manager != nullptr)
        {
            return mirror.source;
        }
    }
    return nullptr;
}

void BeforeMirrorUpdate(int slot) noexcept
{
    if (!g_mirrorReady || slot < 0 || slot >= kMirrorSlots)
    {
        return;
    }
    const MirrorPlayer& mirror = g_mirrors[static_cast<std::size_t>(slot)];
    if (mirror.manager == nullptr || mirror.source == nullptr)
    {
        return;
    }
    MirrorAll(slot);
}

std::wstring MirrorPlayerText()
{
    if (!g_mirrorReady)
    {
        return L"stood aside (an engine check failed)";
    }
    std::wstring text;
    for (int slot = 0; slot < kMirrorSlots; ++slot)
    {
        const MirrorPlayer& mirror = g_mirrors[static_cast<std::size_t>(slot)];
        if (mirror.manager == nullptr && mirror.edgesOn == 0)
        {
            continue;
        }
        text += std::format(L"{}mirror {} {}; slots set now: {}; switched on {}, off {}, poses copied {}, cuts raised {}, resets {}",
                            text.empty() ? L"" : L" | ", slot + 1, mirror.manager != nullptr ? L"copying player 1's slots" : L"no manager",
                            ActiveText(mirror.mirrored), mirror.edgesOn, mirror.edgesOff, mirror.poses, mirror.cuts, mirror.resets);
    }
    return text.empty() ? L"no mirror manager" : text;
}

}  // namespace lss::views::scene_camera_two
