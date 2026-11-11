#include "players/party_member.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/ai_move_guard.h"
#include "players/field_hold.h"
#include "players/field_root.h"
#include "players/lead_gate.h"

#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace lss::players::party_member {

namespace {

using platform::Read;
using platform::ReadPointer;

using GetPlayerActorFn = void* (*)(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate);
using ReconcileFn      = std::int64_t (*)(void* character800);
using SetActorTypeFn   = void (*)(void* character800, std::uint8_t type);

constexpr std::int32_t  kRows             = 3;      // the deployed formation's rows
constexpr std::uint8_t  kStatusSlots      = 8;      // the player types with a party status (EPlayerType below MAX)
constexpr std::size_t   kSeatSlots        = 3;      // player 1 and two further seats (scopes::kMaxPlayers)

GetPlayerActorFn g_getPlayerActor = nullptr;
ReconcileFn      g_reconcile      = nullptr;
SetActorTypeFn   g_setActorType   = nullptr;
void**           g_partyManager   = nullptr;
std::uint8_t**   g_data           = nullptr;

/// The formation as the game keeps it, the field leader (the anchor), the statuses, and player 1's character.
struct Party
{
    std::uint8_t  types[kRows]{facts::kPlayerTypeNone, facts::kPlayerTypeNone, facts::kPlayerTypeNone};
    std::uint8_t  fieldLeader = facts::kPlayerTypeNone;
    std::uint8_t  statuses[kStatusSlots]{};
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
    out.leaderOne = ReadPointer(playerOneController, facts::kControllerLeaderCharacter);
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

/// Player 1's character by its controller's leader or pawn (during a switch the pawn may name the old one for a tick:
/// the session's grace covers it).
bool IsPlayerOne(const Party& party, const void* actor) noexcept
{
    return actor != nullptr && (actor == party.leaderOne || actor == party.pawnOne);
}

/// The rows a further player may play, by rank: the rows after the field leader's in the formation's order, wrapping,
/// that are not battle support, and player 1's only while player 1 is away from the field leader (on another row's
/// member: a battle leader, a switch; that row still counts as its rank's, and the field leader's character, left by
/// player 1, is played instead, while the field leader is a deployed member of the formation).
struct FreeRows
{
    std::int32_t ordinal[kRows]{};
    bool         playerOnes[kRows]{};  // that rank's row is player 1's character now
    std::int32_t count   = 0;
    bool         support = false;      // a row was skipped for its battle-support status
};

FreeRows FreeRowsOf(const Party& party) noexcept
{
    FreeRows           rows;
    const std::int32_t anchor           = Anchor(party);
    const bool         fieldInFormation = party.fieldLeader < kStatusSlots && party.types[anchor] == party.fieldLeader &&
                                  party.statuses[party.fieldLeader] == facts::kStatusParty;
    const bool playerOneOut = fieldInFormation && !IsPlayerOne(party, ActorOfType(party, party.fieldLeader));
    for (std::int32_t step = 1; step < kRows; ++step)
    {
        const std::int32_t ordinal = (anchor + step) % kRows;
        const std::uint8_t type    = party.types[ordinal];
        if (type >= kStatusSlots || type == party.fieldLeader)
        {
            continue;
        }
        if (party.statuses[type] == facts::kStatusBattleSupport)
        {
            rows.support = true;
            continue;
        }
        const bool ones = IsPlayerOne(party, ActorOfType(party, type));
        if (ones && !playerOneOut)
        {
            continue;
        }
        rows.ordinal[rows.count]    = ordinal;
        rows.playerOnes[rows.count] = ones;
        ++rows.count;
    }
    return rows;
}

/// Each seat's rank (party_rank_rules.h); [0] unused. Game thread.
std::array<int, kSeatSlots> g_rank{0, 1, 2};

/// The talk that holds the field lock is player 1's own and leaves the party where it stands (SetTalkKept). Game thread.
bool g_talkKept = false;

/// The field take tests (research/RE-slot-member.md) with the hold rule of field_hold.h, as a wait reason; None when
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
    // A treasure or push-button lock freezes only the interacting character, and a kept talk's lock is player 1's own
    // (LockKeepsMembers): the member is judged by its own state below (an event's listed actor is already in the cinema
    // mode above; a talk's own speaker or listener is held on a script layer). A kept talk's first hand runs before the
    // lock's bit is set: the mask still 0 with the talk kept is that moment, judged the same way.
    const std::uint32_t lock = ControlLockMask();
    if (!LockKeepsMembers(lock) && !(g_talkKept && lock == 0))
    {
        if (PushButtonLocked())
        {
            return Wait::PushButtonEvent;
        }
        // The game's own leader handed to its AI: the game is in a scene, a field action or the control lock, and it
        // hands every player's member the same way (the funnel, or its backstop in the session). A seat that landed
        // inside such a hand would play through it (seen live: Tifa taken mid-animation under lock 0x1).
        if (!LeaderWithPlayer())
        {
            return Wait::LeaderHanded;
        }
    }
    const std::uint8_t* ai = static_cast<const std::uint8_t*>(ReadPointer(member, facts::kCharacterFieldAi));
    field_hold::Judged  state;
    if (!JudgedState(member, state))
    {
        return Wait::NoFieldAi;
    }
    // A scene lead-in the level wrote on the member (lead_keep_rules.h kSceneLeadIns): its AI plays it through, the run and
    // the stand after it, whatever kind the state is now and whatever the lead rule would say of the run.
    if (state.layer == field_hold::kLayerLevel && lead_gate::SceneLeadIn(member))
    {
        return Wait::SceneLeadIn;
    }
    const std::uint8_t kind = state.kind;
    switch (field_hold::Of(state.layer, kind))
    {
    case field_hold::Hold::Free:
        return Wait::None;
    case field_hold::Hold::Animation:
        return Wait::Animation;
    case field_hold::Hold::Arrival:
        switch (lead_gate::Judge(member, ai, kind))
        {
        case lead_keep_rules::Verdict::Guide:
            return Wait::Guide;
        case lead_keep_rules::Verdict::AtMark:
            return Wait::AtMark;
        default:
            return Wait::None;  // staging, or kept by its player (the point marked: hud/lead_mark)
        }
    case field_hold::Hold::NoState:
        return Wait::NoFieldAi;
    default:
        return Wait::AiHeld;
    }
}

}  // namespace

