// Host-side tests for the UI targets' rotation rule (hud/target_rotation.h). Run without the game. The targets are
// stand-in pointers; the "member" is what the game's scene-targets member would name at each call.

#include "hud/target_rotation.h"

#include <cstdio>

namespace {

using lss::hud::target_rotation::kMaxColumns;
using lss::hud::target_rotation::Rotation;

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

int a = 0, b = 0, c = 0;
void* const A = &a;
void* const B = &b;
void* const C = &c;

/// A Render as the game runs it: the UI passes in column order, then the post-processes in column order.
/// `passes` the columns whose UI pass runs; the allocator hands a fresh target whenever the member is null.
struct Render
{
    Rotation rotation;
    void*    member = nullptr;
    int      allocated = 0;
    void*    fresh[3]  = {A, B, C};

    void UiPass(int column, bool enabled)
    {
        if (rotation.BeforeUiPass(column, member, enabled))
        {
            member = nullptr;
        }
        if (member == nullptr)
        {
            member = fresh[allocated++];  // the game's allocator
        }
        rotation.AfterUiPass(column, member);
    }

    /// What the view samples.
    void* PostProcess(int column, int playerColumns)
    {
        if (void* wanted = rotation.BeforePostProcess(column, member, playerColumns); wanted != nullptr)
        {
            member = wanted;
        }
        return member;
    }
};

/// One view: nothing moves, nothing held.
void OneView()
{
    Render r;
    r.UiPass(0, true);
    EXPECT(r.member == A && r.allocated == 1);
    EXPECT(r.PostProcess(0, 1) == A);
    void* held[kMaxColumns];
    EXPECT(r.rotation.TakeHeld(held) == 0);
}

/// Two columns, both players': each samples its own; the mod ends holding column 0's (the member keeps column 1's).
void TwoColumns()
{
    Render r;
    r.UiPass(0, true);
    r.UiPass(1, true);
    EXPECT(r.allocated == 2 && r.member == B);
    EXPECT(r.rotation.Held(0) && !r.rotation.Held(1));
    EXPECT(r.PostProcess(0, 2) == A);
    EXPECT(r.rotation.Held(1) && !r.rotation.Held(0));
    EXPECT(r.PostProcess(1, 2) == B);
    void* held[kMaxColumns];
    EXPECT(r.rotation.TakeHeld(held) == 1 && held[0] == A);
    EXPECT(r.rotation.Count() == 0);  // the record is empty for the next Render
}

/// Three columns, the third a mirror's: it gets its own target for its pass (no drawing over column 1's) and
/// samples column 0's; the mod releases the two targets the member does not keep.
void ThreeColumnsMirrorLast()
{
    Render r;
    r.UiPass(0, true);
    r.UiPass(1, true);
    r.UiPass(2, true);
    EXPECT(r.allocated == 3 && r.member == C);
    EXPECT(r.PostProcess(0, 2) == A);
    EXPECT(r.PostProcess(1, 2) == B);
    EXPECT(r.PostProcess(2, 2) == A);  // the mirror's view shows player 1's HUD
    void* held[kMaxColumns];
    const int n = r.rotation.TakeHeld(held);
    EXPECT(n == 2);
    EXPECT((held[0] == B && held[1] == C) || (held[0] == C && held[1] == B));
}

/// Three columns, all players' (a third seat): each samples its own.
void ThreePlayers()
{
    Render r;
    r.UiPass(0, true);
    r.UiPass(1, true);
    r.UiPass(2, true);
    EXPECT(r.PostProcess(0, 3) == A);
    EXPECT(r.PostProcess(1, 3) == B);
    EXPECT(r.PostProcess(2, 3) == C);
    void* held[kMaxColumns];
    const int n = r.rotation.TakeHeld(held);
    EXPECT(n == 2);  // A and B; the member keeps C
    EXPECT(held[0] != C && held[1] != C && held[0] != held[1]);
}

/// The moves off: one target for every view, nothing held, nothing to release.
void Disabled()
{
    Render r;
    r.UiPass(0, false);
    r.UiPass(1, false);
    r.UiPass(2, false);
    EXPECT(r.allocated == 1 && r.member == A);
    EXPECT(r.PostProcess(0, 2) == A && r.PostProcess(1, 2) == A && r.PostProcess(2, 2) == A);
    void* held[kMaxColumns];
    EXPECT(r.rotation.TakeHeld(held) == 0);
}

/// A further column's pass before column 0's (a frame the loop started elsewhere): nothing moves for it.
void NoColumnZeroYet()
{
    Render r;
    r.UiPass(1, true);
    EXPECT(r.allocated == 1 && r.member == A);
    EXPECT(!r.rotation.Held(0) && !r.rotation.Held(1));
    void* held[kMaxColumns];
    EXPECT(r.rotation.TakeHeld(held) == 0);
}

/// A post-process for a column whose UI pass did not run samples the member as it is (nothing is swapped in) and
/// nothing is held for it.
void PostProcessWithoutPass()
{
    Render r;
    r.UiPass(0, true);
    r.UiPass(1, true);
    EXPECT(r.PostProcess(2, 2) == A);  // a mirror's column with no pass of its own: column 0's target
    EXPECT(r.PostProcess(1, 2) == B);
    void* held[kMaxColumns];
    EXPECT(r.rotation.TakeHeld(held) == 1 && held[0] == A);
}

/// Bad inputs: columns outside the range are ignored.
void BadInputs()
{
    Rotation rotation;
    EXPECT(!rotation.BeforeUiPass(-1, A, true));
    EXPECT(!rotation.BeforeUiPass(kMaxColumns, A, true));
    rotation.AfterUiPass(kMaxColumns, A);
    EXPECT(rotation.Count() == 0);
    EXPECT(rotation.BeforePostProcess(-1, A, 2) == nullptr);
    EXPECT(rotation.Target(-1) == nullptr && !rotation.Held(kMaxColumns));
}

}  // namespace

int main()
{
    OneView();
    TwoColumns();
    ThreeColumnsMirrorLast();
    ThreePlayers();
    Disabled();
    NoColumnZeroYet();
    PostProcessWithoutPass();
    BadInputs();
    std::printf(g_failures == 0 ? "target_rotation: all passed\n" : "target_rotation: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
