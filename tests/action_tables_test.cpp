// Host-side tests for players/action_tables.h, on a fake input owner laid out like the game's.
//
// Two TSets of 40-byte elements (one with a free index, one whose bits live in a secondary allocation), the
// d-pad field, and a view: built zero, exchanged in and out, advanced like the game's tick.

#include "players/action_tables.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

namespace at = lss::players::action_tables;

constexpr at::Layout kLayout{.raw = 0xE8, .ui = 0x138, .dpad = 0x188};

/// A table of `num` elements with `freeIndex` free (-1 for none), bits inline or in `secondary`.
struct FakeTable
{
    std::vector<std::uint8_t> elements;
    std::array<std::uint32_t, 4> secondary{};

    void Place(std::uint8_t* set, std::int32_t num, std::int32_t freeIndex, bool useSecondary, std::uint64_t firstName)
    {
        elements.assign(static_cast<std::size_t>(num) * at::kElementBytes, 0);
        for (std::int32_t i = 0; i < num; ++i)
        {
            const std::uint64_t name = firstName + static_cast<std::uint64_t>(i);
            std::memcpy(elements.data() + i * at::kElementBytes, &name, sizeof(name));
        }
        std::uint8_t* data = elements.data();
        std::memcpy(set + at::kSetData, &data, sizeof(data));
        std::memcpy(set + at::kSetNum, &num, sizeof(num));
        std::uint32_t bits = 0;
        for (std::int32_t i = 0; i < num; ++i)
        {
            if (i != freeIndex)
            {
                bits |= 1u << i;
            }
        }
        std::uint32_t* words = useSecondary ? secondary.data() : reinterpret_cast<std::uint32_t*>(set + at::kSetBits);
        words[0]             = bits;
        const std::uint32_t* secondaryPtr = useSecondary ? secondary.data() : nullptr;
        std::memcpy(set + at::kSetSecondary, &secondaryPtr, sizeof(secondaryPtr));
        std::memcpy(set + at::kSetNumBits, &num, sizeof(num));
        const std::int32_t numFree = freeIndex >= 0 ? 1 : 0;
        std::memcpy(set + at::kSetNumFree, &numFree, sizeof(numFree));
    }

    std::uint8_t* State(std::int32_t i) { return elements.data() + i * at::kElementBytes + at::kElementState; }
};

}  // namespace

int main()
{
    std::array<std::uint8_t, 0x200> owner{};
    FakeTable                       raw, ui;
    raw.Place(owner.data() + kLayout.raw, 3, 1, false, 0x1000);  // index 1 free, bits inline
    ui.Place(owner.data() + kLayout.ui, 2, -1, true, 0x2000);     // bits in a secondary allocation
    owner[kLayout.dpad] = 0x05;

    // Live state: raw[0] held with its latch set, raw[2] previously held, ui[1] repeat.
    raw.State(0)[1] = 1;
    raw.State(0)[3] = 1;
    raw.State(2)[0] = 1;
    ui.State(1)[2]  = 1;

    at::View view;
    EXPECT(at::Build(owner.data(), kLayout, view));
    EXPECT(view.rawShape.names.size() == 3 && view.rawShape.names[1] == 0 && view.rawShape.names[2] == 0x1002);
    EXPECT(view.uiShape.names.size() == 2 && view.uiShape.names[1] == 0x2001);
    EXPECT(at::Fits(owner.data(), kLayout, view));

    // Exchange in: the live bytes read zero (the view), the view holds the live state; the free index is untouched.
    raw.State(1)[1] = 7;  // garbage in the free element, must stay
    at::Exchange(owner.data(), kLayout, view);
    EXPECT(raw.State(0)[1] == 0 && raw.State(0)[3] == 0 && raw.State(2)[0] == 0 && ui.State(1)[2] == 0);
    EXPECT(raw.State(1)[1] == 7);
    EXPECT(view.raw[0][1] == 1 && view.raw[0][3] == 1 && view.raw[2][0] == 1 && view.ui[1][2] == 1);
    EXPECT(owner[kLayout.dpad] == 0 && view.dpad[0] == 0x05);

    // The game writes the pad's press into the live (exchanged) bytes.
    raw.State(2)[1] = 1;
    raw.State(2)[3] = 1;
    owner[kLayout.dpad] = 0x02;

    // Exchange out: the live state is back as it was, the view keeps the pad's press.
    at::Exchange(owner.data(), kLayout, view);
    EXPECT(raw.State(0)[1] == 1 && raw.State(0)[3] == 1 && raw.State(2)[0] == 1 && raw.State(2)[1] == 0 && ui.State(1)[2] == 1);
    EXPECT(owner[kLayout.dpad] == 0x05 && view.dpad[0] == 0x02);
    EXPECT(view.raw[2][1] == 1 && view.raw[2][3] == 1 && view.raw[0][1] == 0);

    // The tick's advance on the view: a latched press keeps prev = 0 this frame ("pressed"), then prev = held.
    at::Advance(view);
    EXPECT(view.raw[2][0] == 0 && view.raw[2][1] == 1 && view.raw[2][3] == 0);
    at::Advance(view);
    EXPECT(view.raw[2][0] == 1 && view.raw[2][1] == 1);

    // A table that changed shape no longer fits; a rebuild starts from zero.
    const std::int32_t more = 4;
    std::memcpy(owner.data() + kLayout.raw + at::kSetNum, &more, sizeof(more));
    EXPECT(!at::Fits(owner.data(), kLayout, view));
    raw.Place(owner.data() + kLayout.raw, 4, -1, false, 0x1000);
    EXPECT(at::Build(owner.data(), kLayout, view) && view.raw.size() == 4 && view.raw[2][1] == 0);

    // An empty table refuses.
    const std::int32_t none = 0;
    std::memcpy(owner.data() + kLayout.ui + at::kSetNum, &none, sizeof(none));
    EXPECT(!at::Build(owner.data(), kLayout, view) && !view.built);

    if (g_failures == 0)
    {
        std::printf("action_tables_test: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
