#pragma once

// Whose sun cascade a view may use (research/RE-haze-per-view.md).
//
// The game makes the sun's shadow cascades once per view and records the view each one was made for (stock
// FProjectedShadowInfo::DependentView; null for a shadow that belongs to no one view, such as a lamp's). Two consumers
// take them from the light's one list without asking whose they are: the forward light data of each view, and the
// translucency lighting volume's light injection. With more than one view, a view then takes the other views'
// cascades as well: in the forward light data the last view's overwrite its own, and in the lighting volume the sun is
// injected once per view. The game's own rule elsewhere (its shadow projection, its per-view culling) is: a cascade
// with an owner is used by that view only. These helpers state that rule. Pure, host-tested
// (tests/sun_owner_test.cpp).

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lss::render::sun_owner {

/// True if the shadow belongs to another view: it has an owner, and the owner is not `view`.
inline bool OtherView(std::uintptr_t owner, std::uintptr_t view) noexcept
{
    return owner != 0 && owner != view;
}

/// One light of the lighting volume's injection list, as the game lays it out (32 bytes): the light, its shadow (or
/// null), whether its light function applies, the light function's material.
struct Injection
{
    std::uintptr_t light;
    std::uintptr_t shadow;
    std::uint64_t  applyLightFunction;
    std::uintptr_t material;
};
static_assert(sizeof(Injection) == 0x20 && offsetof(Injection, shadow) == 0x08);

/// The entries a view keeps: every entry whose shadow is not another view's. `ownerOf(shadow)` reads a shadow's owner
/// (0 if it has none or cannot be read). `out` gets the kept entries in order; returns how many were dropped.
template <class OwnerOf>
std::size_t Keep(std::span<const Injection> in, std::uintptr_t view, OwnerOf ownerOf, std::vector<Injection>& out)
{
    out.clear();
    std::size_t dropped = 0;
    for (const Injection& entry : in)
    {
        if (entry.shadow != 0 && OtherView(ownerOf(entry.shadow), view))
        {
            ++dropped;
        }
        else
        {
            out.push_back(entry);
        }
    }
    return dropped;
}

/// Whether any entry would be dropped (the common case is none: the list then goes to the game as it is).
template <class OwnerOf>
bool AnyOther(std::span<const Injection> in, std::uintptr_t view, OwnerOf ownerOf)
{
    for (const Injection& entry : in)
    {
        if (entry.shadow != 0 && OtherView(ownerOf(entry.shadow), view))
        {
            return true;
        }
    }
    return false;
}

}  // namespace lss::render::sun_owner
