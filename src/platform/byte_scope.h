#pragma once

// The game's single slots answered for one player for the length of a call.
//
// Square Enix's code keeps one copy of state that is really per player: the primary controller, the party
// leader, the UI system's command controller, the input object's focus context, the battle system's target
// fields. The mod makes the game's own calls for a further player with that player's values in those slots
// and the game's values put back after. A ByteContext is one player's values for a set of live byte ranges; a
// ByteScope installs them on construction and restores on destruction. Two kinds of range:
//   - Set: a fixed value (a pointer, a type byte). It is written in; the saved live bytes go back out, unless
//     the game wrote the slot inside the scope, in which case the game's newer value is kept (Close reports it).
//   - Exchange: the player's own copy of state the game keeps there (a focus path, a lock). The copy is
//     exchanged with the live bytes both ways, so what the game wrote inside the scope stays the player's.
// No engine dependency: the host test exercises it on plain memory. The reads and writes are guarded against
// a fault (an unreadable slot opens nothing). Not thread-safe; one thread per context.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lss::platform {

class ByteContext
{
public:
    enum class Kind : std::uint8_t
    {
        Set,
        Exchange,
    };

    struct Range
    {
        std::uintptr_t live = 0;
        std::uint32_t  size = 0;
        Kind           kind = Kind::Set;
    };

    /// Replaces the ranges; every value starts zero. Refused (false, unchanged) while open.
    bool Define(std::span<const Range> ranges);
    /// Forgets the ranges (an unbound context opens nothing). Refused while open.
    bool Clear();

    bool Defined() const noexcept { return !slots_.empty(); }
    bool IsOpen() const noexcept { return open_; }
    std::size_t Count() const noexcept { return slots_.size(); }
    const Range& RangeAt(std::size_t index) const noexcept { return slots_[index].range; }

    /// The value written in (Set) or the player's copy (Exchange), to initialise or read. Empty if out of range.
    std::span<std::uint8_t>       Value(std::size_t index) noexcept;
    std::span<const std::uint8_t> Value(std::size_t index) const noexcept;

    /// Writes every value in. False (nothing left changed) if already open, undefined, or a range faulted.
    bool Open() noexcept;
    /// Puts the live bytes back. Returns the number of Set ranges the game had changed inside (left as the game
    /// wrote them); 0 when everything was still the scope's own.
    unsigned Close() noexcept;

private:
    struct Slot
    {
        Range                     range;
        std::vector<std::uint8_t> value;
        std::vector<std::uint8_t> saved;
    };

    std::vector<Slot> slots_;
    bool              open_ = false;
};

class ByteScope
{
public:
    explicit ByteScope(ByteContext& context) noexcept : context_(context), held_(context.Open()) {}
    ~ByteScope()
    {
        if (held_)
        {
            changed_ = context_.Close();
        }
    }
    ByteScope(const ByteScope&)            = delete;
    ByteScope& operator=(const ByteScope&) = delete;

    bool Held() const noexcept { return held_; }

private:
    ByteContext& context_;
    bool         held_    = false;
    unsigned     changed_ = 0;
};

}  // namespace lss::platform