std::uint8_t FieldLeaderType() noexcept
{
    std::uint8_t* data = nullptr;
    std::uint8_t  type = facts::kPlayerTypeNone;
    return Read(g_data, data) && data != nullptr && Read(data + facts::kDataFieldLeaderType, type) ? type : facts::kPlayerTypeNone;
}

bool IsFieldLeader(const void* character) noexcept
{
    Party party;
    return character != nullptr && Read(g_partyManager, party.manager) && party.manager != nullptr &&
           ActorOfType(party, FieldLeaderType()) == character;
}

std::uint8_t GameLeaderType() noexcept
{
    std::uint8_t* manager = nullptr;
    std::uint8_t  type    = facts::kPlayerTypeNone;
    return Read(g_partyManager, manager) && manager != nullptr && Read(manager + facts::kPartyLeaderType, type) ? type
                                                                                                               : facts::kPlayerTypeNone;
}

bool JudgedState(const void* member, field_hold::Judged& out) noexcept
{
    const auto*  ai    = static_cast<const std::uint8_t*>(ReadPointer(member, facts::kCharacterFieldAi));
    std::uint8_t layer = field_hold::kNoLayer, kind = 0;
    if (ai == nullptr || !Read(ai + facts::kFieldAiActiveLayer, layer) || !Read(ai + facts::kFieldAiActiveKind, kind))
    {
        return false;
    }
    std::uint8_t kinds[field_hold::kLayers] = {};
    // An interest scene never takes a member: it is judged by the request beneath it. Only with the
    // guard: without it an interest walk could start under the player and fault, so the member is then handed.
    if (kind == field_hold::kInterest && ai_move_guard::Active())
    {
        static_assert(field_hold::kLayers == facts::kFieldAiLayers, "the request stack's layers");
        for (std::size_t i = 0; i < field_hold::kLayers; ++i)
        {
            if (!Read(ai + facts::kFieldAiLayerRecords + i * facts::kFieldAiLayerBytes, kinds[i]))
            {
                return false;
            }
        }
        out = field_hold::JudgedState(layer, kind, kinds);
        return true;
    }
    out = {layer, kind};
    return true;
}

