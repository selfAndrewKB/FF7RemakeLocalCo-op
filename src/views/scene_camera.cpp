#include "views/scene_camera.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/field_root.h"
#include "world/shop_act.h"
#include "world/traversal_action.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace lss::views::scene_camera {

namespace {

using platform::Read;

using IsPlayingFn    = bool (*)(void* manager, std::int64_t name);
using SetSlotFn      = void (*)(void* manager, std::uint8_t slot, const void* pose);
using SetSlotBlendFn = std::int64_t (*)(std::uint8_t slot, const void* record);
using ResetSlotsFn   = std::int64_t (*)(void* manager);
using CutFn          = std::int64_t (*)();

constexpr std::uint32_t kSlots = facts::kCameraSlotCount;

/// The blend slots (an on switch and a record; the manager's own UpdateCamera runs and ends them) and, among them,
/// those seeded from the field camera's slot 24 (RE-cutscene-camera-two.md).
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

/// The mirror player's manager and its copy of every slot (views/mirror_player.h).
struct MirrorPlayer
{
    void*         controller = nullptr;
    void*         manager    = nullptr;
    void*         source     = nullptr;  // player 1's manager
    std::uint32_t mirrored   = 0;        // bit i = slot i set on the mirror's manager
    std::uint32_t edgesOn = 0, edgesOff = 0, poses = 0, cuts = 0, resets = 0;
};

/// One seat's copy of the scene: its manager and the slots set on it while the scene holds the players.
struct SeatCopy
{
    void*         manager   = nullptr;  // the manager the copy last wrote, for the reset redirect
    std::uint32_t mirrored  = 0;        // bit i = slot i of the seat's manager set by the copy
    bool          engaged   = false;
    bool          cutBitStuck = false;  // the seat's manager still carried the bit at its next update (logged once)
    bool          ownScene    = false;  // the seat's own shop camera runs (world/shop_act.h): no copy
    bool          playerOne   = false;  // the seat's column shows player 1's picture (views/column_source.h): every slot copied
    std::uint32_t edgesOn = 0, edgesOff = 0, poses = 0, cuts = 0, resets = 0, releases = 0;
};

// Game thread.
constexpr int kSeats     = players::scopes::kMaxPlayers;              // by seat; [0] unused (player 1's is the source)
constexpr int kReadSlots = 1 + players::kSeatsBuilt + kMirrorSlots;  // the managers read for the readings
std::array<Slots, kReadSlots>          g_slots;    // player 1's, the seats' in order, the mirrors'
std::array<MirrorPlayer, kMirrorSlots> g_mirrors;  // one copy record per mirror slot (views/mirror_player)
std::array<SeatCopy, kSeats>           g_seats;
static_assert(players::kSeatsBuilt + 1 <= 4 && kMirrorSlots <= 2, "the readings' names (ReadName, the registration loop)");
Scene                g_scene;
bool                 g_logged = false;
bool                 g_enabled = true;
bool                 g_cutNoted   = false;  // player 1's manager carried the cut bit at its update this frame

// The copies' log names (CopySlots), one per target: no string built per frame.
constexpr const wchar_t* kSeatManager[4]      = {L"player 1's manager", L"player 2's manager", L"player 3's manager", L"player 4's manager"};
constexpr const wchar_t* kSeatManagerOnOne[4] = {L"player 1's manager", L"player 2's manager (on player 1's picture)",
                                                 L"player 3's manager (on player 1's picture)", L"player 4's manager (on player 1's picture)"};
constexpr const wchar_t* kMirrorPrefix[2]     = {L"mirror camera 1", L"mirror camera 2"};
static_assert(kSeats <= 4 && kMirrorSlots <= 2, "the log names");

const wchar_t* ReadName(int index) noexcept
{
    if (index <= players::kSeatsBuilt)
    {
        constexpr const wchar_t* kNames[] = {L"player 1's", L"player 2's", L"player 3's", L"player 4's"};
        return kNames[index];
    }
    return index == players::kSeatsBuilt + 1 ? L"the first mirror's" : L"the second mirror's";
}

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
    const std::uint8_t* stateManager = players::field_root::StateManager();
    if (stateManager != nullptr && players::field_root::ControlLock(now.lockMask, now.lockCount))
    {
        now.valid     = true;
        now.mover     = world::traversal_action::PlayerOneMoveBitsIn(now.lockMask);
        using players::field_root::OnOff;
        now.skippable = OnOff(12);
        now.black     = OnOff(39) || OnOff(40) || OnOff(53);
        now.loading   = OnOff(41);
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
    const std::wstring mover = scene.mover != 0 ? std::format(L"{:#x} = player 1's own move; ", scene.mover) : L"";
    return std::format(L"cutscene {}{}; control lock {:#x} ({}count {}); skippable scene {}; held black {}; loading screen {}{}",
                       !scene.manager ? L"no manager" : scene.playing ? L"playing" : L"not playing", scene.skip ? L" (skipping)" : L"",
                       scene.lockMask, mover, scene.lockCount, scene.skippable ? L"on" : L"off", scene.black ? L"on" : L"off",
                       scene.loading ? L"on" : L"off", scene.valid ? L"" : L" (state manager unreadable)");
}

std::wstring MirrorText()
{
    std::wstring text = std::format(L"{}{}", g_enabled ? L"on" : L"off", !g_mirrorReady ? L" (stood aside: an engine check failed)" : L"");
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const SeatCopy& s = g_seats[static_cast<std::size_t>(seat)];
        text += std::format(L"; {} {}: slots mirrored now: {}; switched on {}, off {}, poses copied {}, cuts raised {}, resets {}, "
                            L"releases {}",
                            players::SeatName(seat), s.playerOne ? L"on player 1's picture" : s.engaged ? L"engaged" : L"idle",
                            ActiveText(s.mirrored), s.edgesOn, s.edgesOff,
                            s.poses, s.cuts, s.resets, s.releases);
    }
    return text;
}

