#include "views/column_source.h"

#include "control/registry.h"

#include <format>

namespace lss::views::column_source {

const wchar_t* NameOf(Why why) noexcept
{
    switch (why)
    {
        case Why::None: return L"their own";
        case Why::Console: return L"single-view on";
        case Why::NoPlayer: return L"no player's column";
    }
    return L"?";
}

std::wstring Describe(int seat, const SeatSource& before, const SeatSource& after)
{
    if (before == after)
    {
        return {};
    }
    const wchar_t* name = players::SeatName(seat);
    if (after.source == Source::Self)
    {
        return std::format(L"{}'s view is their own again", name);
    }
    return after.source == Source::Screen
               ? std::format(L"{}'s view would give way to player 1's single view ({}): not built, their column shows "
                             L"player 1's picture instead",
                             name, NameOf(after.why))
               : std::format(L"{}'s view shows player 1's camera and HUD: {}", name, NameOf(after.why));
}

void RegisterCommands(Override& choice)
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"single-view",
                       .shape = control::Shape::Word,
                       .words = {L"auto", L"on", L"off"},
                       .help  = L"player 1's picture in every further player's column (the mirror look: player 1's camera "
                                L"and HUD) for the game's states that ask for it (auto, the default: none today), always, "
                                L"or never; a development override (views/column_source.h)",
                       .set   = [&choice](const Value& v) {
                           const std::wstring& mode = std::get<std::wstring>(v);
                           // Applied at the next tick, which logs each seat's change.
                           if (mode == L"on")
                           {
                               choice = Override::Single;
                               return Outcome{true, L"player 1's picture in every further column while a further player is in"};
                           }
                           if (mode == L"off")
                           {
                               choice = Override::Split;
                               return Outcome{true, L"every player's own view, always"};
                           }
                           choice = Override::Auto;
                           return Outcome{true, L"by the game's states (auto)"};
                       },
                       .get = [&choice] {
                           return Value{std::wstring(choice == Override::Single  ? L"on"
                                                     : choice == Override::Split ? L"off"
                                                                                   : L"auto")};
                       }});
}

}  // namespace lss::views::column_source
