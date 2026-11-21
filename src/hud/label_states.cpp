#include "hud/label_states.h"

#include <cstring>

namespace lss::hud::label_states {

Parked EmptyState() noexcept
{
    Parked      empty{};
    const float one = 1.0f;
    std::memcpy(empty.quads + kShadowInQuads, &one, sizeof(one));
    std::memcpy(empty.quads + kShadowInQuads + sizeof(one), &one, sizeof(one));
    return empty;
}

bool LabelBook::AnyInside() const noexcept
{
    for (std::size_t i = 0; i < count_; ++i)
    {
        if (records_[i].inside)
        {
            return true;
        }
    }
    return false;
}

bool LabelBook::Holds(const void* controller) const noexcept
{
    for (std::size_t i = 0; i < count_; ++i)
    {
        if (records_[i].controller == controller)
        {
            return records_[i].inside;
        }
    }
    return false;
}

LabelBook::Record* LabelBook::Find(const void* controller) noexcept
{
    for (std::size_t i = 0; i < count_; ++i)
    {
        if (records_[i].controller == controller)
        {
            return &records_[i];
        }
    }
    return nullptr;
}

void LabelBook::Drop(std::size_t index) noexcept
{
    Record& record = records_[index];
    access_.freeParked(record.parked);       // whichever player's state is aside, the mod owns it
    access_.releaseWeak(record.controller);  // last touch: at zero the game frees the block
    record = records_[count_ - 1];
    records_[count_ - 1] = Record{};
    --count_;
}

bool LabelBook::SwapIn(void* widget, void* controller) noexcept
{
    if (!access_.usable(widget))
    {
        ++counts_.refused;
        return true;
    }
    Record* record = Find(controller);
    if (record != nullptr && record->inside)
    {
        return true;  // a second call in the same pass: the seat's state is already in
    }
    const std::uint64_t stamp = access_.stamp(widget);
    if (record == nullptr)
    {
        if (count_ == kMaxLabels)
        {
            ++counts_.full;
            return true;
        }
        if (!access_.holdWeak(controller))
        {
            ++counts_.refused;
            return true;
        }
        record             = &records_[count_++];
        record->widget     = widget;
        record->controller = controller;
        record->stamp      = stamp;
        record->inside     = false;
        record->parked     = fresh_;  // the seat's state is laid out from it
        ++counts_.created;
    }
    else if (stamp != record->stamp)
    {
        // What the seat's state was laid out from changed (its text, a font, the seat's art): laid out again from the
        // fresh state.
        access_.freeParked(record->parked);
        record->parked = fresh_;
        record->stamp  = stamp;
        ++counts_.resets;
    }
    if (!access_.swap(widget, record->parked))
    {
        ++counts_.faults;
        return false;
    }
    record->inside = true;
    ++counts_.swapsIn;
    return true;
}

void LabelBook::SwapBack() noexcept
{
    for (std::size_t i = count_; i-- > 0;)
    {
        Record& record = records_[i];
        if (!record.inside)
        {
            continue;
        }
        record.inside = false;
        if (access_.alive(record.controller))
        {
            // Stamped as it goes out, while the seat's state is in: a stamp that reads the block's text then describes
            // the text the seat's state was laid out for.
            record.stamp = access_.stamp(record.widget);
            if (access_.swap(record.widget, record.parked))
            {
                ++counts_.swapsOut;
                continue;
            }
            // The widget keeps the seat's state; the game's next relayout for player 1 lays player 1's out again.
            ++counts_.faults;
        }
        else
        {
            ++counts_.diedInside;  // its destructor freed the seat's state; player 1's, aside, is freed here
        }
        Drop(i);
    }
}

void LabelBook::Sweep() noexcept
{
    SwapBack();  // nothing is inside outside the passes; kept so a missed swap-back cannot outlive a frame
    for (std::size_t i = count_; i-- > 0;)
    {
        if (!access_.alive(records_[i].controller))
        {
            ++counts_.diedAside;
            Drop(i);
        }
    }
}

void LabelBook::ReleaseAll() noexcept
{
    SwapBack();
    for (std::size_t i = count_; i-- > 0;)
    {
        ++counts_.released;
        Drop(i);
    }
}

}  // namespace lss::hud::label_states
