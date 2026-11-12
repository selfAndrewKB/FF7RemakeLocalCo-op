// Host-side tests for platform::ByteContext / ByteScope on plain memory.

#include "platform/byte_scope.h"

#include <array>
#include <cstdio>
#include <cstring>

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

using lss::platform::ByteContext;
using lss::platform::ByteScope;
using Kind = ByteContext::Kind;

}  // namespace

int main()
{
    std::uint64_t              primary = 0x1111;   // a Set slot (a pointer)
    std::uint8_t               type    = 3;        // a Set slot (a byte)
    std::array<std::uint8_t, 4> focus{1, 2, 3, 4};  // an Exchange slot (state the game keeps)

    ByteContext context;
    EXPECT(!context.Defined() && !context.Open());
    const ByteContext::Range ranges[] = {
        {reinterpret_cast<std::uintptr_t>(&primary), sizeof(primary), Kind::Set},
        {reinterpret_cast<std::uintptr_t>(&type), 1, Kind::Set},
        {reinterpret_cast<std::uintptr_t>(focus.data()), 4, Kind::Exchange},
    };
    EXPECT(context.Define(ranges) && context.Count() == 3);
    const std::uint64_t two = 0x2222;
    std::memcpy(context.Value(0).data(), &two, sizeof(two));
    context.Value(1)[0] = 9;
    context.Value(2)[0] = 7;  // the player's copy starts with a 7 in its first byte

    // Open: the Set values are in, the Exchange copy is in; the game's bytes are saved.
    {
        ByteScope scope(context);
        EXPECT(scope.Held() && context.IsOpen());
        EXPECT(primary == 0x2222 && type == 9);
        EXPECT(focus[0] == 7 && focus[1] == 0 && focus[2] == 0 && focus[3] == 0);
        EXPECT(!context.Open());  // no nesting
        focus[1] = 8;             // the game writes the player's state inside
    }
    EXPECT(!context.IsOpen());
    EXPECT(primary == 0x1111 && type == 3);
    EXPECT(focus[0] == 1 && focus[1] == 2 && focus[2] == 3 && focus[3] == 4);  // the game's bytes back
    EXPECT(context.Value(2)[0] == 7 && context.Value(2)[1] == 8);               // the copy kept the write

    // A Set slot the game changed inside is left as the game wrote it, and reported.
    EXPECT(context.Open());
    primary = 0x3333;
    EXPECT(context.Close() == 1);
    EXPECT(primary == 0x3333 && type == 3);

    // Define is refused while open; Clear forgets.
    EXPECT(context.Open());
    EXPECT(!context.Define(ranges) && !context.Clear());
    context.Close();
    EXPECT(context.Clear() && !context.Defined() && !context.Open());

    // A Hold slot: untouched inside, nothing reported and the saved bytes back; written inside, the write counted and
    // the saved bytes back all the same; a wide range with one byte written goes back whole.
    std::uint64_t              leader = 0x1111;
    std::array<std::uint8_t, 8> wide{1, 2, 3, 4, 5, 6, 7, 8};
    const ByteContext::Range   held[] = {
        {reinterpret_cast<std::uintptr_t>(&leader), sizeof(leader), Kind::Hold},
        {reinterpret_cast<std::uintptr_t>(wide.data()), 8, Kind::Hold},
    };
    EXPECT(context.Define(held));
    const std::uint64_t seats = 0x4444;
    std::memcpy(context.Value(0).data(), &seats, sizeof(seats));
    context.Value(1)[0] = 9;
    EXPECT(context.Open() && leader == 0x4444 && wide[0] == 9 && wide[1] == 0);
    EXPECT(context.Close() == 0 && leader == 0x1111 && wide[0] == 1 && wide[7] == 8);
    EXPECT(context.Open());
    leader  = 0x3333;  // the game writes the slot inside the call
    wide[5] = 0x55;
    EXPECT(context.Close() == 2);
    EXPECT(leader == 0x1111 && wide[5] == 6 && wide[0] == 1);  // player 1's back, the write counted
    EXPECT(context.Clear());

    // An unreadable range opens nothing and leaves the readable ones as they were.
    const ByteContext::Range bad[] = {
        {reinterpret_cast<std::uintptr_t>(&primary), sizeof(primary), Kind::Set},
        {0x10, 8, Kind::Set},
    };
    EXPECT(context.Define(bad));
    std::memcpy(context.Value(0).data(), &two, sizeof(two));
    EXPECT(!context.Open() && primary == 0x3333 && !context.IsOpen());

    // A zero-sized range refuses the definition.
    const ByteContext::Range empty[] = {{reinterpret_cast<std::uintptr_t>(&primary), 0, Kind::Set}};
    EXPECT(!context.Define(empty) && !context.Defined());

    if (g_failures == 0)
    {
        std::printf("byte_scope_test: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
