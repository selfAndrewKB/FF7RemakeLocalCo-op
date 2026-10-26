#include "hud/overlay_pass.h"

namespace lss::hud::overlay_pass {

namespace {
int                 g_column = kSingle;  // game thread only
views::columns::Map g_map;               // game thread only
}

int Column() noexcept
{
    return g_column;
}

void Set(int column) noexcept
{
    g_column = column >= 0 && column < kMaxColumns ? column : kSingle;
}

bool Split() noexcept
{
    return g_column >= 0;
}

bool Further() noexcept
{
    return g_column >= 1;
}

void SetMap(const views::columns::Map& map) noexcept
{
    g_map = map;
}

int SeatOf(int column) noexcept
{
    static_assert(kNoSeat == views::columns::kUnknown, "no seat reads the same in both");
    const int seat = g_map.SeatOf(column);
    return column == 0 ? 0 : seat >= 0 ? seat : kNoSeat;  // column 0 is player 1's, listed or not
}

int ColumnOf(int seat) noexcept
{
    return g_map.ColumnOf(seat);
}

}  // namespace lss::hud::overlay_pass
