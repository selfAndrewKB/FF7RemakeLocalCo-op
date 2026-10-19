#include "platform/byte_scope.h"

#include "platform/guarded.h"

#include <cstring>

namespace lss::platform {

bool ByteContext::Define(std::span<const Range> ranges)
{
    if (open_)
    {
        return false;
    }
    slots_.clear();
    for (const Range& range : ranges)
    {
        if (range.live == 0 || range.size == 0)
        {
            slots_.clear();
            return false;
        }
        slots_.push_back({range, std::vector<std::uint8_t>(range.size), std::vector<std::uint8_t>(range.size)});
    }
    return true;
}

bool ByteContext::Clear()
{
    if (open_)
    {
        return false;
    }
    slots_.clear();
    return true;
}

std::span<std::uint8_t> ByteContext::Value(std::size_t index) noexcept
{
    return index < slots_.size() ? std::span<std::uint8_t>(slots_[index].value) : std::span<std::uint8_t>();
}

std::span<const std::uint8_t> ByteContext::Value(std::size_t index) const noexcept
{
    return index < slots_.size() ? std::span<const std::uint8_t>(slots_[index].value) : std::span<const std::uint8_t>();
}

bool ByteContext::Open() noexcept
{
    if (open_ || slots_.empty())
    {
        return false;
    }
    std::size_t written = 0;
    for (; written < slots_.size(); ++written)
    {
        Slot& slot = slots_[written];
        auto* live = reinterpret_cast<void*>(slot.range.live);
        if (!CopyGuarded(slot.saved.data(), live, slot.range.size) || !WriteGuarded(live, slot.value.data(), slot.range.size))
        {
            break;
        }
    }
    if (written < slots_.size())
    {
        // A range faulted: the ones already written go back, in reverse.
        for (std::size_t i = written; i-- > 0;)
        {
            WriteGuarded(reinterpret_cast<void*>(slots_[i].range.live), slots_[i].saved.data(), slots_[i].range.size);
        }
        return false;
    }
    open_ = true;
    return true;
}

unsigned ByteContext::Close() noexcept
{
    if (!open_)
    {
        return 0;
    }
    unsigned changed = 0;
    for (std::size_t i = slots_.size(); i-- > 0;)
    {
        Slot& slot = slots_[i];
        auto* live = reinterpret_cast<void*>(slot.range.live);
        if (slot.range.kind == Kind::Exchange)
        {
            CopyGuarded(slot.value.data(), live, slot.range.size);  // what the game left there is the player's now
            WriteGuarded(live, slot.saved.data(), slot.range.size);
            continue;
        }
        std::vector<std::uint8_t> now(slot.range.size);
        if (CopyGuarded(now.data(), live, slot.range.size) && std::memcmp(now.data(), slot.value.data(), slot.range.size) != 0)
        {
            ++changed;  // the game wrote the slot inside the scope: its newer value is kept
            continue;
        }
        WriteGuarded(live, slot.saved.data(), slot.range.size);
    }
    open_ = false;
    return changed;
}

}  // namespace lss::platform