/// One slot of a seat's manager set from a 48-byte pose through the game's setter.
bool SetSlot(void* manager, std::uint32_t slot, const std::uint8_t (&pose)[facts::kCameraSlotBytes]) noexcept
{
    return platform::CallGuarded(g_setSlot, manager, static_cast<std::uint8_t>(slot), static_cast<const void*>(pose));
}

/// The seat's copied pose slots switched off on `manager` (a scene's release, the mod's switch, a leave).
void Release(SeatCopy& s, void* manager) noexcept
{
    if (manager != nullptr && s.mirrored != 0)
    {
        for (std::uint32_t i = 0; i < kSlots; ++i)
        {
            if (i == facts::kCameraSlotField || ((s.mirrored >> i) & 1u) == 0 || IsBlendSlot(i))
            {
                continue;  // the seat's own field camera stays on; a blend still running on the seat's manager ends by itself
            }
            std::uint8_t pose[facts::kCameraSlotBytes]{};
            if (platform::CopyGuarded(pose, SlotAt(manager, i), sizeof(pose)))
            {
                pose[facts::kCameraSlotActive] = 0;
                SetSlot(manager, i, pose);
            }
        }
        ++s.releases;
    }
    s.mirrored = 0;
    s.engaged  = false;
}

void ReleaseAll() noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        SeatCopy& s = g_seats[static_cast<std::size_t>(seat)];
        Release(s, s.manager);
    }
}

