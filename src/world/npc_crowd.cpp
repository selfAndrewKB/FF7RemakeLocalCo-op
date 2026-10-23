#include "world/npc_crowd.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/seats.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>

namespace lss::world::npc_crowd {

namespace {

using platform::Read;
using platform::ReadPointer;
using players::kSeatsBuilt;
using players::SeatBuilt;
using players::SeatName;

using ScoreFn = std::int32_t (*)(void* evaluator, void* record);

constexpr auto kSummaryEvery = std::chrono::seconds(60);

platform::VTableEntry g_slot;
ScoreFn               g_score   = nullptr;
bool                  g_enabled = true;

/// One seat: its character while its player is in (set by TickSeat), and its scores higher than every other's.
struct Seat
{
    void*         member = nullptr;
    std::uint64_t higher = 0;
};
std::array<Seat, players::scopes::kMaxPlayers> g_seats;  // by seat; [0] unused

std::uint64_t g_calls = 0, g_eventCentre = 0, g_writeFailed = 0;
std::wstring  g_summary = L"no summary yet";
std::chrono::steady_clock::time_point g_lastSummary = std::chrono::steady_clock::now();

bool AnyMember() noexcept
{
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        if (g_seats[seat].member != nullptr)
        {
            return true;
        }
    }
    return false;
}

bool MemberLocation(void* member, float (&out)[3]) noexcept
{
    void* root = ReadPointer(member, facts::kActorRootComponent);
    return root != nullptr && Read(static_cast<const std::uint8_t*>(root) + facts::kComponentWorldLocation, out);
}

enum class From
{
    Unread,     // the character's place could not be read: the game's score stands for this seat
    Scored,     // scored from the seat's character, the centre put back
    NotPutBack, // scored, but the centre could not be put back: no further seat may write it this call
};

/// The game's score with the seat's character as the centre for one call, the fields exactly as before after it.
From ScoreFrom(void* evaluator, void* record, std::uint8_t* manager, const std::uint8_t (&centre)[facts::kCharaPopCentreBytes],
               void* member, std::int32_t& out) noexcept
{
    float location[3]{};
    if (!MemberLocation(member, location))
    {
        return From::Unread;
    }
    std::uint8_t asSeat[facts::kCharaPopCentreBytes]{};
    std::copy(std::begin(centre), std::end(centre), std::begin(asSeat));
    asSeat[0] = 1;
    std::copy(reinterpret_cast<const std::uint8_t*>(location), reinterpret_cast<const std::uint8_t*>(location) + sizeof(location),
              asSeat + 4);
    if (!platform::WriteGuarded(manager + facts::kCharaPopCentre, asSeat, sizeof(asSeat)))
    {
        ++g_writeFailed;
        return From::Unread;
    }
    out = g_score(evaluator, record);
    if (!platform::WriteGuarded(manager + facts::kCharaPopCentre, centre, sizeof(centre)))
    {
        ++g_writeFailed;
        return From::NotPutBack;
    }
    return From::Scored;
}

std::int32_t Score(void* evaluator, void* record) noexcept
{
    auto* const        self   = static_cast<std::uint8_t*>(evaluator);
    const std::int32_t before = [&] {
        std::int32_t value = 0;
        Read(self + facts::kCharaPopEvaluatorScore, value);
        return value;
    }();
    const std::int32_t one = g_score(evaluator, record);
    if (!g_enabled || !AnyMember())
    {
        return one;
    }
    ++g_calls;
    auto*        manager = static_cast<std::uint8_t*>(ReadPointer(record, facts::kCharaPopRecordManager));
    std::uint8_t centre[facts::kCharaPopCentreBytes]{};
    if (manager == nullptr || !Read(manager + facts::kCharaPopCentre, centre))
    {
        return one;
    }
    if (centre[0] != 0)
    {
        ++g_eventCentre;  // an event's centre: the game's single score
        return one;
    }
    // The highest score among the game's and every seat's: the nearest player's.
    std::int32_t best     = one;
    int          bestSeat = 0;
    bool         scored   = false;  // some seat's score ran
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        if (g_seats[seat].member == nullptr)
        {
            continue;
        }
        std::int32_t score = 0;
        const From   from  = ScoreFrom(evaluator, record, manager, centre, g_seats[seat].member, score);
        if (from == From::Unread)
        {
            continue;
        }
        scored = true;
        if (score > best)
        {
            best     = score;
            bestSeat = seat;
        }
        if (from == From::NotPutBack)
        {
            break;  // the centre is this seat's until the game writes it again: no further seat writes over it
        }
    }
    if (!scored)
    {
        return one;  // the game's one call, its side effect as it left it
    }
    if (bestSeat != 0)
    {
        ++g_seats[bestSeat].higher;
    }
    // The evaluator keeps its last non-zero score, as one game call would have left it.
    const std::int32_t kept = best != 0 ? best : before;
    platform::WriteGuarded(self + facts::kCharaPopEvaluatorScore, &kept, sizeof(kept));
    return best;
}

void Summarise() noexcept
try
{
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastSummary < kSummaryEvery)
    {
        return;
    }
    g_lastSummary = now;
    if (g_calls == 0 && g_eventCentre == 0)
    {
        return;
    }
    std::wstring higher;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        const std::uint64_t n = g_seats[seat].higher;
        higher += std::format(L"{} higher from {} ({:.0f} %), ", n, SeatName(seat), g_calls ? 100.0 * n / g_calls : 0.0);
    }
    g_summary = std::format(L"last minute: {} NPC scores, {}{} kept single for an event's centre, {} centre write(s) failed",
                            g_calls, higher, g_eventCentre, g_writeFailed);
    platform::LogDebug(L"npc crowd: {}", g_summary);
    g_calls = g_eventCentre = g_writeFailed = 0;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        g_seats[seat].higher = 0;  // after the summary is formed: every counter starts the minute together
    }
}
catch (...)
{
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (!platform::ReplaceSlot(image, facts::kCharaPopDistanceScoreSlot, reinterpret_cast<std::uintptr_t>(&Score), g_slot))
    {
        platform::LogWarning(L"npc crowd: the score's slot differs; the crowd follows player 1 only");
        return false;
    }
    g_score = reinterpret_cast<ScoreFn>(g_slot.Original());
    control::Register({.name  = L"npc-crowd",
                       .shape = control::Shape::OnOff,
                       .help  = L"each NPC is scored from the nearest player, so the crowd is around every player (world/npc_crowd.h; on)",
                       .set   = [](const control::Value& value) {
                           SetEnabled(std::get<bool>(value));
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled}; }});
    control::Register({.name = L"world.npc-crowd", .shape = control::Shape::Text,
                       .help = L"the last minute's NPC scores and how many were higher from each further player",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"npc crowd: installed; while a further player is in, each NPC is scored from the nearest player");
    return true;
}

void TickSeat(int seat, void* seatMember) noexcept
{
    if (g_score == nullptr || !SeatBuilt(seat))
    {
        return;
    }
    g_seats[seat].member = seatMember;
}

void Tick() noexcept
{
    if (g_score == nullptr)
    {
        return;
    }
    Summarise();
}

void SetEnabled(bool on) noexcept
{
    g_enabled = on;
    platform::Log(L"npc crowd {}", on ? L"on" : L"off (the crowd follows player 1 only)");
}

std::wstring Reading() noexcept
{
    return std::wstring(g_enabled ? L"on; " : L"off; ") + g_summary;
}

}  // namespace lss::world::npc_crowd
