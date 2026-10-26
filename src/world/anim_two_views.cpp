#include "world/anim_two_views.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/seats.h"
#include "world/actor_place.h"
#include "world/anim_choice.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>

namespace lss::world::anim_two_views {

namespace {

using platform::Read;
using platform::ReadPointer;
using players::kSeatsBuilt;
using players::SeatBuilt;
using players::SeatName;
using namespace anim_choice;

constexpr int kMaxPlayers = players::scopes::kMaxPlayers;

using JudgeFn  = void (*)(void* mesh, bool* forceOut, void** leadOut, bool cameraCut);
using UpdateFn = std::int64_t (*)(void* allocator, float dt, void* a3, void* a4);

platform::VTableEntry g_judgeSlot;
platform::VTableEntry g_judgeSwitchableSlot;
platform::VTableEntry g_updateSlot;
JudgeFn               g_judge  = nullptr;
UpdateFn              g_update = nullptr;

bool g_enabled  = true;
bool g_anyReady = false;  // some seat's snapshot belongs to this frame's Update

/// One seat: its camera and character (set by TickSeat, read at the pre-actor tick of the same frame), its snapshot
/// for this frame and the history the prediction keeps, and its counts.
struct Seat
{
    void*         camera     = nullptr;
    void*         member     = nullptr;
    void*         mesh       = nullptr;  // its character's mesh, for the log
    History       history;               // its previous camera, for the prediction
    Snapshot      snapshot;              // this frame's
    bool          ready      = false;    // the snapshot belongs to this frame's Update
    bool          cut        = false;
    int           judgedFrom = 0;        // which camera its character was judged from last: 0 none, 1 player 1's, seat + 1
    std::uint64_t judged     = 0;        // meshes judged from this seat's camera (the 10 s window)
};
std::array<Seat, kMaxPlayers> g_seats;  // by seat; [0] unused

// The 10 s summary.
using Clock = std::chrono::steady_clock;
Clock::time_point g_windowStart = Clock::now();
std::uint64_t     g_updates = 0, g_judged = 0, g_swapFailed = 0;
double            g_updateMicros = 0.0;
std::wstring      g_summary      = L"no summary yet";

/// The mesh owner's world location (the judge's own fallback measure point).
bool OwnerLocation(void* mesh, Vec& out) noexcept
{
    float at[3]{};
    if (!actor_place::ActorLocation(ReadPointer(mesh, facts::kComponentOwner), at))
    {
        return false;
    }
    out = {at[0], at[1], at[2]};
    return true;
}

/// The camera a further player's character was judged from, for the log (`from`: 1 player 1's, seat + 1).
void NoteJudged(void* mesh, int from) noexcept
{
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        if (mesh == g_seats[seat].mesh && mesh != nullptr)
        {
            g_seats[seat].judgedFrom = from;
        }
    }
}

void Judge(void* mesh, bool* forceOut, void** leadOut, bool cameraCut) noexcept
{
    if (!g_anyReady)
    {
        g_judge(mesh, forceOut, leadOut, cameraCut);
        return;
    }
    ++g_judged;
    auto*    allocator = static_cast<std::uint8_t*>(ReadPointer(mesh, facts::kMeshBudgetAllocator));
    Snapshot one;
    Vec      point;
    if (allocator == nullptr || !Read(allocator + facts::kAllocatorSnapshot, one) || !OwnerLocation(mesh, point))
    {
        NoteJudged(mesh, 1);
        g_judge(mesh, forceOut, leadOut, cameraCut);
        return;
    }
    // Player 1's view first, then each ready seat's in seat order: the best of them judges the mesh.
    std::array<View, kMaxPlayers> views{};
    std::array<int, kMaxPlayers>  seatOf{};
    int                           count = 0;
    views[count]    = Measure(point, one.location, one.forward);
    seatOf[count++] = 0;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        const Seat& s = g_seats[seat];
        if (s.ready)
        {
            views[count]    = Measure(point, s.snapshot.location, s.snapshot.forward);
            seatOf[count++] = seat;
        }
    }
    const int bestSeat = seatOf[static_cast<std::size_t>(Best(views.data(), count))];
    if (bestSeat == 0)
    {
        NoteJudged(mesh, 1);
        g_judge(mesh, forceOut, leadOut, cameraCut);
        return;
    }
    // The game's judge, once, reading the seat's snapshot; the block is player 1's again before the next mesh.
    Seat& best = g_seats[bestSeat];
    if (!platform::WriteGuarded(allocator + facts::kAllocatorSnapshot, &best.snapshot, sizeof(best.snapshot)))
    {
        ++g_swapFailed;
        g_judge(mesh, forceOut, leadOut, cameraCut);
        return;
    }
    ++best.judged;
    NoteJudged(mesh, bestSeat + 1);
    g_judge(mesh, forceOut, leadOut, cameraCut || best.cut);
    platform::WriteGuarded(allocator + facts::kAllocatorSnapshot, &one, sizeof(one));
}

