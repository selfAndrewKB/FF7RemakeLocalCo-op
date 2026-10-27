#include "players/single_view.h"

namespace lss::players {

SingleViewChange SingleView::Update(bool wanted) noexcept
{
    if (wanted != wanted_)
    {
        wanted_ = wanted;
        failed_ = false;
    }
    if (failed_ || wanted == single_)
    {
        return SingleViewChange::None;
    }
    return wanted ? SetSingle() : SetSplit();
}

SingleViewChange SingleView::Release() noexcept
{
    wanted_ = false;
    failed_ = false;
    return single_ && owned_ ? SetSplit() : SingleViewChange::None;
}

SingleViewChange SingleView::SetSingle() noexcept
{
    const auto current = access_.read();
    if (!current)
    {
        failed_ = true;
        return SingleViewChange::SingleFailed;
    }
    if (*current)
    {
        single_ = true;
        owned_  = false;  // someone else's: never cleared by the mod
        return SingleViewChange::AlreadySingle;
    }
    const auto after = access_.write(true) ? access_.read() : std::nullopt;
    if (!after || !*after)
    {
        failed_ = true;
        return SingleViewChange::SingleFailed;
    }
    single_ = true;
    owned_  = true;
    return SingleViewChange::Single;
}

SingleViewChange SingleView::SetSplit() noexcept
{
    if (!owned_)
    {
        single_ = false;  // not the mod's switch: left as it was found
        return SingleViewChange::LeftAlone;
    }
    const auto current = access_.read();
    if (!current)
    {
        failed_ = true;
        return SingleViewChange::SplitFailed;
    }
    if (!*current)
    {
        single_ = owned_ = false;  // someone cleared it meanwhile
        return SingleViewChange::LeftAlone;
    }
    const auto after = access_.write(false) ? access_.read() : std::nullopt;
    if (!after || *after)
    {
        failed_ = true;
        return SingleViewChange::SplitFailed;
    }
    single_ = owned_ = false;
    return SingleViewChange::Split;
}

std::wstring Describe(SingleViewChange change)
{
    switch (change)
    {
        case SingleViewChange::None: return {};
        case SingleViewChange::Single:
            return L"single view: player 1's view alone (the viewport's split switch set); the further players stay in";
        case SingleViewChange::AlreadySingle:
            return L"single view: the split was already switched off by someone else; used, and not cleared by the mod";
        case SingleViewChange::SingleFailed:
            return L"single view: the split switch could not be read or set; the columns stay, player 1's picture in every "
                   L"further one";
        case SingleViewChange::Split: return L"single view: over, the split is back (the viewport's split switch cleared)";
        case SingleViewChange::LeftAlone:
            return L"single view: over; the split switch is not the mod's (or was cleared meanwhile), left as it is";
        case SingleViewChange::SplitFailed:
            return L"single view: the split switch could not be read or cleared; the screen stays single";
    }
    return {};
}

}  // namespace lss::players
