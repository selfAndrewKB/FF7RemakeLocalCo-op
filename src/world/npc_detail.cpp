#include "world/npc_detail.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/field_root.h"
#include "players/seats.h"
#include "world/anim_choice.h"
#include "world/charapop_rule.h"

#include <Unreal/NameTypes.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>

namespace lss::world::npc_detail {

namespace {

using platform::Read;
using platform::ReadPointer;
using players::kSeatsBuilt;
using players::SeatBuilt;
using players::SeatName;

constexpr int kMaxPlayers = players::scopes::kMaxPlayers;

using SetMinLevelFn = bool (*)(const std::uint64_t* objectName, std::uint8_t level, bool evaluateNow);

constexpr std::uint32_t kEvery           = 10;   // ticks between passes
constexpr std::uint32_t kReleasePasses   = 6;    // a raised entry goes back after this many unneeded passes (~1 s)
constexpr std::int32_t  kFirstJudged     = 3;    // the LOD evaluator runs for states 3..8
constexpr std::int32_t  kLastJudged      = 8;
constexpr std::uint8_t  kLevelLight      = 1;
constexpr std::uint8_t  kLevelMiddle     = 2;
constexpr std::uint8_t  kLevelHeavy      = 3;
constexpr std::uint8_t  kCategoryHides   = 1;
constexpr std::uint8_t  kCategoryEnemy   = 3;
constexpr int           kMaxRaised       = 128;
constexpr auto          kSummaryEvery    = std::chrono::seconds(60);

struct Raised
{
    std::uint64_t name      = 0;
    std::uint8_t* record    = nullptr;
    std::uint8_t  baseLevel = 0;  // the entry's level before it was raised: picks Light or Middle
    std::uint32_t outside   = 0;  // passes in a row it was not needed
    bool          seen      = false;
};

/// One camera as the evaluator sees it: the cached POV location and its forward.
struct Camera
{
    anim_choice::Vec location;
    anim_choice::Vec forward;
    bool             read = false;
};

/// The further players' views of one entry, in seat order: the seats whose camera read this pass.
struct SeatViews
{
    std::array<charapop::EntryView, kMaxPlayers> views{};
    std::array<int, kMaxPlayers>                 seatOf{};
    int                                          count = 0;
};

SetMinLevelFn   g_setMinLevel = nullptr;
bool            g_enabled     = true;
std::array<void*, kMaxPlayers> g_cameras{};  // each seat's camera manager while its player is in; [0] unused

std::array<Raised, kMaxRaised> g_raised{};
int                            g_raisedCount = 0;
std::uint8_t*                  g_manager     = nullptr;  // the CharaPop the records belong to
std::uint32_t                  g_ticks       = 0;
std::uint32_t g_raisedTotal = 0, g_outOfReach = 0, g_covered = 0, g_gone = 0, g_takenBack = 0;
int           g_needed = 0, g_coveredNow = 0;  // the last pass
bool          g_fullLogged = false;
std::chrono::steady_clock::time_point g_lastSummary = std::chrono::steady_clock::now();

void NameText(std::int64_t bits, std::wstring* out) noexcept
try
{
    *out = RC::Unreal::FName(bits).ToString();
}
catch (...)
{
    out->clear();
}

std::wstring Name(std::uint64_t bits) noexcept
{
    std::wstring text;
    if (!platform::CallGuarded(&NameText, static_cast<std::int64_t>(bits), &text) || text.empty())
    {
        return std::format(L"name#{:x}", bits);
    }
    return text;
}

template <class T>
T Field(const void* object, std::uint32_t offset) noexcept
{
    T value{};
    Read(static_cast<const std::uint8_t*>(object) + offset, value);
    return value;
}

Camera CameraOf(void* manager) noexcept
{
    Camera camera;
    float  rotation[3]{};
    const auto* base = static_cast<const std::uint8_t*>(manager);
    camera.read      = manager != nullptr && Read(base + facts::kCameraManagerPovLocation, camera.location) &&
                  Read(base + facts::kCameraManagerPovRotation, rotation);
    camera.forward = anim_choice::ForwardOf(rotation[0], rotation[1]);
    return camera;
}

charapop::EntryView ViewFrom(const Camera& camera, const anim_choice::Vec& at, std::uint8_t category) noexcept
{
    const anim_choice::View v = anim_choice::Measure(at, camera.location, camera.forward);
    return {v.distance, v.angle * 57.29578f, category == kCategoryHides, category == kCategoryEnemy};
}

/// The distances the evaluator uses for an entry of `level`, the manager's overrides applied.
charapop::LodDistances DistancesFor(const std::uint8_t* manager, std::uint8_t level) noexcept
{
    const bool             middle = level == kLevelMiddle;
    charapop::LodDistances lod;
    lod.fovDegrees = Field<float>(manager, facts::kCharaPopFov);
    lod.shouldTick = Field<float>(manager, middle ? facts::kCharaPopMiddleShouldTick : facts::kCharaPopLightShouldTick);
    lod.inCamera   = Field<float>(manager, middle ? facts::kCharaPopMiddleInCamera : facts::kCharaPopLightInCamera);
    lod.outCamera  = Field<float>(manager, middle ? facts::kCharaPopMiddleOutCamera : facts::kCharaPopLightOutCamera);
    lod.enableDraw = Field<float>(manager, middle ? facts::kCharaPopMiddleDraw : facts::kCharaPopLightDraw);
    lod.enemyAdded = Field<float>(manager, facts::kCharaPopEnemyInCameraAdd);
    if (Field<std::uint8_t>(manager, facts::kCharaPopOverrideInCamera) != 0)
    {
        lod.inCamera = Field<float>(manager, facts::kCharaPopOverrideInCamera + 4);
    }
    if (Field<std::uint8_t>(manager, facts::kCharaPopOverrideOutCamera) != 0)
    {
        lod.outCamera = Field<float>(manager, facts::kCharaPopOverrideOutCamera + 4);
    }
    if (Field<std::uint8_t>(manager, facts::kCharaPopOverrideDraw) != 0)
    {
        lod.enableDraw = Field<float>(manager, facts::kCharaPopOverrideDraw + 4);
    }
    return lod;
}

bool SetMinLevel(std::uint64_t name, std::uint8_t level) noexcept
{
    bool found = false;
    return platform::CallGuarded(g_setMinLevel, found, static_cast<const std::uint64_t*>(&name), level, true) && found;
}

/// The record still names the entry the mod raised, and it still holds the mod's 3.
bool StillRaised(const Raised& raised) noexcept
{
    return Field<std::uint64_t>(raised.record, facts::kCharaPopRecordName) == raised.name &&
           Field<std::uint8_t>(raised.record, facts::kCharaPopRecordMinLevel) == kLevelHeavy;
}

void Drop(int index) noexcept
{
    g_raised[static_cast<std::size_t>(index)]               = g_raised[static_cast<std::size_t>(g_raisedCount - 1)];
    g_raised[static_cast<std::size_t>(g_raisedCount - 1)] = {};
    --g_raisedCount;
}

/// Clears `index` (only if it is still the mod's 3) and drops the record. True if it was cleared.
bool Clear(int index) noexcept
{
    const Raised& raised  = g_raised[static_cast<std::size_t>(index)];
    const bool    cleared = StillRaised(raised);
    if (cleared)
    {
        SetMinLevel(raised.name, 0);
    }
    else
    {
        ++g_takenBack;
    }
    Drop(index);
    return cleared;
}

void ClearAll(const wchar_t* why) noexcept
{
    if (g_raisedCount == 0)
    {
        return;
    }
    const int count = g_raisedCount;
    while (g_raisedCount > 0)
    {
        Clear(g_raisedCount - 1);
    }
    platform::Log(L"npc detail: cleared {} raised NPC(s) ({})", count, why);
}

int Find(const std::uint8_t* record) noexcept
{
    for (int i = 0; i < g_raisedCount; ++i)
    {
        if (g_raised[static_cast<std::size_t>(i)].record == record)
        {
            return i;
        }
    }
    return -1;
}

std::wstring Where(const charapop::EntryView* view) noexcept
try
{
    return view != nullptr ? std::format(L"{:.0f} m at {:.0f} deg", view->distance / 100.0f, view->angleDegrees)
                           : std::wstring(L"unread");
}
catch (...)
{
    return {};
}

/// Each further player's view of the entry, named: "player 2: 20 m at 10 deg, player 3: ...".
std::wstring WhereSeats(const SeatViews& seats) noexcept
try
{
    std::wstring out;
    for (int i = 0; i < seats.count; ++i)
    {
        out += std::format(L"{}{}: {}", i == 0 ? L"" : L", ", SeatName(seats.seatOf[static_cast<std::size_t>(i)]),
                           Where(&seats.views[static_cast<std::size_t>(i)]));
    }
    return out;
}
catch (...)
{
    return {};
}

void HandleRaised(int index, const charapop::LodDistances& lod, const SeatViews& seats, const charapop::EntryView* one) noexcept
{
    Raised& raised = g_raised[static_cast<std::size_t>(index)];
    raised.seen    = true;
    if (Field<std::uint8_t>(raised.record, facts::kCharaPopRecordMinLevel) != kLevelHeavy)
    {
        ++g_takenBack;  // the game changed the minimum: it is the game's now
        Drop(index);
        return;
    }
    const charapop::Hold hold = charapop::HoldOf(lod, seats.views.data(), seats.count, one, 1.0f);
    if (hold != charapop::Hold::OutOfReach)
    {
        ++g_needed;
        g_coveredNow += hold == charapop::Hold::PlayerOneCovers ? 1 : 0;
    }
    raised.outside = hold == charapop::Hold::Keep ? 0 : raised.outside + 1;
    if (raised.outside < kReleasePasses)
    {
        return;
    }
    const bool covered = hold == charapop::Hold::PlayerOneCovers;
    if (platform::Enabled(platform::Level::Debug))
    {
        platform::LogDebug(L"npc detail: released {} ({}; {}, player 1: {})", Name(raised.name),
                           covered ? L"player 1's camera keeps it" : L"out of every further player's reach", WhereSeats(seats),
                           Where(one));
    }
    if (Clear(index))
    {
        ++(covered ? g_covered : g_outOfReach);
    }
}

void HandleCandidate(std::uint8_t* record, std::uint8_t level, const charapop::LodDistances& lod, const SeatViews& seats,
                     const charapop::EntryView* one) noexcept
{
    bool needed = false;
    for (int i = 0; i < seats.count && !needed; ++i)
    {
        needed = charapop::KeepsAwakeOrShown(lod, seats.views[static_cast<std::size_t>(i)], 1.0f);
    }
    if (!needed)
    {
        return;
    }
    ++g_needed;
    if (!charapop::ShouldRaise(lod, seats.views.data(), seats.count, one, 1.0f))
    {
        ++g_coveredNow;
        return;
    }
    if (g_raisedCount == kMaxRaised)
    {
        if (!g_fullLogged)
        {
            g_fullLogged = true;
            platform::Log(L"npc detail: {} NPCs raised, the most kept: more NPCs near the further players are left as the "
                          L"game has them",
                          kMaxRaised);
        }
        return;
    }
    const auto name = Field<std::uint64_t>(record, facts::kCharaPopRecordName);
    if (name == 0 || !SetMinLevel(name, kLevelHeavy))
    {
        return;
    }
    g_raised[static_cast<std::size_t>(g_raisedCount++)] = {name, record, level, 0, true};
    ++g_raisedTotal;
    if (platform::Enabled(platform::Level::Debug))
    {
        platform::LogDebug(L"npc detail: raised {} (level {}; {}, player 1: {}, which does not keep it)", Name(name), level,
                           WhereSeats(seats), Where(one));
    }
}

void Pass(void* cameraOne) noexcept
{
    std::uint8_t* manager = static_cast<std::uint8_t*>(ReadPointer(players::field_root::Root(), facts::kRootCharaPop));
    if (manager != g_manager)
    {
        if (g_raisedCount > 0)
        {
            platform::Log(L"npc detail: forgot {} raised NPC(s) (a new map's NPC list)", g_raisedCount);
        }
        g_raised      = {};
        g_raisedCount = 0;
        g_manager     = manager;
    }
    bool anySeat = false;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        anySeat = anySeat || g_cameras[static_cast<std::size_t>(seat)] != nullptr;
    }
    if (!g_enabled || !anySeat || manager == nullptr)
    {
        ClearAll(!g_enabled ? L"switched off" : L"every further player left");
        return;
    }
    // The cameras this pass judges from: player 1's, and each seat's that reads.
    const Camera one = CameraOf(cameraOne);
    std::array<Camera, kMaxPlayers> cameras{};
    bool                            anyRead = false, anyUnread = false;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        void* const manager = g_cameras[static_cast<std::size_t>(seat)];
        cameras[static_cast<std::size_t>(seat)] = CameraOf(manager);
        anyRead   = anyRead || cameras[static_cast<std::size_t>(seat)].read;
        anyUnread = anyUnread || (manager != nullptr && !cameras[static_cast<std::size_t>(seat)].read);
    }
    if (!anyRead || anyUnread)
    {
        return;  // a further player's camera unreadable this pass: everything held, as one unread camera always did
    }
    auto* records = static_cast<std::uint8_t*>(ReadPointer(manager, facts::kCharaPopRecordsHeap));
    if (records == nullptr)
    {
        records = manager + facts::kCharaPopRecordsInline;
    }
    for (int i = 0; i < g_raisedCount; ++i)
    {
        g_raised[static_cast<std::size_t>(i)].seen = false;
    }
    g_needed = g_coveredNow = 0;
    for (std::int32_t i = 0; i < facts::kCharaPopRecordCount; ++i)
    {
        std::uint8_t* record = records + i * facts::kCharaPopRecordBytes;
        const auto    state  = Field<std::int32_t>(record, facts::kCharaPopRecordState);
        if (Field<std::uint8_t>(record, facts::kCharaPopRecordInUse) == 0 || state < kFirstJudged || state > kLastJudged)
        {
            continue;
        }
        void*            actor = ReadPointer(record, facts::kCharaPopRecordActor);
        void*            rootComponent = ReadPointer(actor, facts::kActorRootComponent);
        anim_choice::Vec at;
        if (rootComponent == nullptr ||
            !Read(static_cast<const std::uint8_t*>(rootComponent) + facts::kComponentWorldLocation, at))
        {
            continue;
        }
        const int          index    = Find(record);
        const std::uint8_t level    = index >= 0 ? g_raised[static_cast<std::size_t>(index)].baseLevel
                                                 : Field<std::uint8_t>(record, facts::kCharaPopRecordLevel);
        if (level != kLevelLight && level != kLevelMiddle)
        {
            continue;  // Heavy entries are awake and shown already
        }
        const auto                   category = Field<std::uint8_t>(record, facts::kCharaPopRecordCategory);
        const charapop::LodDistances lod      = DistancesFor(manager, level);
        SeatViews                    seats;
        for (int seat = 1; seat <= kSeatsBuilt; ++seat)
        {
            const Camera& camera = cameras[static_cast<std::size_t>(seat)];
            if (camera.read)
            {
                seats.views[static_cast<std::size_t>(seats.count)]  = ViewFrom(camera, at, category);
                seats.seatOf[static_cast<std::size_t>(seats.count)] = seat;
                ++seats.count;
            }
        }
        const charapop::EntryView  fromOne = ViewFrom(one, at, category);
        const charapop::EntryView* onePtr  = one.read ? &fromOne : nullptr;
        if (index >= 0)
        {
            HandleRaised(index, lod, seats, onePtr);
        }
        else if (Field<std::uint8_t>(record, facts::kCharaPopRecordMinLevel) == 0)
        {
            HandleCandidate(record, level, lod, seats, onePtr);
        }
    }
    // A raised entry no longer judged (despawned, freed): cleared if it is still the mod's, then forgotten.
    for (int i = g_raisedCount - 1; i >= 0; --i)
    {
        if (!g_raised[static_cast<std::size_t>(i)].seen)
        {
            if (Clear(i))
            {
                ++g_gone;
            }
        }
    }
}