/// Player 1's manager's slots (`one` as read, `source` its manager) set on `target` through the game's setters, as
/// `record` keeps them (its `mirrored` bits and counts): the scene slots (`allSlots` false: every slot but the field
/// camera's 24, a blend from the field seeded with the target's own 24, so it starts from that view's camera) or
/// every slot (`allSlots` true: the mirror look, a blend seeded as player 1's was). A pose slot is copied every frame
/// while on for either; a blend slot once as it turns on for player 1; player 1's cut raised on the target. `prefix`
/// and `who` name the log lines.
template <class Record>
void CopySlots(Record& record, const Slots& one, void* source, void* target, bool allSlots, const wchar_t* prefix,
               const wchar_t* who) noexcept
{
    for (std::uint32_t i = 0; i < kSlots; ++i)
    {
        if (!allSlots && i == facts::kCameraSlotField)
        {
            continue;  // the seat's own field camera
        }
        const bool   activeOne = ((one.active >> i) & 1u) != 0;
        const bool   mirrored  = ((record.mirrored >> i) & 1u) != 0;
        std::uint8_t pose[facts::kCameraSlotBytes]{};
        if (IsBlendSlot(i))
        {
            if (activeOne && !mirrored)
            {
                // The on switch: the record, then the seed.
                std::uint8_t        blend[facts::kCameraSlotBlendBytes]{};
                const bool          ownSeed = !allSlots && SeededFromField(i);
                const std::uint8_t* seed    = ownSeed ? SlotAt(target, facts::kCameraSlotField) : SlotAt(source, i);
                if (platform::CopyGuarded(blend, RecordAt(source, i), sizeof(blend)) && platform::CopyGuarded(pose, seed, sizeof(pose)))
                {
                    std::int64_t unused = 0;
                    platform::CallGuarded(g_setSlotBlend, unused, static_cast<std::uint8_t>(i), static_cast<const void*>(blend));
                    pose[facts::kCameraSlotActive] = 1;
                    if (SetSlot(target, i, pose))
                    {
                        record.mirrored |= 1u << i;
                        ++record.edgesOn;
                        platform::LogDebug(L"{}: blend slot {} switched on for {} (seed {}; record kind {}, {} ms)", prefix, i, who,
                                           ownSeed ? L"its own field camera" : L"player 1's slot", blend[0],
                                           *reinterpret_cast<const std::int32_t*>(blend + 4));
                    }
                }
            }
            else if (!activeOne && mirrored)
            {
                record.mirrored &= ~(1u << i);  // the target manager ends its own blend
                ++record.edgesOff;
                platform::LogDebug(L"{}: blend slot {} off for player 1; {} ends its own by itself", prefix, i, who);
            }
            continue;
        }
        // A pose slot: copied every frame while on for either, the active byte with it.
        if (!activeOne && !mirrored)
        {
            continue;
        }
        if (!platform::CopyGuarded(pose, SlotAt(source, i), sizeof(pose)) || !SetSlot(target, i, pose))
        {
            continue;
        }
        ++record.poses;
        if (activeOne && !mirrored)
        {
            record.mirrored |= 1u << i;
            ++record.edgesOn;
            platform::LogDebug(L"{}: pose slot {} copied to {} (fov {:.1f})", prefix, i, who,
                               *reinterpret_cast<const float*>(pose + facts::kCameraSlotFov));
        }
        else if (!activeOne && mirrored)
        {
            record.mirrored &= ~(1u << i);
            ++record.edgesOff;
            platform::LogDebug(L"{}: pose slot {} off on player 1's manager and {}", prefix, i, who);
        }
    }
    // The cut: player 1's manager carried the bit at its update; the game's primary-scoped cut raises the target's.
    if (g_cutNoted && (allSlots || record.mirrored != 0) && g_primaryCut != nullptr)
    {
        std::int64_t unused = 0;
        if (platform::CallGuarded(g_primaryCut, unused))
        {
            ++record.cuts;
            platform::LogDebug(L"{}: camera cut raised on {}", prefix, who);
        }
    }
    // The note is player 1's update's, rewritten every frame; every later update this frame reads it too.
}

/// The scene slots of player 1's manager onto the seat's (the caller holds the seat's primary scope).
void Mirror(void* managerOne, int seat, void* manager) noexcept
{
    SeatCopy&      s    = g_seats[static_cast<std::size_t>(seat)];
    const wchar_t* who  = players::SeatName(seat);
    const Slots    one  = ReadSlots(managerOne);
    if (!one.valid)
    {
        return;
    }
    const bool holds = g_scene.Holds();
    if (!s.engaged && holds)
    {
        s.engaged = true;
        platform::Log(L"scene camera: the scene holds the players; {}'s view takes the scene camera (player 1's manager {})", who,
                      SlotsText(one));
    }
    if (s.engaged && !holds)
    {
        platform::Log(L"scene camera: the scene released the players; {}'s view back to its own camera (mirrored slots {}: the pose "
                      L"slots switched off, a blend finishes by itself)",
                      who, ActiveText(s.mirrored));
        Release(s, manager);
        return;
    }
    if (!s.engaged)
    {
        return;
    }
    s.manager = manager;
    CopySlots(s, one, managerOne, manager, false, L"scene camera", kSeatManager[static_cast<std::size_t>(seat)]);
}

/// Every slot of player 1's manager onto mirror `slot`'s (the caller holds the mirror's primary scope).
void MirrorAll(int slot) noexcept
{
    MirrorPlayer& mirror = g_mirrors[static_cast<std::size_t>(slot)];
    const Slots   one    = ReadSlots(mirror.source);
    if (!one.valid || mirror.manager == nullptr)
    {
        return;
    }
    CopySlots(mirror, one, mirror.source, mirror.manager, true, kMirrorPrefix[static_cast<std::size_t>(slot)], L"the mirror's manager");
}

