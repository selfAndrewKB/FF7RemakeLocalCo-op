#include "players/party_member.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"

#include <cstdint>

namespace lss::players::party_member {

namespace {

using GetPlayerActorFn = void* (*)(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate);

constexpr std::int32_t  kRows            = 3;      // the deployed formation's rows
constexpr std::uint32_t kControllerLeader = 0x6F8;  // AEndPlayerController::LeaderCharacter
constexpr std::uint32_t kCharacterPlayerSlot = 0xCE8;  // AEndCharacter: the PLAYER controller slot

GetPlayerActorFn g_getPlayerActor = nullptr;
void**           g_partyManager   = nullptr;

template <class T>
bool Read(const void* address, T& out) noexcept
{
    return address != nullptr && platform::CopyGuarded(&out, address, sizeof(T));
}

void* ReadPointer(const void* object, std::uint32_t offset) noexcept
{
    void* value = nullptr;
    return object != nullptr && Read(static_cast<const std::uint8_t*>(object) + offset, value) ? value : nullptr;
}

/// The formation as the game keeps it, plus player 1's character.
struct Party
{
    std::uint8_t types[kRows]{kTypeNone, kTypeNone, kTypeNone};
    std::uint8_t leaderType = kTypeNone;
    void*        leaderOne  = nullptr;  // player 1's controller's LeaderCharacter
    void*        pawnOne    = nullptr;
    std::uint8_t* manager   = nullptr;
};

bool ReadParty(void* playerOneController, Party& out) noexcept
{
    if (!Read(g_partyManager, out.manager) || out.manager == nullptr || !Read(out.manager + facts::kPartySlotTypes, out.types) ||
        !Read(out.manager + facts::kPartyLeaderType, out.leaderType))
    {
        return false;
    }
    out.leaderOne = ReadPointer(playerOneController, kControllerLeader);
    out.pawnOne   = ReadPointer(playerOneController, facts::kControllerPawn);
    return true;
}

void* ActorOfType(const Party& party, std::uint8_t type) noexcept
{
    void* actor = nullptr;
    return type < kTypeNone && platform::CallGuarded(g_getPlayerActor, actor, static_cast<void*>(party.manager), std::uint8_t{0}, type, false)
               ? actor
               : nullptr;
}

/// Player 1's own: the game's leader row, or the character its controller holds.
bool PlayerOnes(const Party& party, std::uint8_t type, void* actor) noexcept
{
    return type == party.leaderType || (actor != nullptr && (actor == party.leaderOne || actor == party.pawnOne));
}

/// The ordinal of player 1's leader row (0 if its type is not in the formation).
std::int32_t Anchor(const Party& party) noexcept
{
    for (std::int32_t i = 0; i < kRows; ++i)
    {
        if (party.types[i] == party.leaderType)
        {
            return i;
        }
    }
    return 0;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto find  = platform::FindFunction(image, facts::kGetPlayerActor);
    const auto party = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    if (!find || !party)
    {
        return false;
    }
    g_getPlayerActor = reinterpret_cast<GetPlayerActorFn>(*find);
    g_partyManager   = reinterpret_cast<void**>(*party);
    return true;
}

Choice Choose(int seat, void* playerOneController, void* taker, bool log) noexcept
{
    Choice choice;
    Party  party;
    if (seat < 1 || g_getPlayerActor == nullptr || !ReadParty(playerOneController, party))
    {
        choice.wait = Wait::Unread;
        return choice;
    }
    // The rows after player 1's, in the formation's order, wrapping; the seat-th free one is this seat's.
    const std::int32_t anchor = Anchor(party);
    std::int32_t       free   = 0;
    for (std::int32_t step = 1; step < kRows; ++step)
    {
        const std::int32_t ordinal = (anchor + step) % kRows;
        const std::uint8_t type    = party.types[ordinal];
        if (type >= kTypeNone)
        {
            continue;
        }
        void* actor = ActorOfType(party, type);
        if (PlayerOnes(party, type, actor))
        {
            continue;
        }
        if (++free != seat)
        {
            continue;
        }
        choice.type    = type;
        choice.ordinal = ordinal;
        if (actor == nullptr)
        {
            choice.wait = Wait::NotLoaded;
            break;
        }
        void* holder = ReadPointer(actor, kCharacterPlayerSlot);
        if (holder != nullptr && holder != taker)
        {
            choice.wait = Wait::Held;
            break;
        }
        choice.member = actor;
        choice.wait   = Wait::None;
        break;
    }
    if (log)
    {
        platform::LogDebug(L"party member: formation types {} {} {}, player 1's leader type {} at ordinal {}; seat {}'s row: ordinal {} type {}: {}",
                           party.types[0], party.types[1], party.types[2], party.leaderType, anchor, seat, choice.ordinal, choice.type,
                           Describe(choice.wait));
    }
    return choice;
}

const wchar_t* WhyNotPlayable(int seat, void* member, std::uint8_t type, void* playerOneController) noexcept
{
    Party party;
    if (member == nullptr || !ReadParty(playerOneController, party))
    {
        return nullptr;  // unreadable this tick: nothing is decided on it
    }
    if (type == party.leaderType || member == party.leaderOne)
    {
        return L"now player 1's leader";
    }
    bool deployed = false;
    for (const std::uint8_t row : party.types)
    {
        deployed = deployed || row == type;
    }
    if (!deployed)
    {
        return L"no longer in the deployed formation";
    }
    const Choice slot = Choose(seat, playerOneController, nullptr, false);
    if (slot.wait != Wait::Unread && slot.type != type)
    {
        return L"no longer in this seat's row of the party order";
    }
    return nullptr;
}

const wchar_t* Describe(Wait wait) noexcept
{
    switch (wait)
    {
    case Wait::None:
        return L"free to take";
    case Wait::NoSlot:
        return L"no row beyond player 1's (a solo party)";
    case Wait::NotLoaded:
        return L"its character is not loaded";
    case Wait::Held:
        return L"another controller holds it";
    case Wait::Unread:
        return L"the party could not be read";
    }
    return L"?";
}

}  // namespace lss::players::party_member
