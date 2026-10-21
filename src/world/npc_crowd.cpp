#include "world/npc_crowd.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>

namespace lss::world::npc_crowd {

namespace {

using platform::Read;
using platform::ReadPointer;

using ScoreFn = std::int32_t (*)(void* evaluator, void* record);

constexpr auto kSummaryEvery = std::chrono::seconds(60);

platform::VTableEntry g_slot;
ScoreFn               g_score   = nullptr;
bool                  g_enabled = true;
void*                 g_member  = nullptr;  // player 2's character, set by Tick

std::uint64_t g_calls = 0, g_twoHigher = 0, g_eventCentre = 0, g_writeFailed = 0;
std::wstring  g_summary = L"no summary yet";
std::chrono::steady_clock::time_point g_lastSummary = std::chrono::steady_clock::now();

bool MemberLocation(float (&out)[3]) noexcept
{
    void* root = ReadPointer(g_member, facts::kActorRootComponent);
    return root != nullptr && Read(static_cast<const std::uint8_t*>(root) + facts::kComponentWorldLocation, out);
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
    if (!g_enabled || g_member == nullptr)
    {
        return one;
    }
    ++g_calls;
    auto*         manager = static_cast<std::uint8_t*>(ReadPointer(record, facts::kCharaPopRecordManager));
    std::uint8_t  centre[facts::kCharaPopCentreBytes]{};
    float         location[3]{};
    if (manager == nullptr || !Read(manager + facts::kCharaPopCentre, centre) || !MemberLocation(location))
    {
        return one;
    }
    if (centre[0] != 0)
    {
        ++g_eventCentre;  // an event's centre: the game's single score
        return one;
    }
    // Player 2's character as the centre for one call, the fields exactly as before after it.
    std::uint8_t asTwo[facts::kCharaPopCentreBytes]{};
    std::copy(std::begin(centre), std::end(centre), std::begin(asTwo));
    asTwo[0] = 1;
    std::copy(reinterpret_cast<const std::uint8_t*>(location), reinterpret_cast<const std::uint8_t*>(location) + sizeof(location),
              asTwo + 4);
    if (!platform::WriteGuarded(manager + facts::kCharaPopCentre, asTwo, sizeof(asTwo)))
    {
        ++g_writeFailed;
        return one;
    }
    const std::int32_t two = g_score(evaluator, record);
    platform::WriteGuarded(manager + facts::kCharaPopCentre, centre, sizeof(centre));
    const std::int32_t best = std::max(one, two);
    g_twoHigher += two > one ? 1 : 0;
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
    g_summary = std::format(L"last minute: {} NPC scores, {} higher from player 2 ({:.0f} %), {} kept single for an event's "
                            L"centre, {} centre write(s) failed",
                            g_calls, g_twoHigher, g_calls ? 100.0 * g_twoHigher / g_calls : 0.0, g_eventCentre, g_writeFailed);
    platform::LogDebug(L"npc crowd: {}", g_summary);
    g_calls = g_twoHigher = g_eventCentre = g_writeFailed = 0;
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
                       .help  = L"each NPC is scored from the nearer player, so the crowd is around both (world/npc_crowd.h; on)",
                       .set   = [](const control::Value& value) {
                           SetEnabled(std::get<bool>(value));
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled}; }});
    control::Register({.name = L"world.npc-crowd", .shape = control::Shape::Text,
                       .help = L"the last minute's NPC scores and how many were higher from player 2",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"npc crowd: installed; while player 2 is in, each NPC is scored from the nearer player");
    return true;
}

void Tick(void* memberTwo) noexcept
{
    if (g_score == nullptr)
    {
        return;
    }
    g_member = memberTwo;
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
