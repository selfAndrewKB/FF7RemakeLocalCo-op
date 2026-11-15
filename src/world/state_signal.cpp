#include "world/state_signal.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/name_text.h"
#include "platform/verified_code.h"
#include "players/game_state.h"
#include "world/data_rows.h"

#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace lss::world::state_signal {

namespace {

using SendFn = void (*)(std::uint64_t trigger, std::uint64_t sender, float delay, std::uint8_t flags);

SendFn g_send = nullptr;

/// A trigger's row as a send would apply it: found, its scope, the objects it sets whatever their state, its save step.
/// `readable` false when any of the fields a refusal rests on could not be read: such a row is refused.
struct RowReading
{
    std::uint64_t             name       = 0;
    const std::uint8_t*       row        = nullptr;
    bool                      readable   = false;
    std::uint8_t              scope      = 0;
    std::uint8_t              saveStep   = 0;
    std::int32_t              directSets = 0;
    std::vector<std::int64_t> objects, states, sendObjects;
};

std::wstring List(const std::vector<std::int64_t>& names)
{
    std::wstring out;
    for (const std::int64_t name : names)
    {
        out += (out.empty() ? L"" : L", ") + platform::NameOrBits(name);
    }
    return out.empty() ? L"none" : out;
}

RowReading ReadRow(const std::wstring& text)
{
    RowReading r;
    r.name = platform::FindName(text.c_str());
    r.row  = data_rows::Row(data_rows::Table::Trigger, static_cast<std::int64_t>(r.name));
    if (r.row == nullptr)
    {
        return r;
    }
    r.readable = platform::Read(r.row + facts::kTriggerRowScope, r.scope) && platform::Read(r.row + facts::kTriggerRowSaveStep, r.saveStep) &&
                 platform::Read(r.row + facts::kTriggerRowObjects + 8, r.directSets);
    r.objects     = data_rows::Names(r.row + facts::kTriggerRowObjects, facts::kTriggerRowDirectSetsMax);
    r.states      = data_rows::Names(r.row + facts::kTriggerRowStates, facts::kTriggerRowDirectSetsMax);
    r.sendObjects = data_rows::Names(r.row + facts::kTriggerRowSendObjects, 64);
    return r;
}

/// Why a send of this row is refused, empty when it may go.
std::wstring Refusal(const RowReading& r)
{
    if (r.row == nullptr)
    {
        return L"no trigger of that name in the level's tables";
    }
    if (!r.readable)
    {
        return L"its row could not be read";
    }
    if (r.saveStep != 0)
    {
        return std::format(L"its row writes a save record when sent (save step {})", static_cast<int>(r.saveStep));
    }
    if (r.directSets != 0)
    {
        return L"its row sets objects directly, whatever their state";
    }
    return {};
}

std::wstring Describe(const std::wstring& text, const RowReading& r)
{
    if (r.row == nullptr)
    {
        return std::format(L"{}: no trigger of that name in the level's tables", text);
    }
    const std::wstring refused = Refusal(r);
    std::wstring       sets;
    for (std::size_t i = 0; i < r.objects.size(); ++i)
    {
        const std::wstring to = i < r.states.size() ? platform::NameOrBits(r.states[i]) : std::wstring(L"?");
        sets += std::format(L"{}{} -> {}", sets.empty() ? L"" : L", ", platform::NameOrBits(r.objects[i]), to);
    }
    return std::format(L"{}: scope {} (0 the sender's own, 3 the listed objects, else every active state that waits for it); "
                       L"objects set directly: {}; listed objects: {}; save step {}; {}",
                       text, static_cast<int>(r.scope), sets.empty() ? L"none" : sets, List(r.sendObjects), static_cast<int>(r.saveStep),
                       refused.empty() ? std::wstring(L"state.signal may send it") : L"state.signal refuses it: " + refused);
}

control::Outcome Signal(const std::wstring& text)
{
    const RowReading   r       = ReadRow(text);
    const std::wstring refused = Refusal(r);
    if (!refused.empty())
    {
        return {false, std::format(L"{}: not sent: {}", text, refused)};
    }
    const players::game_state::Snapshot& state = players::game_state::Current();
    if (!state.valid || !players::game_state::SteadyField(state))
    {
        return {false, std::format(L"{}: not sent: the field is not steady (field step {})", text, state.step)};
    }
    if (!platform::CallGuarded(g_send, r.name, std::uint64_t{0}, -1.0f, std::uint8_t{0}))
    {
        platform::LogWarning(L"state signal: the send of {} faulted", text);
        return {false, std::format(L"{}: the send faulted", text)};
    }
    platform::Log(L"state signal: {} sent from the console, queued for the state manager's next tick", text);
    return {true, std::format(L"{}: queued for the state manager's next tick", text)};
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto send = platform::FindFunction(image, facts::kStateSendSignal);
    if (!send || !data_rows::Installed(data_rows::Table::Trigger))
    {
        platform::Log(L"state signal: the game's trigger sender is not as the research read it; no trigger is sent from the console");
        return false;
    }
    g_send = reinterpret_cast<SendFn>(*send);
    control::Register({.name  = L"state.trigger",
                       .shape = control::Shape::Text,
                       .help  = L"what a level state trigger's row would do if sent: its scope, the objects it sets directly, the "
                                L"objects it lists, its save step, and whether state.signal sends it",
                       .set   = [](const control::Value& v) -> control::Outcome {
                           try
                           {
                               const std::wstring& text = std::get<std::wstring>(v);
                               return {true, Describe(text, ReadRow(text))};
                           }
                           catch (...)
                           {
                               return {false, L"the row could not be read"};
                           }
                       }});
    control::Register({.name  = L"state.signal",
                       .shape = control::Shape::Text,
                       .help  = L"sends a level state trigger by name through the game's own sender, as a walk-up's exit sends a "
                                L"spot's end trigger: on the steady field, queued for the state manager's next tick; a trigger whose row "
                                L"sets objects directly or writes a save record is refused (state.trigger says why); a sent trigger is "
                                L"marked fired in the save, as the game's own sends are",
                       .set   = [](const control::Value& v) -> control::Outcome {
                           try
                           {
                               return Signal(std::get<std::wstring>(v));
                           }
                           catch (...)
                           {
                               return {false, L"the trigger could not be read"};
                           }
                       }});
    platform::Log(L"state signal: installed; state.trigger reads a level trigger's row, state.signal sends one (the repair of a "
                  L"spot's level state left on)");
    return true;
}

}  // namespace lss::world::state_signal
