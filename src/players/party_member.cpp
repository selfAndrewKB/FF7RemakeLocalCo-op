#include "players/party_member.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/field_hold.h"
#include "players/lead_gate.h"

#include <cstdint>
#include <format>
#include <string>

namespace lss::players::party_member {

namespace {

using platform::Read;
using platform::ReadPointer;

using GetPlayerActorFn = void* (*)(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate);
using GetPartyLeaderFn = void* (*)(void* partyManager, std::uint8_t variant);
using ReconcileFn      = std::int64_t (*)(void* character800);
using SetActorTypeFn   = void (*)(void* character800, std::uint8_t type);

constexpr std::int32_t  kRows             = 3;      // the deployed formation's rows
constexpr std::uint32_t kControllerLeader = 0x6F8;  // AEndPlayerController::LeaderCharacter

GetPlayerActorFn g_getPlayerActor = nullptr;
GetPartyLeaderFn g_getPartyLeader = nullptr;
ReconcileFn      g_reconcile      = nullptr;
SetActorTypeFn   g_setActorType   = nullptr;
void**           g_partyManager   = nullptr;
std::uint8_t**   g_data           = nullptr;
std::uint8_t**   g_root           = nullptr;

/// The formation as the game keeps it, the field leader (the anchor), the statuses, and player 1's character.
struct Party
{
    std::uint8_t  types[kRows]{facts::kPlayerTypeNone, facts::kPlayerTypeNone, facts::kPlayerTypeNone};
    std::uint8_t  fieldLeader = facts::kPlayerTypeNone;
    std::uint8_t  statuses[8]{};
    void*         leaderOne = nullptr;  // player 1's controller's LeaderCharacter
    void*         pawnOne   = nullptr;
    std::uint8_t* manager   = nullptr;
};

bool ReadParty(void* playerOneController, Party& out) noexcept
{
    std::uint8_t* data = nullptr;
    if (!Read(g_partyManager, out.manager) || out.manager == nullptr || !Read(out.manager + facts::kPartySlotTypes, out.types) ||
        !Read(g_data, data) || data == nullptr || !Read(data + facts::kDataFieldLeaderType, out.fieldLeader) ||
        !Read(data + facts::kDataPlayerStatus, out.statuses))
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
    return type < facts::kPlayerTypeNone &&
                   platform::CallGuarded(g_getPlayerActor, actor, static_cast<void*>(party.manager), std::uint8_t{0}, type, false)
               ? actor
               : nullptr;
}

/// The ordinal of the field leader's row (0 if its type is not in the formation).
std::int32_t Anchor(const Party& party) noexcept
{
    for (std::int32_t i = 0; i < kRows; ++i)
    {
        if (party.types[i] == party.fieldLeader)
        {
            return i;
        }
    }
    return 0;
}

/// The field take tests of RE-slot-member.md §2.5 with the hold rule of field_hold.h, as a wait reason; None when
/// the member is free.
Wait FieldWait(void* member) noexcept
{
    const std::uint8_t mode = ActorMode(member);
    if (mode == facts::kActorModeFieldCinema || mode == facts::kActorModeCinema)
    {
        return Wait::Cinema;
    }
    if (mode == facts::kActorModeBattle)
    {
        return Wait::None;  // the battle AI never runs a player-held member; the cycle's own tests are player 1's
    }
    if (EventLocked())
    {
        return Wait::EventLock;
    }
    // The game's own leader handed to its AI: the game is in a scene, a field action or the control lock, and it
    // hands every player's member the same way (the funnel, or its backstop in the session). A seat that landed
    // inside such a hand played through it (seen live 2026-10-21: Tifa taken mid-animation under lock 0x1).
    if (!LeaderWithPlayer())
    {
        return Wait::LeaderHanded;
    }
    const std::uint8_t* ai    = static_cast<const std::uint8_t*>(ReadPointer(member, facts::kCharacterFieldAi));
    std::uint8_t        layer = field_hold::kNoLayer, kind = 0;
    if (ai == nullptr || !Read(ai + facts::kFieldAiActiveLayer, layer) || !Read(ai + facts::kFieldAiActiveKind, kind))
    {
        return Wait::NoFieldAi;
    }
    switch (field_hold::Of(layer, kind))
    {
    case field_hold::Hold::Free:
        return Wait::None;
    case field_hold::Hold::Animation:
        return Wait::Animation;
    case field_hold::Hold::Lead:
        return lead_gate::Guide(member, ai) ? Wait::Guide : Wait::None;
    case field_hold::Hold::NoState:
        return Wait::NoFieldAi;
    default:
        return Wait::AiHeld;
    }
}

}  // namespace

Wait HoldWait(void* member) noexcept
{
    return member == nullptr ? Wait::None : FieldWait(member);
}

std::uint8_t ActorMode(const void* member) noexcept
{
    std::uint8_t mode = 0;
    Read(static_cast<const std::uint8_t*>(ReadPointer(member, facts::kCharacterActorMode)) + facts::kActorModeMode, mode);
    return mode;
}

bool Install(const platform::GameImage& image) noexcept
{
    const auto find      = platform::FindFunction(image, facts::kGetPlayerActor);
    const auto leader    = platform::FindFunction(image, facts::kGetPartyLeader);
    const auto reconcile = platform::FindFunction(image, facts::kReconcileController);
    const auto setType   = platform::FindFunction(image, facts::kSetActorType);
    const auto party     = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    const auto data      = image.Rebase(facts::kDataSingletonGlobal, sizeof(void*));
    const auto root      = image.Rebase(facts::kEndGameRootGlobal, sizeof(void*));
    if (!find || !leader || !reconcile || !setType || !party || !data || !root)
    {
        return false;
    }
    g_getPlayerActor = reinterpret_cast<GetPlayerActorFn>(*find);
    g_getPartyLeader = reinterpret_cast<GetPartyLeaderFn>(*leader);
    g_reconcile      = reinterpret_cast<ReconcileFn>(*reconcile);
    g_setActorType   = reinterpret_cast<SetActorTypeFn>(*setType);
    g_partyManager   = reinterpret_cast<void**>(*party);
    g_data           = reinterpret_cast<std::uint8_t**>(*data);
    g_root           = reinterpret_cast<std::uint8_t**>(*root);
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
    choice.formation = static_cast<std::uint32_t>(party.types[0]) | static_cast<std::uint32_t>(party.types[1]) << 8 |
                       static_cast<std::uint32_t>(party.types[2]) << 16 | static_cast<std::uint32_t>(party.fieldLeader) << 24;
    // The rows after the field leader's, in the formation's order, wrapping; the seat-th one not player 1's and not
    // a battle-support member is this seat's. Temporary states are waited on, never stepped over.
    const std::int32_t anchor  = Anchor(party);
    std::int32_t       free    = 0;
    bool               support = false;  // a row skipped for its battle-support status (the wait's reason with no row)
    for (std::int32_t step = 1; step < kRows; ++step)
    {
        const std::int32_t ordinal = (anchor + step) % kRows;
        const std::uint8_t type    = party.types[ordinal];
        if (type >= facts::kPlayerTypeNone)
        {
            continue;
        }
        if (party.statuses[type] == facts::kStatusBattleSupport)
        {
            support = true;
            continue;
        }
        void* actor = ActorOfType(party, type);
        if (type == party.fieldLeader || (actor != nullptr && (actor == party.leaderOne || actor == party.pawnOne)))
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
        void* holder = ReadPointer(actor, facts::kCharacterPlayerSlot);
        if (holder != nullptr && holder != taker)
        {
            choice.wait = Wait::Held;
            break;
        }
        choice.wait = FieldWait(actor);
        if (choice.wait == Wait::None)
        {
            choice.member = actor;
        }
        break;
    }
    if (choice.ordinal < 0 && support)
    {
        choice.wait = Wait::Support;  // the user's Barret case (2026-10-22 11:10): types 0 1 9 read as a solo party
    }
    if (log)
    {
        platform::LogDebug(L"party member: formation types {} {} {}, field leader type {} at ordinal {}; seat {}'s row: ordinal {} type {}: {}",
                           party.types[0], party.types[1], party.types[2], party.fieldLeader, anchor, seat, choice.ordinal, choice.type,
                           Describe(choice.wait));
    }
    return choice;
}

void* RowActor(int seat, void* playerOneController) noexcept
{
    Party party;
    if (!ReadParty(playerOneController, party))
    {
        return nullptr;
    }
    const Choice choice = Choose(seat, playerOneController, nullptr, false);
    return choice.type < facts::kPlayerTypeNone ? ActorOfType(party, choice.type) : nullptr;
}

std::wstring FormationReading(void* playerOneController) noexcept
try
{
    Party party;
    if (!ReadParty(playerOneController, party))
    {
        return L"unread";
    }
    std::wstring text = std::format(L"field leader type {}; ", party.fieldLeader);
    for (std::int32_t i = 0; i < kRows; ++i)
    {
        const std::uint8_t type = party.types[i];
        if (type >= facts::kPlayerTypeNone)
        {
            continue;
        }
        void* actor = ActorOfType(party, type);
        text += std::format(L"[row {} type {} status {} holder {:#x}: {}] ", i, type, party.statuses[type],
                            reinterpret_cast<std::uintptr_t>(ReadPointer(actor, facts::kCharacterPlayerSlot)), AiDetail(actor));
    }
    return text;
}
catch (...)
{
    return L"unread";
}

const wchar_t* WhyNotInSlot(int seat, void* member, std::uint8_t type, void* playerOneController) noexcept
{
    Party party;
    if (member == nullptr || !ReadParty(playerOneController, party))
    {
        return nullptr;  // unreadable this tick: nothing is decided on it
    }
    if (member == party.leaderOne || member == party.pawnOne)
    {
        return L"now player 1's";
    }
    if (!InFormation(type))
    {
        return L"no longer in the deployed formation";
    }
    const Choice slot = Choose(seat, playerOneController, nullptr, false);
    if (slot.wait != Wait::Unread && slot.wait != Wait::NoSlot && slot.wait != Wait::Support && slot.type != type)
    {
        return L"no longer in this seat's row of the party order";
    }
    return nullptr;
}

const wchar_t* WhyHeldByScript(void* member) noexcept
{
    switch (member == nullptr ? Wait::None : FieldWait(member))
    {
    case Wait::Cinema:
        return L"in a cutscene or event (the cinema actor mode)";
    case Wait::AiHeld:
        return L"held by a story script (a script layer of its field AI, or the unread ESCAPE_POSITION kind)";
    case Wait::Animation:
        return L"in a scripted animation (an ANIMATION or one-shot kind): its AI plays it";
    case Wait::Guide:
        return L"a guide lead (a level step waits on its arrival, or the story's marker rides on it): its AI walks it";
    default:
        return nullptr;  // an event lock or the game's leader handed is the game's own hand (party_events); the rest holds nothing
    }
}

bool InFormation(std::uint8_t type) noexcept
{
    std::uint8_t* data   = nullptr;
    std::uint8_t  status = 0;
    return type < 8 && Read(g_data, data) && data != nullptr && Read(data + facts::kDataPlayerStatus + type, status) &&
           (status == facts::kStatusParty || status == facts::kStatusBattleSupport);
}

std::uint32_t ControlLockMask() noexcept
{
    std::uint8_t* root = nullptr;
    std::uint8_t* sm   = nullptr;
    std::uint32_t lock = 0;
    return Read(g_root, root) && root != nullptr && Read(root + facts::kRootStateManager, sm) && sm != nullptr &&
                   Read(sm + facts::kStateManagerControlLock, lock)
               ? lock
               : 0;
}

bool EventLocked() noexcept
{
    return (ControlLockMask() & facts::kControlLockEvent) != 0;
}

bool LeaderWithPlayer() noexcept
{
    void* manager = nullptr;
    void* leader  = nullptr;
    return g_getPartyLeader != nullptr && Read(g_partyManager, manager) && manager != nullptr &&
           platform::CallGuarded(g_getPartyLeader, leader, manager, std::uint8_t{0}) && leader != nullptr &&
           ReadPointer(leader, facts::kCharacterPlayerSlot) != nullptr;
}

bool HandToAi(void* member) noexcept
{
    void*        none    = nullptr;
    std::int64_t ignored = 0;
    return member != nullptr &&
           platform::WriteGuarded(static_cast<std::uint8_t*>(member) + facts::kCharacterPlayerSlot, &none, sizeof(none)) &&
           platform::CallGuarded(g_reconcile, ignored, static_cast<void*>(static_cast<std::uint8_t*>(member) + facts::kCharacter800));
}

bool ReturnTo(void* member, void* controller) noexcept
{
    std::int64_t ignored = 0;
    return member != nullptr && controller != nullptr &&
           platform::WriteGuarded(static_cast<std::uint8_t*>(member) + facts::kCharacterPlayerSlot, &controller, sizeof(controller)) &&
           platform::CallGuarded(g_reconcile, ignored, static_cast<void*>(static_cast<std::uint8_t*>(member) + facts::kCharacter800));
}

bool HandedToAi(void* member) noexcept
{
    return member != nullptr && ReadPointer(member, facts::kCharacterPlayerSlot) == nullptr;
}

bool ReassertPlayerType(void* member) noexcept
{
    std::uint8_t type = 0xFF;
    Read(static_cast<const std::uint8_t*>(ReadPointer(member, facts::kCharacterActorMode)) + facts::kActorModeType, type);
    return member != nullptr &&
           (type == facts::kActorTypePlayer ||
            platform::CallGuarded(g_setActorType, static_cast<void*>(static_cast<std::uint8_t*>(member) + facts::kCharacter800),
                                  facts::kActorTypePlayer));
}

std::wstring AiDetail(void* member) noexcept
try
{
    const std::uint8_t* ai    = static_cast<const std::uint8_t*>(ReadPointer(member, facts::kCharacterFieldAi));
    std::uint8_t        layer = 0xFF, kind = 0xFF;
    std::wstring        kinds;
    if (ai != nullptr)
    {
        Read(ai + facts::kFieldAiActiveLayer, layer);
        Read(ai + facts::kFieldAiActiveKind, kind);
        for (std::uint32_t i = 0; i < facts::kFieldAiLayers; ++i)
        {
            std::uint8_t k = 0xFF;
            Read(ai + facts::kFieldAiLayerRecords + i * facts::kFieldAiLayerBytes, k);
            kinds += std::format(L"{}{}", i == 0 ? L"" : L" ", k);
        }
    }
    return std::format(L"mode {}, field AI {}: active layer {} kind {} {}, layer kinds [{}], lock {:#x}", ActorMode(member),
                       ai == nullptr ? L"none" : L"present", layer, kind, field_hold::KindName(kind), kinds, ControlLockMask());
}
catch (...)
{
    return L"unread";
}

const wchar_t* Describe(Wait wait) noexcept
{
    switch (wait)
    {
    case Wait::None:
        return L"free to take";
    case Wait::NoSlot:
        return L"no row beyond player 1's (a solo party)";
    case Wait::Support:
        return L"the rows beyond player 1's hold only battle-support members (guests the game fights with, not deployed members)";
    case Wait::NotLoaded:
        return L"its character is not loaded";
    case Wait::Held:
        return L"another controller holds it";
    case Wait::Cinema:
        return L"in a cutscene or event (the cinema actor mode)";
    case Wait::EventLock:
        return L"an event holds the field";
    case Wait::LeaderHanded:
        return L"the game's leader is with its AI (a scene, a field action, the control lock), or unread";
    case Wait::AiHeld:
        return L"a story script holds its field AI (a script layer, or the unread ESCAPE_POSITION kind)";
    case Wait::Animation:
        return L"its AI plays a scripted animation (an ANIMATION or one-shot kind)";
    case Wait::Guide:
        return L"its AI walks a guide lead (a level step waits on its arrival, or the story's marker rides on it)";
    case Wait::NoFieldAi:
        return L"its field AI has no state yet";
    case Wait::Unread:
        return L"the party could not be read";
    }
    return L"?";
}

}  // namespace lss::players::party_member
