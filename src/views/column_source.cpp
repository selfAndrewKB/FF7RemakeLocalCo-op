#include "views/column_source.h"

#include "control/registry.h"

#include <format>

namespace lss::views::column_source {

const wchar_t* NameOf(Why why) noexcept
{
    switch (why)
    {
        case Why::None: return L"their own";
        case Why::WholeScreen: return L"the game takes the whole screen";
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
               ? std::format(L"{}'s view gives way to player 1's single view: {}", name, NameOf(after.why))
               : std::format(L"{}'s view shows player 1's camera and HUD: {}", name, NameOf(after.why));
}

void RegisterCommands(Override& choice)
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"single-view",
                       .shape = control::Shape::Word,
                       .words = {L"auto", L"on", L"off"},
                       .help  = L"player 1's picture for every further player (one window: the engine's single view; "
                                L"a monitor row: player 1's camera and HUD in their columns) for the game's whole-screen "
                                L"moments (auto, the default), always, or never; a development override "
                                L"(views/column_source.h)",
                       .set   = [&choice](const Value& v) {
                           const std::wstring& mode = std::get<std::wstring>(v);
                           // Applied at the next tick, which logs each seat's change.
                           if (mode == L"on")
                           {
                               choice = Override::Single;
                               return Outcome{true, L"player 1's picture for every further player while one is in"};
                           }
                           if (mode == L"off")
                           {
                               choice = Override::Split;
                               return Outcome{true, L"every player's own view, always"};
                           }
                           choice = Override::Auto;
                           return Outcome{true, L"by the game's whole-screen moments (auto)"};
                       },
                       .get = [&choice] {
                           return Value{std::wstring(choice == Override::Single  ? L"on"
                                                     : choice == Override::Split ? L"off"
                                                                                   : L"auto")};
                       }});
}

}  // namespace lss::views::column_source
