// Host-side tests for hud::button_art_rules, run without the game: each player's device as the game's listener should
// see it, and when two players' art is the same.

#include "hud/button_art_rules.h"

#include <cstdio>

namespace {

using namespace lss::hud::button_art_rules;

int g_failures = 0;

#define EXPECT(condition)                                                    \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);     \
            ++g_failures;                                                    \
        }                                                                    \
    } while (false)

constexpr DeviceArt kXbox{kClassXInput, kSubClassXInput, 0};
constexpr DeviceArt kDualSense{kClassPlayStation, kSubClassPlayStation, 0};
constexpr DeviceArt kDualShock4{kClassPlayStation, kSubClassPlayStation, 1};

/// Each device's bytes: an XInput pad is Xbox art whatever player 1 plays on; a PlayStation pad its own kind; the keyboard
/// keycaps with the game's sub-class (its `,pad` tags in player 1's pad art).
void EachDevice()
{
    EXPECT((ArtOf(Device::XInputPad, false, kDualShock4) == DeviceArt{kClassXInput, kSubClassXInput, 1}));
    EXPECT(SameArt(ArtOf(Device::XInputPad, true, kDualSense), kXbox));
    EXPECT(ArtOf(Device::PlayStationPad, true, kXbox) == kDualShock4);
    EXPECT(ArtOf(Device::PlayStationPad, false, kXbox) == kDualSense);
    EXPECT((ArtOf(Device::Keyboard, false, kXbox) == DeviceArt{kClassKeyboard, kSubClassXInput, 0}));
    EXPECT((ArtOf(Device::Keyboard, false, kDualShock4) == DeviceArt{kClassKeyboard, kSubClassPlayStation, 1}));
}

/// Same art: one class and the byte it reads; the DualShock 4 byte means nothing to an Xbox pad, the sub-class nothing to
/// a PlayStation pad.
void SameArtRule()
{
    EXPECT(SameArt(kXbox, DeviceArt{kClassXInput, kSubClassXInput, 1}));
    EXPECT(!SameArt(kDualSense, kDualShock4));
    EXPECT(SameArt(kDualSense, DeviceArt{kClassPlayStation, 2, 0}));
    EXPECT(!SameArt(kXbox, kDualSense));
    EXPECT(!SameArt(DeviceArt{kClassKeyboard, 0, 0}, DeviceArt{kClassKeyboard, 1, 0}));
    EXPECT(SameArt(DeviceArt{kClassKeyboard, 1, 0}, DeviceArt{kClassKeyboard, 1, 1}));
    // Player 1 on a DualSense, player 2 on another DualSense: no pass of its own needed.
    EXPECT(SameArt(ArtOf(Device::PlayStationPad, false, kDualSense), kDualSense));
}

}  // namespace

int main()
{
    EachDevice();
    SameArtRule();
    if (g_failures != 0)
    {
        std::printf("button_art: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("button_art: all passed\n");
    return 0;
}
