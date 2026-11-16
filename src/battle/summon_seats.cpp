#include "battle/summon_seats.h"

#include "battle/command_menu.h"
#include "engine_facts.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/scopes.h"
#include "players/seats.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lss::battle::summon_seats {

namespace {

using ReaderFn = char (*)(void* controller);

platform::EntryRedirect g_shown, g_onField;

// Game thread: each seat's last answer per reader, for the log (0 not yet asked, 1 no, 2 yes, 3 player 1's not asked).
using Said = std::array<int, players::kSeatsBuilt + 1>;
Said g_shownSaid{};
Said g_onFieldSaid{};

/// The seat whose command controller `controller` is, or 0 (player 1's, or none of the mod's).
int SeatOf(const void* controller) noexcept
{
    for (int seat = 1; controller != nullptr && seat <= players::kSeatsBuilt; ++seat)
    {
        if (command_menu::Controller(seat) == controller)
        {
            return seat;
        }
    }
    return 0;
}

/// The reader's answer for `controller`: a seat's asks with player 1's controller, the holder of the party gauge's state.
char Answer(const platform::EntryRedirect& redirect, void* controller, Said& said, const wchar_t* what) noexcept
{
    const auto original = reinterpret_cast<ReaderFn>(redirect.Original());
    const int  seat     = SeatOf(controller);
    if (seat == 0)
    {
        return original(controller);
    }
    // Inside the seat's own UI slot scope the slot holds the seat's controller: player 1's is the one it held before.
    void* const playerOne = players::scopes::PlayerOneCommandController();
    char        answer    = 0;
    const bool  asked     = playerOne != nullptr && platform::CallGuarded(original, answer, playerOne);
    int&        last      = said[static_cast<std::size_t>(seat)];
    const int   now       = !asked ? 3 : answer != 0 ? 2 : 1;
    if (last != now)
    {
        last = now;
        platform::LogDebug(L"summon seats: player {}'s menu asks whether {}: {}", seat + 1, what,
                           !asked ? L"no (player 1's controller could not be asked)" : answer != 0 ? L"yes (player 1's answer)"
                                                                                       : L"no (player 1's answer)");
    }
    return asked ? answer : 0;
}

char GaugeShown(void* controller) noexcept
{
    return Answer(g_shown, controller, g_shownSaid, L"the summon gauge shows");
}

char OnField(void* controller) noexcept
{
    return Answer(g_onField, controller, g_onFieldSaid, L"a summon is on the field");
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto shown   = platform::FindFunction(image, facts::kSummonGaugeShown);
    const auto onField = platform::FindFunction(image, facts::kSummonOnField);
    const auto first   = std::span(facts::kSummonGaugeReaderEntry.data(), facts::kSummonGaugeReaderFirstBytes);
    if (!shown || !onField || !g_shown.Install(*shown, first, reinterpret_cast<std::uintptr_t>(&GaugeShown)) ||
        !g_onField.Install(*onField, first, reinterpret_cast<std::uintptr_t>(&OnField)))
    {
        g_shown.Restore();
        g_onField.Restore();
        platform::LogWarning(L"summon seats: an engine check failed; only player 1 can summon");
        return false;
    }
    platform::Log(L"summon seats: installed; players 2 and 3's menus offer Summon with the party's gauge, as player 1's");
    return true;
}

void Remove() noexcept
{
    g_onField.Restore();
    g_shown.Restore();
}

}  // namespace lss::battle::summon_seats
