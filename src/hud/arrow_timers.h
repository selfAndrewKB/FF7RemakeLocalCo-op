#pragma once

// Each seat's own timers for the off-screen enemy arrows (hud/outside_icons; Rebirth's hud_rules::TimerSwap, one store
// per seat here and no allocation), a pure store tested on the host (tests/arrow_timers_test.cpp).
//
// Around a seat's run of the game's arrow tick: player 1's timer of each arrow is set aside and the seat's put in (0 for
// an arrow the seat has not seen); after it, the seat keeps what its run left on the arrows it saw (the others are
// dropped) and player 1's go back on the arrows that were there before the run (an arrow the run added keeps what it
// has). Fixed capacity: the game pools 16 arrow widgets; hud/outside_icons skips a seat's run when more arrows are live, and
// an arrow past the capacity here would start at 0 for the seat (counted).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace lss::hud::arrow_timers {

/// An arrow: its weak widget as stored (index and serial) and the source it was made for, so a widget reused from the
/// pool for another enemy is a new arrow.
struct Key
{
    std::uint64_t widget = 0;
    std::uint64_t source = 0;
    friend bool operator==(const Key&, const Key&) = default;
};

inline constexpr std::size_t kCapacity = 64;

class List
{
public:
    void Clear() noexcept { count_ = 0; }

    /// False when full.
    bool Add(const Key& key, float t) noexcept
    {
        if (count_ == kCapacity)
        {
            return false;
        }
        items_[count_++] = {key, t};
        return true;
    }

    std::optional<float> Find(const Key& key) const noexcept
    {
        for (std::size_t i = 0; i < count_; ++i)
        {
            if (items_[i].key == key)
            {
                return items_[i].t;
            }
        }
        return std::nullopt;
    }

    std::size_t Count() const noexcept { return count_; }

private:
    struct Item
    {
        Key   key;
        float t = 0.0f;
    };
    std::array<Item, kCapacity> items_{};
    std::size_t                 count_ = 0;
};

class SeatTimers
{
public:
    /// The seat's run starts.
    void BeginIn() noexcept { one_.Clear(); }

    /// Sets player 1's timer for `key` aside and gives the seat's: 0 for an arrow the seat has not seen.
    float In(const Key& key, float mine) noexcept
    {
        if (!one_.Add(key, mine))
        {
            ++overflows_;
        }
        return own_.Find(key).value_or(0.0f);
    }

    /// The seat's run ended.
    void BeginOut() noexcept { seen_.Clear(); }

    /// Keeps the seat's timer for `key` and gives player 1's back if the arrow was there before the run.
    std::optional<float> Out(const Key& key, float theirs) noexcept
    {
        if (!seen_.Add(key, theirs))
        {
            ++overflows_;
        }
        return one_.Find(key);
    }

    /// The seat's timers are what its run saw; nothing of player 1's is held any more.
    void EndOut() noexcept
    {
        own_ = seen_;
        one_.Clear();
    }

    /// The seat's column did not run (the player out, no battle): its timers forgotten.
    void Forget() noexcept
    {
        one_.Clear();
        seen_.Clear();
        own_.Clear();
    }

    std::size_t Count() const noexcept { return own_.Count(); }
    std::uint32_t Overflows() const noexcept { return overflows_; }

private:
    List          one_;   // player 1's, set aside during the run
    List          seen_;  // the seat's, as the run left them
    List          own_;   // the seat's, between frames
    std::uint32_t overflows_ = 0;
};

}  // namespace lss::hud::arrow_timers