/// Each seat's snapshot, read from its camera manager at the moment the game reads player 1's.
void MakeSnapshots(float dt) noexcept
{
    g_anyReady = false;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        Seat&         s       = g_seats[seat];
        const auto*   manager = static_cast<const std::uint8_t*>(s.camera);
        Vec           location;
        float         rotation[3]{};
        std::uint32_t flags = 0;
        s.ready             = false;
        if (!g_enabled || manager == nullptr || !Read(manager + facts::kCameraManagerPovLocation, location) ||
            !Read(manager + facts::kCameraManagerPovRotation, rotation) || !Read(manager + facts::kCameraManagerCutFlags, flags))
        {
            s.history.valid = false;
            continue;
        }
        s.cut      = (flags & facts::kCameraManagerCutBit) != 0;
        s.snapshot = Predict(s.history, location, ForwardOf(rotation[0], rotation[1]), s.cut, dt);
        s.ready    = true;
        g_anyReady = true;
    }
}

std::int64_t Update(void* allocator, float dt, void* a3, void* a4) noexcept
{
    MakeSnapshots(dt);
    const auto         start  = Clock::now();
    const std::int64_t result = g_update(allocator, dt, a3, a4);
    g_updateMicros += std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    ++g_updates;
    g_anyReady = false;  // the snapshots are used only inside this frame's loop
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        g_seats[seat].ready = false;
    }
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
    std::wstring shares, members;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        Seat& s = g_seats[seat];
        shares += std::format(L"{:.0f} from {}'s camera ({:.0f} %), ", static_cast<double>(s.judged) / g_updates, SeatName(seat),
                              g_judged ? 100.0 * s.judged / g_judged : 0.0);
        if (s.mesh != nullptr && s.judgedFrom != 0)
        {
            float distance = 0.0f, angle = 0.0f;
            Read(static_cast<const std::uint8_t*>(s.mesh) + facts::kMeshAnimDistance, distance);
            Read(static_cast<const std::uint8_t*>(s.mesh) + facts::kMeshAnimAngle, angle);
            members += std::format(L"; {}'s character judged from {}'s camera ({:.1f} m, {:.0f} deg, cloth mode {})",
                                   SeatName(seat), SeatName(s.judgedFrom - 1), distance / 100.0f, angle * 57.29578f,
                                   ClothMode(s.member));
        }
        else
        {
            members += std::format(L"; {}'s character not judged", SeatName(seat));
        }
    }
    g_summary = std::format(L"{:.0f} meshes judged per frame, {}judging {:.3f} ms per frame, {} snapshot write(s) failed{}",
                            static_cast<double>(g_judged) / g_updates, shares, g_updateMicros / g_updates / 1000.0,
                            g_swapFailed, members);
    platform::LogDebug(L"anim two views: {}", g_summary);
    g_updates = g_judged = g_swapFailed = 0;
    g_updateMicros = 0.0;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        g_seats[seat].judged = 0;  // with the others, after the summary is formed: every count starts the window together
    }
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
                       .help  = L"characters near a further player are animated as if that player's camera were the game's (world/anim_two_views.h; on)",
                       .set   = [](const control::Value& value) {
                           SetEnabled(std::get<bool>(value));
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled}; }});
    control::Register({.name = L"world.anim", .shape = control::Shape::Text,
                       .help = L"the last 10 s: meshes judged, the share judged from each further player's camera, the time, their characters",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"anim two views: installed; each character is judged from the camera that sees it best while a further "
                  L"player is in");
    return true;
}

void TickSeat(int seat, void* seatCameraManager, void* seatMember) noexcept
{
    if (g_judge == nullptr || !SeatBuilt(seat))
    {
        return;
    }
    Seat& s  = g_seats[seat];
    s.camera = seatCameraManager;
    if (seatMember != s.member)
    {
        s.member     = seatMember;
        s.judgedFrom = 0;
    }
    s.mesh = ReadPointer(seatMember, facts::kCharacterMesh);
}

void Tick() noexcept
{
    if (g_judge == nullptr)
    {
        return;
    }
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
