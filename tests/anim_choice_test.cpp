// Host test for world/anim_choice.h: the camera choice for the game's animation judge and player 2's snapshot.

#include "world/anim_choice.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok)
    {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

}  // namespace

int main()
{
    using namespace lss::world::anim_choice;
    constexpr float kDeg = 3.14159265f / 180.0f;

    // Tiers at the judge's thresholds.
    Check(Tier({300.0f, 170.0f * kDeg}) == 2, "within 5 m: full whatever the angle");
    Check(Tier({600.0f, 10.0f * kDeg}) == 2, "in the cone within 100 m: full");
    Check(Tier({600.0f, 70.0f * kDeg}) == 1, "out of the cone at 6 m: bones kept, not full");
    Check(Tier({2500.0f, 70.0f * kDeg}) == 0, "out of the cone at 25 m: neither");
    Check(Tier({12000.0f, 5.0f * kDeg}) == 1, "beyond 100 m in the cone: bones kept, not full");

    // The choice: player 1 behind the mesh, player 2 facing it near.
    const Vec mesh{0, 0, 0};
    const View one = Measure(mesh, {-3000, 0, 0}, {-1, 0, 0});  // 30 m, facing away
    const View two = Measure(mesh, {400, 0, 0}, {-1, 0, 0});    // 4 m, facing it
    Check(Near(one.distance, 3000.0f) && Near(one.angle, 180.0f * kDeg, 1e-2f), "measure: behind player 1");
    Check(Near(two.distance, 400.0f) && Near(two.angle, 0.0f, 1e-3f), "measure: in front of player 2");
    Check(PreferTwo(one, two), "player 2's camera chosen when it sees the mesh better");
    Check(!PreferTwo(two, one), "player 1's camera kept when it sees the mesh better");
    Check(!PreferTwo(one, one), "a tie keeps player 1's camera");
    // Same tier, the nearer (higher significance) wins.
    Check(PreferTwo({800.0f, 10.0f * kDeg}, {600.0f, 10.0f * kDeg}), "same tier: nearer player 2 chosen");

    // The snapshot: a reset frame, then a moving and turning camera.
    History h;
    Snapshot s = Predict(h, {100, 0, 0}, ForwardOf(0, 0), false, 1.0f / 60.0f);
    Check(Near(s.location.x, 100.0f) && Near(s.forward.x, 1.0f) && s.speed == 0.0f && s.turn == 0.0f,
          "first frame: no prediction");
    const float dt = 0.1f;
    s = Predict(h, {200, 0, 0}, ForwardOf(0, 10), false, dt);  // moved 100 cm in 0.1 s, turned 10 degrees
    Check(Near(s.speed, 1000.0f, 0.5f), "speed in cm/s");
    Check(Near(s.location.x, 300.0f, 0.1f), "location 0.1 s ahead");
    Check(Near(s.turn, 10.0f * kDeg / dt, 1e-2f), "turn rate in rad/s");
    const float yaw = std::atan2(s.forward.y, s.forward.x) / kDeg;
    Check(Near(yaw, 13.0f, 0.05f), "forward 1.3 turns ahead of the previous one");
    Check(Near(Length(s.forward), 1.0f), "forward stays a unit vector");
    s = Predict(h, {500, 0, 0}, ForwardOf(0, 10), true, dt);
    Check(Near(s.location.x, 500.0f) && s.speed == 0.0f, "a camera cut resets the prediction");
    s = Predict(h, {600, 0, 0}, ForwardOf(0, 10), false, dt);
    Check(Near(s.location.x, 600.0f) && s.speed == 0.0f, "the frame after a cut resets too (the game's rule)");

    std::printf(g_failures == 0 ? "anim_choice: all passed\n" : "anim_choice: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