void Summarise() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastSummary < kSummaryEvery)
    {
        return;
    }
    g_lastSummary = now;
    if (platform::Enabled(platform::Level::Debug))
    {
        platform::LogDebug(L"npc detail: {}", Reading());  // the reading's text formed only when it is written
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto set  = platform::FindFunction(image, facts::kCharaPopSetMinLevel);
    if (!set || !players::field_root::Installed())
    {
        platform::LogWarning(L"npc detail: a check failed; NPCs near a further player keep the detail player 1's camera gives them");
        return false;
    }
    g_setMinLevel = reinterpret_cast<SetMinLevelFn>(*set);
    control::Register({.name  = L"npc-detail",
                       .shape = control::Shape::OnOff,
                       .help  = L"NPCs near a further player keep the detail that player's camera would give them (world/npc_detail.h; on)",
                       .set   = [](const control::Value& value) {
                           SetEnabled(std::get<bool>(value));
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled}; }});
    control::Register({.name = L"world.npc-detail", .shape = control::Shape::Text,
                       .help = L"NPCs raised for the further players' views now, and the counts so far",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"npc detail: installed; while a further player is in, the NPCs their cameras need are kept awake and shown "
                  L"with the game's own setter");
    return true;
}

void TickSeat(int seat, void* seatCameraManager) noexcept
{
    if (g_setMinLevel == nullptr || !SeatBuilt(seat))
    {
        return;
    }
    g_cameras[static_cast<std::size_t>(seat)] = seatCameraManager;
}

void Tick(void* cameraManagerOne) noexcept
{
    if (g_setMinLevel == nullptr || ++g_ticks % kEvery != 0)
    {
        return;
    }
    Pass(cameraManagerOne);
    Summarise();
}

void SetEnabled(bool on) noexcept
{
    g_enabled = on;
    platform::Log(L"npc detail {}", on ? L"on" : L"off (NPCs near a further player get only what player 1's camera gives them)");
}

std::wstring Reading() noexcept
try
{
    return std::format(L"{}; {} raised now; last pass the further players' views needed {}, player 1's camera already kept {} "
                       L"of them; so far {} raised, {} released out of reach, {} released to player 1's camera, {} gone, {} "
                       L"taken over by the game",
                       g_enabled ? L"on" : L"off", g_raisedCount, g_needed, g_coveredNow, g_raisedTotal, g_outOfReach,
                       g_covered, g_gone, g_takenBack);
}
catch (...)
{
    return L"(the reading could not be formatted)";
}

}  // namespace lss::world::npc_detail
