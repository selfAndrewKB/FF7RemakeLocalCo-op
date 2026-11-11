#pragma once

// Where a further pad's key events go while the game's menus, minigames and bike chases need them
// (research/RE-pause-menu-any-pad.md; research/RE-minigame-input.md).
//
// A further pad's events normally write that pad's own copy of the action tables (players/pad_buttons.h), so the
// game's one pause poll, its menus and its minigames never see them: only player 1 could pause, move in a menu or
// play a minigame. The game itself treats every pad alike, so the lever restores its own rule in two places:
//   - while the game's input is shared, a new press is the game's own pad event and every pad drives it, as in an
//     unmodded game: a menu owns input (the game's pause or menu state, or its own IsPauseMenuClosed is false), or a
//     minigame or a bike chase holds the whole screen (views/whole_screen.h; SharedPlay below: the game plays them on
//     one picture, reading its pad-blind pollers and the merged stick);
//   - outside that, a press that pressed only menu openers (PauseMenu, MapMenu, EnemyIntel, besides the raw key
//     itself) in the pad's copy is also given to the game, so the game's own poll opens the menu with its own gates.
// Each key's release goes where its press went (the copy, the game, or both), so neither side keeps a press it
// cannot release, and a key's repeats follow its press too. Pure functions, host-tested (tests/menu_pads_test.cpp).

#include "players/action_tables.h"
#include "views/whole_screen_rules.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lss::players::menu_pads {

inline constexpr std::uint8_t kToCopy = 1;  // the pad's own copy (its player's state)
inline constexpr std::uint8_t kToGame = 2;  // the game's own tables and menus

/// One pad's keys that are down, each with where its press went. A key that does not fit is not recorded (its
/// release then takes the untracked route).
struct Keys
{
    static constexpr std::size_t kMax = 16;
    std::array<std::uint64_t, kMax> names{};  // the key's FName; 0 = a free entry
    std::array<std::uint8_t, kMax>  where{};
};

/// Where the key's press went; 0 if the key is not down (or was not recorded).
inline std::uint8_t Where(const Keys& keys, std::uint64_t key) noexcept
{
    for (std::size_t i = 0; i < Keys::kMax; ++i)
    {
        if (key != 0 && keys.names[i] == key)
        {
            return keys.where[i];
        }
    }
    return 0;
}

/// Adds `to` to where the key's press went. False if the key is new and every entry is taken.
inline bool Mark(Keys& keys, std::uint64_t key, std::uint8_t to) noexcept
{
    if (key == 0)
    {
        return false;
    }
    std::size_t freeAt = Keys::kMax;
    for (std::size_t i = 0; i < Keys::kMax; ++i)
    {
        if (keys.names[i] == key)
        {
            keys.where[i] |= to;
            return true;
        }
        if (keys.names[i] == 0 && freeAt == Keys::kMax)
        {
            freeAt = i;
        }
    }
    if (freeAt == Keys::kMax)
    {
        return false;
    }
    keys.names[freeAt] = key;
    keys.where[freeAt] = to;
    return true;
}

inline void Forget(Keys& keys, std::uint64_t key) noexcept
{
    for (std::size_t i = 0; i < Keys::kMax; ++i)
    {
        if (key != 0 && keys.names[i] == key)
        {
            keys.names[i] = 0;
            keys.where[i] = 0;
        }
    }
}

/// The whole-screen moments the game plays with every pad as its own, as it does a menu: a minigame, which the game
/// runs on player 1's picture through its pad-blind pollers and the stick aggregate, and a bike chase, driven the
/// same way. A cutscene, a scripted scene, a held black screen or a loading screen plays nothing of a pad's, and a
/// menu has the game's own test (players/game_state.h MenuOwnsInput), so neither is one.
constexpr bool SharedPlay(views::whole_screen::Reason reason) noexcept
{
    return reason == views::whole_screen::Reason::Minigame || reason == views::whole_screen::Reason::Bike;
}

/// The route of a key event: a key already down (a repeat, or its release) goes where its press went; a new press
/// (or the release of a key never recorded) goes to the game while the game's input is shared (a menu owns it, or a
/// minigame or a bike chase holds the screen), otherwise to the pad's copy.
inline std::uint8_t Route(const Keys& keys, std::uint64_t key, bool shared) noexcept
{
    const std::uint8_t where = Where(keys, key);
    if (where != 0)
    {
        return where;
    }
    return shared ? kToGame : kToCopy;
}

/// The names of the copy's raw elements this event pressed: latch 0 before, latch 1 and held after (the game's own
/// press rule wrote them). `before` and `after` are indexed like `shape`; a size mismatch yields nothing.
inline void Pressed(const action_tables::Shape& shape, std::span<const action_tables::State> before,
                    std::span<const action_tables::State> after, std::vector<std::uint64_t>& out)
{
    out.clear();
    if (before.size() != shape.names.size() || after.size() != shape.names.size())
    {
        return;
    }
    for (std::size_t i = 0; i < shape.names.size(); ++i)
    {
        if (shape.names[i] != 0 && before[i][3] == 0 && after[i][3] != 0 && after[i][1] != 0)
        {
            out.push_back(shape.names[i]);
        }
    }
}

/// Whether any of the pressed names is a menu opener (0 entries in `openers` are unread names, never matched).
inline bool AnyOpener(std::span<const std::uint64_t> pressed, std::span<const std::uint64_t> openers) noexcept
{
    for (const std::uint64_t name : pressed)
    {
        for (const std::uint64_t o : openers)
        {
            if (o != 0 && name == o)
            {
                return true;
            }
        }
    }
    return false;
}

/// Whether a press is a menu opener the game must see too: it pressed at least one opener, and nothing besides
/// openers and the element named by the event's own key.
inline bool OpensMenu(std::span<const std::uint64_t> pressed, std::span<const std::uint64_t> openers,
                      std::uint64_t key) noexcept
{
    bool opener = false;
    for (const std::uint64_t name : pressed)
    {
        bool isOpener = false;
        for (const std::uint64_t o : openers)
        {
            isOpener = isOpener || (o != 0 && name == o);
        }
        if (isOpener)
        {
            opener = true;
        }
        else if (name != key)
        {
            return false;
        }
    }
    return opener;
}

}  // namespace lss::players::menu_pads