/// The cutscene end's reset, redirected: the game's call for the primary manager, then the same on every seat's and
/// on the mirror players'.
std::int64_t ResetSlots(void* manager)
{
    const auto original = reinterpret_cast<ResetSlotsFn>(g_resetCall.Original());
    const std::int64_t result = original(manager);
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        SeatCopy& s = g_seats[static_cast<std::size_t>(seat)];
        if (g_enabled && s.manager != nullptr && s.manager != manager && s.mirrored != 0)
        {
            original(s.manager);
            s.mirrored = 0;
            ++s.resets;
            platform::Log(L"scene camera: the cutscene's end reset ran on {}'s manager too (every slot off, the field camera on)",
                          players::SeatName(seat));
        }
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
    const auto isPlaying = platform::FindFunction(image, facts::kCutsceneIsPlaying);
    if (!manager || !players::field_root::Installed() || !isPlaying)
    {
        platform::Log(L"scene camera: an engine check failed; the scene readings stay unknown and the mirror is stood aside");
        return false;
    }
    g_cutsceneManager = reinterpret_cast<void* const*>(*manager);
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
    // One reading per manager read: player 1's, each seat's, each mirror's (camera-slots.one, .two, .three, .mirror,
    // .mirror-2).
    constexpr const wchar_t* kReadingWords[] = {L"one", L"two", L"three", L"four"};
    for (int index = 0; index < kReadSlots; ++index)
    {
        const bool   mirror = index > players::kSeatsBuilt;
        const int    slot   = index - players::kSeatsBuilt - 1;
        std::wstring name   = mirror ? (slot == 0 ? std::wstring(L"camera-slots.mirror") : std::format(L"camera-slots.mirror-{}", slot + 1))
                                     : std::format(L"camera-slots.{}", kReadingWords[index]);
        std::wstring help   = index == 0 ? std::wstring(L"player 1's camera manager: the slot shown, the active slots, blend, cut, FOV, aspect")
                                         : std::format(L"{} camera manager, the same", ReadName(index));
        Register({.name = std::move(name), .shape = Shape::Text, .help = std::move(help),
                  .get = [index] { return Value{SlotsText(g_slots[static_cast<std::size_t>(index)])}; }});
    }
    Register({.name = L"scene-camera", .shape = Shape::OnOff,
              .help = L"the scene camera (a cutscene, a talk or event camera) on every further player's view too, while the scene "
                      L"holds the players",
              .set  = [](const Value& v) {
                  g_enabled = std::get<bool>(v);
                  if (!g_enabled)
                  {
                      ReleaseAll();
                  }
                  platform::Log(L"scene camera: the further players' views {} the scene camera", g_enabled ? L"take" : L"keep their own camera through");
                  return control::Outcome{};
              },
              .get = [] { return Value{g_enabled}; }});
    Register({.name = L"scene-camera.state", .shape = Shape::Text, .help = L"the mirror: engaged, the slots mirrored now, its counts",
              .get = [] { return Value{MirrorText()}; }});
    platform::Log(L"scene camera: readings installed (scene.playing, scene.skip, scene.state, camera-slots.one, .two, .three, .mirror, .mirror-2); "
                  L"the scene camera on the further players' views {}",
                  g_mirrorReady ? L"ready (switch scene-camera)" : L"stood aside");
    return g_mirrorReady;
}

void Remove() noexcept
{
    ReleaseAll();
    g_resetCall.Restore();
    g_mirrorReady = false;
}

void Tick(void* managerOne, const SeatManagers& seatManagers) noexcept
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
    std::array<Slots, kReadSlots> slots{};
    slots[0] = ReadSlots(managerOne);
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        slots[static_cast<std::size_t>(seat)] = ReadSlots(seatManagers[static_cast<std::size_t>(seat)]);
    }
    for (int slot = 0; slot < kMirrorSlots; ++slot)
    {
        slots[static_cast<std::size_t>(1 + players::kSeatsBuilt + slot)] = ReadSlots(g_mirrors[static_cast<std::size_t>(slot)].manager);
    }
    for (int player = 0; player < kReadSlots; ++player)
    {
        const Slots&   now  = slots[static_cast<std::size_t>(player)];
        const Slots&   was  = g_slots[static_cast<std::size_t>(player)];
        const wchar_t* name = ReadName(player);
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
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        SeatCopy& s = g_seats[static_cast<std::size_t>(seat)];
        void*     manager = seatManagers[static_cast<std::size_t>(seat)];
        if (manager == nullptr || (s.manager != nullptr && manager != s.manager))
        {
            // The seat is out (its manager is gone with it), or came back with another: its copy starts from nothing.
            s.mirrored = 0;
            s.engaged  = false;
            s.manager  = nullptr;
        }
    }
    g_slots  = slots;
    g_scene  = scene;
    g_logged = true;
}

