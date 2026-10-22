#pragma once

// The coins enemies hold (battle/enemy_coin.h): a lock-free table keyed by the enemy's battle AI controller, each
// coin the player it names (1..4) and when it was tossed, plus one mark per coin: "an action has run since this
// toss" (the toss rule reads the game's action field at the game's own decision call; the mark remembers that it was
// seen non-zero). A controller's coin is in one of four neighbouring slots; a new one takes its own slot, else an
// empty one, else the one tossed longest ago (a live enemy tosses at each decision, every few seconds; the oldest is
// a gone enemy's). Two threads placing coins in the same slot at once leave one of them: the other tosses again at
// its next question. Pure over its arrays with the clock passed in, so it is tested on the host
// (tests/coin_table_test.cpp). Any thread. Taken from Rebirth's player_two/coin_table.h; the mark is new.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace lss::battle::coin_table {

inline constexpr std::size_t    kBits   = 10;
inline constexpr std::size_t    kSlots  = std::size_t{1} << kBits;
inline constexpr std::size_t    kProbe  = 4;
inline constexpr std::uintptr_t kMask   = 7;  // the coin: the key with the player in its low bits (1..4), 0 empty
inline constexpr std::size_t    kNoSlot = kSlots;

/// The `probe`th slot a key may sit in (keys are object addresses: 16-byte aligned, the low bits free).
constexpr std::size_t SlotOf(std::uintptr_t key, std::size_t probe) noexcept
{
    const auto base = static_cast<std::size_t>(((key >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - kBits));
    return (base + probe) & (kSlots - 1);
}

class CoinTable
{
public:
    /// The slot holding `key`'s coin, or kNoSlot.
    std::size_t Find(std::uintptr_t key) const noexcept
    {
        for (std::size_t probe = 0; probe < kProbe; ++probe)
        {
            const std::size_t slot = SlotOf(key, probe);
            if ((coins_[slot].load(std::memory_order_relaxed) & ~kMask) == key)
            {
                return slot;
            }
        }
        return kNoSlot;
    }

    /// The player `key`'s coin names: 1..4; 0 with no coin.
    int PlayerOf(std::uintptr_t key) const noexcept
    {
        const std::size_t slot = Find(key);
        const auto        held = slot == kNoSlot ? 0 : coins_[slot].load(std::memory_order_relaxed);
        return (held & ~kMask) != key ? 0 : static_cast<int>(held & kMask);
    }

    /// `key` counts `player` (1..4) from `now` (seconds): in its own slot, else an empty one, else the one tossed
    /// longest ago among its four. The mark is cleared: this toss has seen no action yet.
    void Set(std::uintptr_t key, int player, std::uint32_t now) noexcept
    {
        const std::size_t slot = Place(key, now);
        tossedAt_[slot].store(now, std::memory_order_relaxed);
        marked_[slot].store(0, std::memory_order_relaxed);
        coins_[slot].store(key | static_cast<std::uintptr_t>(player & kMask), std::memory_order_relaxed);
    }

    /// Forgets `key`'s coin, if it still holds one (a compare-and-swap: a coin placed there meanwhile stays).
    void Clear(std::uintptr_t key) noexcept
    {
        if (const std::size_t slot = Find(key); slot != kNoSlot)
        {
            std::uintptr_t held = coins_[slot].load(std::memory_order_relaxed);
            if ((held & ~kMask) == key)
            {
                coins_[slot].compare_exchange_strong(held, 0, std::memory_order_relaxed);
            }
        }
    }

    /// An action has run under `key`'s coin (nothing without a coin).
    void Mark(std::uintptr_t key) noexcept
    {
        if (const std::size_t slot = Find(key); slot != kNoSlot)
        {
            marked_[slot].store(1, std::memory_order_relaxed);
        }
    }

    /// Whether an action has run under `key`'s coin (false without a coin).
    bool Marked(std::uintptr_t key) const noexcept
    {
        const std::size_t slot = Find(key);
        return slot != kNoSlot && marked_[slot].load(std::memory_order_relaxed) != 0;
    }

private:
    /// Where a new coin for `key` goes: its own slot, else an empty one, else the one tossed longest ago.
    std::size_t Place(std::uintptr_t key, std::uint32_t now) const noexcept
    {
        if (const std::size_t own = Find(key); own != kNoSlot)
        {
            return own;
        }
        std::size_t   oldest = kNoSlot;
        std::uint32_t age    = 0;
        for (std::size_t probe = 0; probe < kProbe; ++probe)
        {
            const std::size_t slot = SlotOf(key, probe);
            if (coins_[slot].load(std::memory_order_relaxed) == 0)
            {
                return slot;
            }
            const std::uint32_t slotAge = now - tossedAt_[slot].load(std::memory_order_relaxed);
            if (oldest == kNoSlot || slotAge >= age)
            {
                age    = slotAge;
                oldest = slot;
            }
        }
        return oldest;
    }

    std::atomic<std::uintptr_t> coins_[kSlots]{};
    std::atomic<std::uint32_t>  tossedAt_[kSlots]{};  // seconds on the steady clock
    std::atomic<std::uint8_t>   marked_[kSlots]{};
};

}  // namespace lss::battle::coin_table
