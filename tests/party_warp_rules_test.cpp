// Host-side tests for the field warp's rules (world/party_warp_rules.h): when the ask is refused, and the directions
// tried around player 1's character. Run without the game.

#include "world/party_warp_rules.h"

#include <cmath>
#include <cstdio>
#include <cwchar>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "party_warp_rules: all passed\n" : "party_warp_rules: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
}  // namespace lss::test

#define EXPECT(condition)                                                    \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);     \
            ++lss::test::g_failures;                                         \
        }                                                                    \
    } while (false)

namespace {

using namespace lss::world::party_warp_rules;

bool Near(const Vec3& a, float x, float y)
{
    return std::fabs(a.x - x) < 1e-4f && std::fabs(a.y - y) < 1e-4f && a.z == 0.0f;
}

bool Says(const wchar_t* why, const wchar_t* part)
{
    return why != nullptr && std::wcsstr(why, part) != nullptr;
}

/// Free play: nothing refuses.
void FreePlay()
{
    EXPECT(Refusal(Facts{}) == nullptr);
}

/// Each gate refuses alone, with its own reason.
void EachGate()
{
    EXPECT(Says(Refusal(Facts{.paused = true}), L"paused"));
    EXPECT(Says(Refusal(Facts{.fieldLock = 0x1}), L"holds the party"));
    EXPECT(Says(Refusal(Facts{.leaderWithPlayer = false}), L"holds the party"));
    EXPECT(Says(Refusal(Facts{.memberWithPlayer = false}), L"holds that character"));
    EXPECT(Says(Refusal(Facts{.memberInField = false}), L"holds that character"));
    EXPECT(Says(Refusal(Facts{.memberBusy = true}), L"it is in a field action"));
    EXPECT(Says(Refusal(Facts{.leaderBusy = true}), L"player 1's character"));
    EXPECT(Says(Refusal(Facts{.beside = true}), L"already stands beside"));
}

/// Beside: near in the ground plane and on the same level; a floor above or below, or the edge itself, is not.
void BesideRule()
{
    EXPECT(Beside(0.0f, 0.0f));
    EXPECT(Beside(kNearCm - 1.0f, kLevelCm - 1.0f));
    EXPECT(Beside(100.0f, -(kLevelCm - 1.0f)));
    EXPECT(!Beside(kNearCm, 0.0f));
    EXPECT(!Beside(100.0f, kLevelCm));
    EXPECT(!Beside(100.0f, -kLevelCm));
    EXPECT(!Beside(1000.0f, 0.0f));
    // A field action outranks it: the reason given is the one the player can do something about first.
    EXPECT(Says(Refusal(Facts{.memberBusy = true, .beside = true}), L"it is in a field action"));
}

/// With several gates shut, the widest reason is given: the pause, then the scene, then the character's own hold.
void Order()
{
    EXPECT(Says(Refusal(Facts{.paused = true, .fieldLock = 0x2, .memberBusy = true}), L"paused"));
    EXPECT(Says(Refusal(Facts{.fieldLock = 0x2, .memberWithPlayer = false, .memberBusy = true}), L"holds the party"));
    EXPECT(Says(Refusal(Facts{.memberWithPlayer = false, .memberBusy = true, .leaderBusy = true}), L"holds that character"));
    EXPECT(Says(Refusal(Facts{.memberBusy = true, .leaderBusy = true}), L"it is in a field action"));
}

/// Facing +X: behind is -X, left is -Y, right is +Y, in front +X.
void SpotsFacingEast()
{
    const auto spots = Spots(0.0f);
    EXPECT(Near(spots[0], -1, 0));
    EXPECT(Near(spots[1], 0, -1));
    EXPECT(Near(spots[2], 0, 1));
    EXPECT(Near(spots[3], 1, 0));
}

/// Facing +Y (yaw 90): behind is -Y, left is +X, right is -X.
void SpotsFacingNorth()
{
    const auto spots = Spots(90.0f);
    EXPECT(Near(spots[0], 0, -1));
    EXPECT(Near(spots[1], 1, 0));
    EXPECT(Near(spots[2], -1, 0));
    EXPECT(Near(spots[3], 0, 1));
}

/// Facing between the axes (yaw 45): unit vectors, behind opposite in front, left opposite right, left a quarter turn
/// from in front.
void SpotsFacingBetween()
{
    const auto  spots = Spots(45.0f);
    const float h     = std::sqrt(0.5f);
    EXPECT(Near(spots[3], h, h));
    EXPECT(Near(spots[0], -h, -h));
    EXPECT(Near(spots[1], h, -h));
    EXPECT(Near(spots[2], -h, h));
    for (const Vec3& spot : spots)
    {
        EXPECT(std::fabs(std::hypot(spot.x, spot.y) - 1.0f) < 1e-4f);
    }
}

void Names()
{
    EXPECT(std::wcscmp(SpotName(0), L"behind it") == 0);
    EXPECT(std::wcscmp(SpotName(1), L"to its left") == 0);
    EXPECT(std::wcscmp(SpotName(2), L"to its right") == 0);
    EXPECT(std::wcscmp(SpotName(3), L"in front of it") == 0);
    EXPECT(std::wcscmp(SpotName(kTries), L"nowhere") == 0);
}

}  // namespace

int main()
{
    FreePlay();
    EachGate();
    BesideRule();
    Order();
    SpotsFacingEast();
    SpotsFacingNorth();
    SpotsFacingBetween();
    Names();
    return lss::test::Report();
}
