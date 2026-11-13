#include "players/scopes.h"

#include "engine_facts.h"
#include "platform/byte_scope.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/field_root.h"
#include "players/weak_object.h"

#include <array>
#include <cstring>

namespace lss::players::scopes {

namespace {

using platform::Read;

using platform::ByteContext;
using Kind = ByteContext::Kind;

constexpr std::uint8_t kTypeNone = facts::kPlayerTypeNone;

// Set once at install (live addresses).
void**         g_partyManager = nullptr;
std::uint8_t** g_input        = nullptr;  // the SquareInput owner
std::uint8_t** g_uiFilters    = nullptr;  // EndUISystem[37]

struct Player
{
    ByteContext  primary, cameraOperator, leader, uiSlot, focus, latches;
    void*        controller     = nullptr;
    void*        member         = nullptr;
    std::uint8_t type           = kTypeNone;
    void*        commandCtrl    = nullptr;
    std::uint8_t otherLeaderType = kTypeNone;
};
std::array<Player, kMaxPlayers> g_players;

bool ValidPlayer(int player) noexcept
{
    return player >= 1 && player < kMaxPlayers;
}

/// One Set context holding `value` at `live`.
template <class T>
void DefineValue(ByteContext& context, std::uintptr_t live, const T& value) noexcept
{
    const ByteContext::Range range{live, sizeof(T), Kind::Set};
    if (live == 0 || !context.Define(std::span<const ByteContext::Range>(&range, 1)))
    {
        context.Clear();
        return;
    }
    std::memcpy(context.Value(0).data(), &value, sizeof(T));
}

std::uintptr_t PartyManager() noexcept
{
    void* manager = nullptr;
    return Read(g_partyManager, manager) ? reinterpret_cast<std::uintptr_t>(manager) : 0;
}

std::uintptr_t Object(std::uint8_t** global) noexcept
{
    std::uint8_t* object = nullptr;
    return Read(global, object) ? reinterpret_cast<std::uintptr_t>(object) : 0;
}

/// The focus context: the four ranges exchanged with the player's own copy and the delegate muted. The copy starts
/// empty, with the capture map's FirstFreeIndex at -1 as the game's constructor leaves an empty map.
void DefineFocus(ByteContext& context) noexcept
{
    const std::uintptr_t owner = Object(g_input);
    if (owner == 0)
    {
        context.Clear();
        return;
    }
    const ByteContext::Range ranges[] = {
        {owner + facts::kFocusPath, facts::kFocusPathBytes, Kind::Exchange},
        {owner + facts::kFocusLayers, facts::kFocusLayersBytes, Kind::Exchange},
        {owner + facts::kFocusCause, 1, Kind::Exchange},
        {owner + facts::kFocusCaptureMap, facts::kFocusCaptureMapBytes, Kind::Exchange},
        {owner + facts::kFocusMenuDelegate, facts::kFocusMenuDelegateBytes, Kind::Set},
    };
    if (!context.Define(ranges))
    {
        return;
    }
    const std::int32_t none = -1;
    std::memcpy(context.Value(3).data() + facts::kFocusCaptureFirstFree, &none, sizeof(none));
}

void DefineLatches(ByteContext& context) noexcept
{
    const std::uintptr_t filters = Object(g_uiFilters);
    if (filters == 0)
    {
        context.Clear();
        return;
    }
    const ByteContext::Range range{filters + facts::kUiFilterLatches, 2, Kind::Exchange};
    context.Define(std::span<const ByteContext::Range>(&range, 1));
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto party   = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    const auto input   = image.Rebase(facts::kSquareInputOwnerGlobal, sizeof(void*));
    const auto filters = image.Rebase(facts::kUiFilterSlotGlobal, sizeof(void*));
    if (!party || !field_root::Installed() || !input || !filters)
    {
        platform::Log(L"scopes: a global is outside the image; no further player's work can run in its own scopes");
        return false;
    }
    g_partyManager = reinterpret_cast<void**>(*party);
    g_input        = reinterpret_cast<std::uint8_t**>(*input);
    g_uiFilters    = reinterpret_cast<std::uint8_t**>(*filters);
    return true;
}

void BindController(int player, void* controller) noexcept
{
    if (!ValidPlayer(player))
    {
        return;
    }
    Player& p    = g_players[player];
    p.controller = controller;
    if (controller == nullptr)
    {
        p.primary.Clear();
        return;
    }
    DefineValue(p.primary, PartyManager() + facts::kPartyPrimaryController, controller);
}

void BindOperator(int player, const std::int32_t (&weak)[2]) noexcept
{
    if (!ValidPlayer(player))
    {
        return;
    }
    Player&        p    = g_players[player];
    const auto     root = g_partyManager != nullptr ? reinterpret_cast<std::uintptr_t>(field_root::Root()) : 0;  // 0 until installed
    if (weak[1] == 0 || root == 0)
    {
        p.cameraOperator.Clear();
        return;
    }
    DefineValue(p.cameraOperator, root + facts::kRootOperatorWeak, weak);
}

void BindMember(int player, void* member, std::uint8_t type) noexcept
{
    if (!ValidPlayer(player))
    {
        return;
    }
    Player& p = g_players[player];
    p.member  = member;
    p.type    = member == nullptr ? kTypeNone : type;
    std::int32_t weak[2]{-1, 0};
    if (member == nullptr || !weak_object::WeakPairOf(member, weak))
    {
        p.leader.Clear();
        return;
    }
    // Held, not set: the game's one leader is player 1's between the player's calls. A player's own battle switch makes
    // the game write the new member here inside the player's call (the session hears that write through the leader
    // funnel and takes the member); kept, the write would leave the player's member as the game's leader for every
    // leader-bound system, player 1's character with no controls.
    const std::uintptr_t     manager = PartyManager();
    const ByteContext::Range ranges[] = {
        {manager + facts::kPartyLeaderWeak, sizeof(weak), Kind::Hold},
        {manager + facts::kPartyLeaderType, 1, Kind::Hold},
    };
    if (manager == 0 || !p.leader.Define(ranges))
    {
        p.leader.Clear();
        return;
    }
    std::memcpy(p.leader.Value(0).data(), weak, sizeof(weak));
    p.leader.Value(1)[0] = type;
}

void BindCommandController(int player, void* controller) noexcept
{
    if (!ValidPlayer(player))
    {
        return;
    }
    Player& p     = g_players[player];
    p.commandCtrl = controller;
    if (controller == nullptr)
    {
        p.uiSlot.Clear();
        p.focus.Clear();
        p.latches.Clear();
        return;
    }
    DefineValue(p.uiSlot, field_root::CommandControllerSlot(), controller);
    DefineFocus(p.focus);
    DefineLatches(p.latches);
}

void Unbind(int player) noexcept
{
    if (ValidPlayer(player))
    {
        BindCommandController(player, nullptr);
        BindMember(player, nullptr, kTypeNone);
        const std::int32_t none[2]{-1, 0};
        BindOperator(player, none);
        BindController(player, nullptr);
    }
}

void* Controller(int player) noexcept
{
    return ValidPlayer(player) ? g_players[player].controller : nullptr;
}

void* Member(int player) noexcept
{
    return ValidPlayer(player) ? g_players[player].member : nullptr;
}

std::uintptr_t PartyManagerAddress() noexcept
{
    return PartyManager();
}

std::uint8_t MemberType(int player) noexcept
{
    return ValidPlayer(player) ? g_players[player].type : kTypeNone;
}

void* CommandController(int player) noexcept
{
    return ValidPlayer(player) ? g_players[player].commandCtrl : nullptr;
}

std::uint8_t OtherLeaderType(int player) noexcept
{
    return ValidPlayer(player) && g_players[player].leader.IsOpen() ? g_players[player].otherLeaderType : kTypeNone;
}

bool FocusPathSet(int player) noexcept
{
    if (!ValidPlayer(player) || !g_players[player].focus.Defined())
    {
        return false;
    }
    std::int32_t count = 0;
    std::memcpy(&count, g_players[player].focus.Value(0).data() + 8, sizeof(count));  // the path's TArray num
    return count > 0;
}

int OpenPlayer(unsigned which) noexcept
{
    for (int player = 1; player < kMaxPlayers; ++player)
    {
        const Player& p = g_players[player];
        if (((which & Primary) != 0 && p.primary.IsOpen()) || ((which & Operator) != 0 && p.cameraOperator.IsOpen()) ||
            ((which & Leader) != 0 && p.leader.IsOpen()) || ((which & UiSlot) != 0 && p.uiSlot.IsOpen()) ||
            ((which & Focus) != 0 && p.focus.IsOpen()) || ((which & Latches) != 0 && p.latches.IsOpen()))
        {
            return player;
        }
    }
    return 0;
}

Scope::Scope(int player, unsigned which) noexcept : player_(player)
{
    if (!ValidPlayer(player))
    {
        return;
    }
    Player& p = g_players[player];
    // The order matters for the leader: its saved byte is read after it opens.
    const struct
    {
        unsigned     bit;
        ByteContext* context;
    } table[] = {{Primary, &p.primary}, {Operator, &p.cameraOperator}, {Leader, &p.leader},
                 {UiSlot, &p.uiSlot},   {Focus, &p.focus},             {Latches, &p.latches}};
    for (const auto& entry : table)
    {
        if ((which & entry.bit) == 0)
        {
            continue;
        }
        if (entry.bit == Leader)
        {
            // The game's own leader type (player 1's), read before this player's goes in.
            Read(reinterpret_cast<const void*>(PartyManager() + facts::kPartyLeaderType), p.otherLeaderType);
        }
        if (entry.bit == Latches && !p.latches.Defined() && p.commandCtrl != nullptr)
        {
            DefineLatches(p.latches);  // the UI filters' object can appear later than the bind
        }
        if (!entry.context->Open())
        {
            return;  // ~Scope closes what opened; held_ stays false
        }
        opened_ |= entry.bit;
    }
    held_ = true;
}

Scope::~Scope()
{
    if (!ValidPlayer(player_))
    {
        return;
    }
    Player& p = g_players[player_];
    const struct
    {
        unsigned     bit;
        ByteContext* context;
    } table[] = {{Latches, &p.latches}, {Focus, &p.focus},             {UiSlot, &p.uiSlot},
                 {Leader, &p.leader},   {Operator, &p.cameraOperator}, {Primary, &p.primary}};
    for (const auto& entry : table)
    {
        if ((opened_ & entry.bit) != 0 && entry.context->Close() != 0)
        {
            platform::LogDebug(L"scopes: player {}'s {} slot was changed by the game inside a scope; {}", player_ + 1,
                               entry.bit == Primary ? L"primary" : entry.bit == Operator ? L"operator" : entry.bit == Leader ? L"leader" : L"UI",
                               entry.bit == Leader ? L"player 1's put back (the game's leader stays player 1's)" : L"the game's value kept");
        }
    }
}

LeaderPause::LeaderPause(int player) noexcept : player_(player)
{
    if (ValidPlayer(player) && g_players[player].leader.IsOpen())
    {
        if (g_players[player].leader.Close() != 0)  // the game's (player 1's) leader bytes back
        {
            platform::LogDebug(L"scopes: the game wrote the leader slot inside player {}'s call before a pause; player 1's put back",
                               player + 1);
        }
        paused_ = true;
    }
}

LeaderPause::~LeaderPause()
{
    if (paused_ && !g_players[player_].leader.Open())
    {
        platform::LogWarning(L"scopes: player {}'s leader could not be written in again after a pause", player_ + 1);
    }
}

}  // namespace lss::players::scopes
