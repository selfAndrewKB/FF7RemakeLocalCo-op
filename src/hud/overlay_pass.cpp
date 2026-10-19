#include "hud/overlay_pass.h"

namespace lss::hud::overlay_pass {

namespace {
Pass g_pass = Pass::Single;  // game thread only
}

Pass Current() noexcept
{
    return g_pass;
}

void Set(Pass pass) noexcept
{
    g_pass = pass;
}

}  // namespace lss::hud::overlay_pass