bool InterestOnTop(const void* member) noexcept
{
    const auto*  ai   = static_cast<const std::uint8_t*>(ReadPointer(member, facts::kCharacterFieldAi));
    std::uint8_t kind = 0;
    return ai != nullptr && Read(ai + facts::kFieldAiActiveKind, kind) && kind == field_hold::kInterest;
}

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
    const auto reconcile = platform::FindFunction(image, facts::kReconcileController);
    const auto setType   = platform::FindFunction(image, facts::kSetActorType);
    const auto party     = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    const auto data      = image.Rebase(facts::kDataSingletonGlobal, sizeof(void*));
    if (!find || !reconcile || !setType || !party || !data || !field_root::Installed())
    {
        return false;
    }
    g_getPlayerActor = reinterpret_cast<GetPlayerActorFn>(*find);
    g_reconcile      = reinterpret_cast<ReconcileFn>(*reconcile);
    g_setActorType   = reinterpret_cast<SetActorTypeFn>(*setType);
    g_partyManager   = reinterpret_cast<void**>(*party);
    g_data           = reinterpret_cast<std::uint8_t**>(*data);
    return true;
}

Choice Choose(int seat, void* playerOneController, void* taker, bool log, std::uint8_t standIn) noexcept
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
    // A seat standing in for player 1's former character (the game's leader change took the seat's member: the
    // battle leader option, a forced switch) plays that character, wherever its row is, while it is in the deployed
    // formation and not player 1's again (the session clears the mark when the game's leader returns home).
    const std::int32_t anchor = Anchor(party);
    std::int32_t       target = -1;
    bool               stood  = false;  // the stand-in was used
    if (standIn < kStatusSlots && party.statuses[standIn] == facts::kStatusParty)
    {
        for (std::int32_t i = 0; i < kRows; ++i)
        {
            if (party.types[i] == standIn)
            {
                if (!IsPlayerOne(party, ActorOfType(party, standIn)))
                {
                    target = i;
                    stood  = true;
                }
                break;
            }
        }
    }
    // Otherwise the row at the seat's rank (FreeRowsOf; a temporary state is waited on, never stepped over). A rank
    // whose row is player 1's now (player 1 away from the field leader) gives the field leader's character instead.
    const FreeRows rows    = FreeRowsOf(party);
    const int      rank    = Rank(seat);
    const bool     support = rows.support;  // a row skipped for its battle-support status (the wait's reason with no row)
    bool           toField = false;
    if (target < 0 && rank >= 1 && rank <= rows.count)
    {
        target  = rows.ordinal[rank - 1];
        toField = rows.playerOnes[rank - 1];
    }
    if (toField)
    {
        target = anchor;  // the seat's row is player 1's now: player 1's former character instead
    }
    if (target >= 0)
    {
        const std::uint8_t type  = party.types[target];
        void*              actor = ActorOfType(party, type);
        choice.type              = type;
        choice.ordinal           = target;
        void* holder             = actor != nullptr ? ReadPointer(actor, facts::kCharacterPlayerSlot) : nullptr;
        if (actor == nullptr)
        {
            choice.wait = Wait::NotLoaded;
        }
        else if (holder != nullptr && holder != taker)
        {
            choice.wait = Wait::Held;
        }
        else
        {
            choice.wait = FieldWait(actor);
            if (choice.wait == Wait::None || JoinsThroughHold(choice.wait))
            {
                choice.member = actor;
            }
        }
    }
    if (choice.ordinal < 0 && support)
    {
        choice.wait = Wait::Support;  // types 0 1 9 read as a solo party (seen with Barret)
    }
    if (log)
    {
        platform::LogDebug(L"party member: formation types {} {} {}, field leader type {} at ordinal {}; seat {}'s row (rank {} of {}): "
                           L"ordinal {} type {}{}: {}",
                           party.types[0], party.types[1], party.types[2], party.fieldLeader, anchor, seat, rank, rows.count,
                           choice.ordinal, choice.type,
                           stood     ? std::format(L" (standing in for player 1's former character, type {})", standIn)
                           : toField ? std::wstring(L" (player 1 plays this seat's row: the field leader's character instead)")
                                     : std::wstring(),
                           Describe(choice.wait));
    }
    return choice;
}

