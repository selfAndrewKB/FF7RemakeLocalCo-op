// Host-side tests for the record of a moved desktop: it reads back as written, a damaged one is called damaged, only a desktop
// that still reads as the mod left it is put back, nobody undoes the row of a game still running, and a record is given up only
// on a definite answer.

#include "views/row_journal.h"

#include <cstdio>
#include <vector>

namespace {

using namespace lss::views::row_journal;

int g_failures = 0;

#define EXPECT(condition)                                                \
    do                                                                   \
    {                                                                    \
        if (!(condition))                                                \
        {                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition); \
            ++g_failures;                                                \
        }                                                                \
    } while (false)

// Monitors as the record names them: by device path, lower-cased.
const wchar_t* const kLg    = L"\\\\?\\display#gsm774b#5&21434701&1&uid4353#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
const wchar_t* const kDell  = L"\\\\?\\display#dela199#5&21434701&1&uid4355#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
const wchar_t* const kDeck  = L"\\\\?\\display#smkd1ce#1&15ecd195&1&uid256#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
const wchar_t* const kOther = L"\\\\?\\display#smkd1ce#1&15ecd195&1&uid257#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";

const std::vector<Entry> kEntries{{kLg, 0, 0, 0, 0}, {kDell, -2560, 0, 0, 1440}, {kDeck, 6000, -5, 2560, 1440}};
const Record             kRecord{4242, 133000000000000000ull, kEntries};
const std::vector<Seen>  kLeft{{kDeck, 2560, 1440}, {kLg, 0, 0}, {kDell, 0, 1440}};
const std::vector<Seen>  kBefore{{kLg, 0, 0}, {kDell, -2560, 0}, {kDeck, 6000, -5}};

void RoundTrip()
{
    const auto read = Parse(Format(kRecord));
    EXPECT(read.has_value() && *read == kRecord);
    // No monitors listed still reads; line endings and a missing last newline do not matter.
    EXPECT(Parse(L"owner\t1\t2\n").has_value());
    EXPECT(Parse(L"owner\t1\t2\r\n\\\\.\\D1\t0\t0\t1\t1\r\n").has_value());
    const auto noNewline = Parse(L"owner\t1\t2\n\\\\.\\D1\t-5\t0\t1\t1");
    EXPECT(noNewline.has_value() && noNewline->entries.size() == 1 && noNewline->entries[0].fromX == -5);
}

void DamagedRecordIsDamaged()
{
    const wchar_t* const damaged[] = {
        L"",                                           // nothing at all: not even whose it is
        L"\\\\.\\D1\t0\t0\t0\t0\n",                    // no owner line
        L"owner\t1\n",                                 // an owner short
        L"owner\tx\t2\n",                              // an owner not a number
        L"owner\t-1\t2\n",                             // a process number below zero
        L"owner\t1\t2\t3\n",                           // a field too many
        L"owner\t1\t2\n\\\\.\\D1\t0\t0\t0\n",          // a number short
        L"owner\t1\t2\n\\\\.\\D1\t0\t0\t0\t0\t9\n",    // a number too many
        L"owner\t1\t2\n\\\\.\\D1\t0\t0\t0\t0\t\n",     // a stray tab
        L"owner\t1\t2\n\\\\.\\D1\t0\tx\t0\t0\n",       // not a number
        L"owner\t1\t2\n\\\\.\\D1\t0\t-\t0\t0\n",       // a lone minus
        L"owner\t1\t2\n\\\\.\\D1\t0\t--5\t0\t0\n",     // a double minus
        L"owner\t1\t2\n\\\\.\\D1\t0\t99999999999\t0\t0\n",  // beyond the range
        L"owner\t1\t2\n\\\\.\\D1\t0\t1234567890123456789012345\t0\t0\n",  // absurdly long
        L"owner\t1\t2\n\t0\t0\t0\t0\n",                // no device
        L"owner\t1\t2\n\\\\.\\D1\t0\t0\t0\t0\n\\\\.\\D2\tbad\n",  // one bad line voids the rest
    };
    for (const wchar_t* text : damaged)
    {
        EXPECT(!Parse(text).has_value());
    }
}

void Decisions()
{
    EXPECT(Decide(kEntries, kLeft) == Verdict::PutBack);
    EXPECT(Decide(kEntries, kBefore) == Verdict::AlreadyBack);

    // Partway: Windows was still applying the move, or applied part of it. Ours to undo, not the player's doing.
    EXPECT(Decide(kEntries, std::vector<Seen>{{kLg, 0, 0}, {kDell, 0, 1440}, {kDeck, 6000, -5}}) == Verdict::PutBack);

    // The player moved a monitor, a monitor went, a monitor came: none is the mod's to undo.
    EXPECT(Decide(kEntries, std::vector<Seen>{{kLg, 0, 0}, {kDell, 100, 1440}, {kDeck, 2560, 1440}}) == Verdict::Changed);
    EXPECT(Decide(kEntries, std::vector<Seen>{{kLg, 0, 0}, {kDell, 0, 1440}}) == Verdict::Changed);
    auto more = kLeft;
    more.push_back({kOther, 5120, 1440});
    EXPECT(Decide(kEntries, more) == Verdict::Changed);

    // A desktop that could not be read proves nothing.
    EXPECT(Decide(kEntries, std::vector<Seen>{}) == Verdict::Unknown);

    // An empty record has nothing to put back.
    EXPECT(Decide(std::vector<Entry>{}, kLeft) == Verdict::AlreadyBack);
}

void Owners()
{
    // The guard of the game that made it acts; the guard of another game leaves it, however that game is doing.
    EXPECT(Judge(kRecord, kLeft, 4242, false) == Verdict::PutBack);
    EXPECT(Judge(kRecord, kLeft, 4242, true) == Verdict::PutBack);
    EXPECT(Judge(kRecord, kLeft, 7, false) == Verdict::NotOurs);

    // A later start acts only when the game that made it is gone.
    EXPECT(Judge(kRecord, kLeft, 0, false) == Verdict::PutBack);
    EXPECT(Judge(kRecord, kLeft, 0, true) == Verdict::NotOurs);

    EXPECT(Judge(std::nullopt, kLeft, 4242, false) == Verdict::Damaged);
    EXPECT(Judge(std::nullopt, kLeft, 0, true) == Verdict::Damaged);
    EXPECT(Judge(kRecord, std::vector<Seen>{}, 4242, false) == Verdict::Unknown);
}

}  // namespace

/// A monitor's id in a log line: its model and its place on the adapter; any other text as it is.
void ShortNames()
{
    using lss::views::row_journal::ShortName;
    EXPECT(ShortName(kLg) == L"gsm774b uid4353");
    EXPECT(ShortName(kDeck) == L"smkd1ce uid256" && ShortName(kOther) == L"smkd1ce uid257");
    EXPECT(ShortName(L"\\\\.\\display5") == L"\\\\.\\display5");  // a monitor whose path could not be read
    EXPECT(ShortName(L"a#b#c#d") == L"b c" && ShortName(L"a#b#c") == L"a#b#c" && ShortName(L"").empty());
}

int main()
{
    ShortNames();
    RoundTrip();
    DamagedRecordIsDamaged();
    Decisions();
    Owners();
    std::printf(g_failures == 0 ? "row journal: all passed\n" : "row journal: %d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
