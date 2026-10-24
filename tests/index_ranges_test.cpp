// Host-side tests for render/index_ranges.h: the object-index ranges the occlusion-exempt test command takes.

#include "render/index_ranges.h"

#include <cstdio>

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

namespace ir = lss::render::index_ranges;

void TestParse()
{
    const auto empty = ir::Parse(L"");
    EXPECT(empty && empty->empty());
    const auto none = ir::Parse(L"  none ");
    EXPECT(none && none->empty());
    const auto upper = ir::Parse(L"None");
    EXPECT(upper && upper->empty());
    const auto one = ir::Parse(L"0 1204");
    EXPECT(one && one->size() == 1 && (*one)[0].from == 0 && (*one)[0].to == 1204);
    const auto two = ir::Parse(L" 753 903 ,2408   4817 ");
    EXPECT(two && two->size() == 2 && (*two)[0].from == 753 && (*two)[1].to == 4817);
    EXPECT(ir::Parse(L"5 5") && ir::Parse(L"5 5")->size() == 1);  // an empty range is allowed
    EXPECT(!ir::Parse(L"9 3"));                                     // backwards
    EXPECT(!ir::Parse(L"1"));                                       // one number
    EXPECT(!ir::Parse(L"1 2 3"));                                   // three
    EXPECT(!ir::Parse(L"1 2,"));                                    // an empty part
    EXPECT(!ir::Parse(L"-1 4"));                                    // a sign
    EXPECT(!ir::Parse(L"0 99999999"));                              // too large
    EXPECT(!ir::Parse(L"a b"));
    EXPECT(ir::Parse(L"0 16777215"));                              // the largest
    EXPECT(!ir::Parse(L"0 16777216"));                             // 2^24
    EXPECT(!ir::Parse(L",1 2"));                                   // a leading comma
    EXPECT(!ir::Parse(L","));
    EXPECT(!ir::Parse(L", ,"));
    EXPECT(!ir::Parse(L"none, 1 2"));
    EXPECT(!ir::Parse(L"1 2x"));
    EXPECT(!ir::Parse(L"1 \u00B2"));                               // a non-ASCII digit
    const auto tabs = ir::Parse(L"1\t2,\n3 4\r\n");
    EXPECT(tabs && tabs->size() == 2 && (*tabs)[1].from == 3 && (*tabs)[1].to == 4);
}

void TestCount()
{
    EXPECT(ir::Count({}, 100) == 0);
    EXPECT(ir::Count({{0, 10}}, 100) == 10);
    EXPECT(ir::Count({{0, 10}, {5, 20}}, 100) == 20);  // overlap counted once
    EXPECT(ir::Count({{50, 200}}, 100) == 50);         // clipped at the limit
    EXPECT(ir::Count({{30, 40}, {0, 10}}, 100) == 20);  // any order
}

}  // namespace

int main()
{
    TestParse();
    TestCount();
    if (g_failures != 0)
    {
        std::printf("index_ranges: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("index_ranges: all passed\n");
    return 0;
}