int Rank(int seat) noexcept
{
    return seat >= 1 && seat < static_cast<int>(kSeatSlots) ? g_rank[static_cast<std::size_t>(seat)] : 0;
}

void SetRank(int seat, int rank) noexcept
{
    if (seat >= 1 && seat < static_cast<int>(kSeatSlots))
    {
        g_rank[static_cast<std::size_t>(seat)] = rank;
    }
}

int Ranks(void* playerOneController) noexcept
{
    Party party;
    return g_getPlayerActor != nullptr && ReadParty(playerOneController, party) ? FreeRowsOf(party).count : 0;
}

void* RowActor(int seat, void* playerOneController, std::uint8_t standIn) noexcept
{
    Party party;
    if (!ReadParty(playerOneController, party))
    {
        return nullptr;
    }
    const Choice choice = Choose(seat, playerOneController, nullptr, false, standIn);
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

const wchar_t* WhyNotInSlot(int seat, void* member, std::uint8_t type, void* playerOneController, std::uint8_t standIn) noexcept
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
    const Choice slot = Choose(seat, playerOneController, nullptr, false, standIn);
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
        return L"a guide (a lead or a run to a mark: a level step waits on its arrival, or the story's marker rides on it): its AI walks it";
    case Wait::AtMark:
        return L"brought to its move's point: its AI finishes the arrival until the level's next setting";
    case Wait::SceneLeadIn:
        return L"on a scene lead-in (a run and a stand a scene opens on, excluded from the lead rule by name): its AI plays it through";
    default:
        return nullptr;  // a push-button event or the game's leader handed is the game's own hand (party_events); the rest holds nothing
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
    return field_root::ControlLockMask();
}

static_assert(field_hold::kLockPushButton == facts::kControlLockPushButton && field_hold::kLockTreasure == facts::kControlLockTreasure,
              "the interaction lock bits are the game's EndFieldExclusion bits");

bool PushButtonLocked() noexcept
{
    return (ControlLockMask() & facts::kControlLockPushButton) != 0;
}

void SetTalkKept(bool kept) noexcept
{
    g_talkKept = kept;
}

bool TalkKept() noexcept
{
    return g_talkKept;
}

bool LockKeepsMembers(std::uint32_t mask) noexcept
{
    return field_hold::KeepsMembers(mask, g_talkKept);
}

bool LeaderWithPlayer() noexcept
{
    void* leader = field_root::PartyLeader(facts::kLeaderCurrent);
    return leader != nullptr && ReadPointer(leader, facts::kCharacterPlayerSlot) != nullptr;
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
        return L"no free party member left for this player (every row past player 1's is another player's, or the party has none)";
    case Wait::Support:
        return L"the rows beyond player 1's hold only battle-support members (guests the game fights with, not deployed members)";
    case Wait::NotLoaded:
        return L"its character is not loaded";
    case Wait::Held:
        return L"another controller holds it";
    case Wait::Cinema:
        return L"in a cutscene or event (the cinema actor mode)";
    case Wait::PushButtonEvent:
        return L"a push-button event (a switch, a lever, a terminal) holds the field with another lock";
    case Wait::LeaderHanded:
        return L"the game's leader is with its AI (a scene, a field action, the control lock), or unread";
    case Wait::AiHeld:
        return L"a script holds its field AI (a script layer, or the unread ESCAPE_POSITION kind)";
    case Wait::Animation:
        return L"its AI plays a scripted animation (an ANIMATION or one-shot kind)";
    case Wait::Guide:
        return L"its AI walks a guide (a lead or a run to a mark: a level step waits on its arrival, or the story's marker rides on it)";
    case Wait::AtMark:
        return L"its player brought it to its move's point: its AI finishes the arrival (a lead waits for player 1 first), until the "
               L"level's next setting";
    case Wait::SceneLeadIn:
        return L"the level's scene lead-in is on it (a run and a stand a scene opens on, excluded from the lead rule by name): its AI "
               L"plays it through to the scene";
    case Wait::NoFieldAi:
        return L"its field AI has no state yet";
    case Wait::Unread:
        return L"the party could not be read";
    }
    return L"?";
}

}  // namespace lss::players::party_member
