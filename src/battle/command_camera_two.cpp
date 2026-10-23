#include "battle/command_camera_two.h"

#include "battle/command_menu_two.h"
#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/player_two_camera.h"
#include "players/seat_objects.h"
#include "players/seats.h"

#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace lss::battle::command_camera_two {

namespace {

using platform::Read;
using players::SeatName;
using players::kSeatsBuilt;

/// FEndCameraFlavorCommand's per-frame aim (vtable +0x20): this, the frame time, and three of the operator loop's
/// arguments it passes through.
using AimFn = void (*)(void* flavor, float deltaSeconds, void* a3, void* a4, void* a5);

struct Seat
{
    std::uint32_t aims          = 0;  // the flavor's aim ran on this seat's operator
    std::uint32_t ownSlot       = 0;  // ... with the seat's own controller in the UI slot (player 1: the game's)
    std::uint32_t otherOperator = 0;  // ... on a flavor whose operator is not the ticking seat's (never expected)
    std::uint32_t targets       = 0;  // the flavor's stored target changed since the last aim on this seat
    std::uint32_t scopesHeld    = 0;  // the UI scope around the operator tick, held
    std::uint32_t scopesRefused = 0;  // ... refused while the seat's menu was in battle
    void*         lastTarget    = nullptr;  // the stored target at the last aim (set by the flavor's first tick, changed with the row)
    std::uint8_t  active        = 0;
    std::uint8_t  started       = 0;
};

std::array<Seat, players::scopes::kMaxPlayers> g_seats;  // [0] = player 1 (the game's own camera), [s] = seat s
bool                   g_enabled = true;
platform::VTableEntry  g_aimSlot;
AimFn                  g_aim         = nullptr;
std::uintptr_t         g_commandSlot = 0;  // EndUISystem[19], live

/// The flavor's aim, on whichever operator ticks: counted per seat, the target change logged.
void Aim(void* flavor, float deltaSeconds, void* a3, void* a4, void* a5)
{
    const int seat = players::player_two_camera::TickingSeat();
    Seat&     s    = g_seats[seat];
    ++s.aims;
    void* flavorOperator = nullptr;
    void* expected       = seat == 0 ? players::player_two_camera::OperatorOne() : players::seat_objects::Operator(seat);
    if (Read(static_cast<std::uint8_t*>(flavor) + facts::kFlavorOperator, flavorOperator) && flavorOperator != expected)
    {
        ++s.otherOperator;
    }
    void* live = nullptr;
    const bool own = seat != 0 && Read(reinterpret_cast<const void*>(g_commandSlot), live) && live != nullptr &&
                     live == command_menu_two::Controller(seat);
    if (own)
    {
        ++s.ownSlot;
    }
    g_aim(flavor, deltaSeconds, a3, a4, a5);
    Read(static_cast<std::uint8_t*>(flavor) + facts::kFlavorActive, s.active);
    Read(static_cast<std::uint8_t*>(flavor) + facts::kFlavorStarted, s.started);
    // The stored target is set by the flavor's first tick (before this slot runs in the same frame) and changed by
    // this one when the cursor's row changes, so it is compared across frames, not around the call.
    void* target = nullptr;
    if (Read(static_cast<std::uint8_t*>(flavor) + facts::kCommandFlavorTarget, target) && target != s.lastTarget)
    {
        ++s.targets;
        s.lastTarget = target;
        void* const after = target;
        platform::LogDebug(L"command camera 2: {}'s Command camera {} ({:#x}; UI slot {})", SeatName(seat),
                           after == nullptr ? L"let its target go" : L"frames a new target",
                           reinterpret_cast<std::uintptr_t>(after),
                           seat == 0 ? L"the game's" : own ? L"its own" : L"player 1's");
    }
}

std::wstring Reading()
{
    std::wstring text;
    for (int seat = 0; seat <= kSeatsBuilt; ++seat)
    {
        const Seat& s = g_seats[seat];
        if (seat != 0)
        {
            text += L"; ";
        }
        text += std::format(L"{}: aims {} (own UI slot {}, other operator {}), target changes {} (now {:#x}), active {} started {}",
                            SeatName(seat), s.aims, s.ownSlot, s.otherOperator, s.targets,
                            reinterpret_cast<std::uintptr_t>(s.lastTarget), s.active, s.started);
        if (seat != 0)
        {
            text += std::format(L", UI scope held {} refused {}", s.scopesHeld, s.scopesRefused);
        }
    }
    return text;
}

}  // namespace

TickScope::TickScope(int seat) noexcept
{
    if (!g_enabled || seat < 1 || seat > kSeatsBuilt || !command_menu_two::Ready(seat))
    {
        return;
    }
    std::uint8_t battle = 0;
    if (!Read(static_cast<std::uint8_t*>(command_menu_two::Controller(seat)) + facts::kCommandCtrlBattleHudMode, battle) ||
        battle == 0)
    {
        return;
    }
    Seat& s = g_seats[seat];
    scope_.emplace(seat, players::scopes::UiSlot | players::scopes::Focus);
    if (scope_->Held())
    {
        ++s.scopesHeld;
        return;
    }
    scope_.reset();  // closes what opened
    const std::uint32_t refused = ++s.scopesRefused;
    if (refused <= 5 || refused % 600 == 0)
    {
        platform::Log(L"command camera 2: {}'s operator ticked without {}'s UI context ({} time(s)); its Command camera reads "
                      L"player 1's menu this frame",
                      SeatName(seat), SeatName(seat), refused);
    }
}

bool Install(const platform::GameImage& image) noexcept
{
    // The switch and the reading first: the UI scope works without the aim slot (the reading then shows the scope
    // counts alone).
    control::Register({.name  = L"cmd-camera-two",
                       .shape = control::Shape::OnOff,
                       .help  = L"each seat's operator tick holds the seat's UI context in battle, so the Command camera frames "
                                L"the seat's own target list (battle/command_camera_two.h; on)",
                       .set   = [](const control::Value& value) {
                           g_enabled = std::get<bool>(value);
                           platform::Log(L"command camera 2 {}", g_enabled ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled}; }});
    control::Register({.name = L"cmd-camera-two.aims", .shape = control::Shape::Text,
                       .help = L"the Command camera's per-frame aim by seat: runs, runs with the seat's own UI slot, runs on a "
                               L"foreign operator, target changes, its on/started bytes; the UI scope held and refused",
                       .get  = [] { return control::Value{Reading()}; }});
    const auto slot = image.Rebase(facts::kCommandCtrlSlotGlobal, sizeof(void*));
    if (!slot)
    {
        platform::Log(L"command camera 2: the UI slot is outside the image; the aim reading is off, the UI scope stays");
        return false;
    }
    g_commandSlot = *slot;
    if (!platform::ReplaceSlot(image, facts::kCommandFlavorAim, reinterpret_cast<std::uintptr_t>(&Aim), g_aimSlot))
    {
        platform::Log(L"command camera 2: the Command flavor's aim slot differs; the aim reading is off, the UI scope stays");
        return false;
    }
    g_aim = reinterpret_cast<AimFn>(g_aimSlot.Original());
    platform::Log(L"command camera 2: installed; each seat's Command camera reads the seat's own target list in battle");
    return true;
}

void Remove() noexcept
{
    g_aimSlot.Restore();
    g_aim = nullptr;
}

}  // namespace lss::battle::command_camera_two