void BeforeUpdateCamera(void* managerOne, int seat, void* manager) noexcept
{
    if (!g_mirrorReady || managerOne == nullptr)
    {
        return;
    }
    std::uint32_t flags = 0;
    if (seat == 0)
    {
        // Player 1's update: the cut bit as the cut instance left it this frame (the game clears it in the update).
        // Noted whatever the switch, for the mirror player's copy too.
        g_cutNoted = Read(Bytes(managerOne) + facts::kCameraManagerCutFlags, flags) && (flags & facts::kCameraManagerCutBit) != 0;
        return;
    }
    if (seat < 1 || seat > players::kSeatsBuilt || manager == nullptr)
    {
        return;
    }
    SeatCopy& s = g_seats[static_cast<std::size_t>(seat)];
    if (s.playerOne)
    {
        // The seat's column shows player 1's picture (views/column_source.h): every slot of player 1's, as a mirror's,
        // whatever the scene switch (that switch is the scene on further views; the decision keeps a shop's opener Self).
        const Slots one = ReadSlots(managerOne);
        if (one.valid)
        {
            s.manager = manager;
            CopySlots(s, one, managerOne, manager, true, L"scene camera", kSeatManagerOnOne[static_cast<std::size_t>(seat)]);
        }
        return;
    }
    if (!g_enabled)
    {
        return;
    }
    if (world::shop_act::OwnsScene(seat))
    {
        // The seat's own shop: its act sets the shop camera on the seat's manager (world/shop_act.h); player 1's slots
        // are not copied over it.
        if (!s.ownScene)
        {
            s.ownScene = true;
            platform::LogDebug(L"scene camera: {}'s own shop camera; player 1's scene slots are not copied to its view",
                               players::SeatName(seat));
        }
        // The shop's own camera slot (14, written by its act every frame and switched off by its finish) is the act's
        // on this manager now: never switched off as a mirrored one. Any other mirrored slot is released.
        s.mirrored &= ~(1u << 14);
        if (s.mirrored != 0)
        {
            Release(s, manager);
        }
        return;
    }
    s.ownScene = false;
    if (!s.cutBitStuck && s.mirrored != 0 && Read(Bytes(manager) + facts::kCameraManagerCutFlags, flags) &&
        (flags & facts::kCameraManagerCutBit) != 0 && !g_cutNoted)
    {
        s.cutBitStuck = true;
        platform::Log(L"scene camera: {}'s manager still carries the cut bit at its next update (the game did not clear it there)",
                      players::SeatName(seat));
    }
    Mirror(managerOne, seat, manager);
}

void SetSource(int seat, bool playerOne, void* manager) noexcept
{
    if (!players::SeatBuilt(seat))
    {
        return;
    }
    SeatCopy& s = g_seats[static_cast<std::size_t>(seat)];
    if (s.playerOne == playerOne)
    {
        return;
    }
    s.playerOne = playerOne;
    if (playerOne)
    {
        s.engaged = false;  // the every-slot copy covers the scene slots; a scene's release is not awaited
    }
    else if (manager != nullptr && manager == s.manager)
    {
        Release(s, manager);  // back to its own view: the copied slots off (Release keeps its own field camera on)
    }
    else
    {
        s.mirrored = 0;  // the seat left, or came back with another manager: nothing to switch off on the old one
        s.engaged  = false;
    }
}

bool ShowsPlayerOne(int seat) noexcept
{
    if (!players::SeatBuilt(seat))
    {
        return false;
    }
    const SeatCopy& s = g_seats[static_cast<std::size_t>(seat)];
    return s.playerOne || (s.engaged && s.mirrored != 0);
}

bool EventShot() noexcept
{
    return g_slots[0].valid && ((g_slots[0].active >> facts::kCameraSlotEvent) & 1U) != 0;
}

bool SlotActive(const void* manager, std::uint32_t slot) noexcept
{
    std::uint8_t active = 0;
    return manager != nullptr && slot < kSlots &&
           Read(Bytes(manager) + facts::kCameraManagerSlots + slot * facts::kCameraSlotBytes + facts::kCameraSlotActive, active) &&
           active != 0;
}

Scene ReadSceneNow() noexcept
{
    return g_cutsceneManager == nullptr ? Scene{} : ReadScene();
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
        // A manager another slot's record already copies to (the mirror module moved it down a slot): the record
        // moves with it, its slots set as they are. Else a new manager starts from nothing; a gone one is not touched.
        bool moved = false;
        for (int other = 0; manager != nullptr && other < kMirrorSlots && !moved; ++other)
        {
            MirrorPlayer& record = g_mirrors[static_cast<std::size_t>(other)];
            if (other != slot && record.manager == manager)
            {
                mirror = record;
                record = MirrorPlayer{};
                moved  = true;
            }
        }
        if (!moved)
        {
            mirror.mirrored = 0;
        }
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

}  // namespace lss::views::scene_camera
