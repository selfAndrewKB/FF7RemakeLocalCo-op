#include "hud/overlay_pass.h"

namespace lss::hud::overlay_pass {

namespace {
int   g_column = kSingle;  // game thread only
Seats g_seats;             // game thread only
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

void SetSeats(const Seats& seats) noexcept
{
    g_seats = seats;
}

int SeatOf(int column) noexcept
{
    return g_seats.SeatOf(column);
}

int ColumnOf(int seat) noexcept
{
    return g_seats.ColumnOf(seat);
}

}  // namespace lss::hud::overlay_pass
