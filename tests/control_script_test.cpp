// Host-side tests for the control language (control/script.h) against a fake registry.

#include "control/script.h"

#include <cstdio>
#include <string>

namespace {

using namespace lss::control;

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

Registry FakeRegistry()
{
    Registry registry;
    registry.Add({.name = L"join", .shape = Shape::None, .help = L"player 2 joins", .set = [](const Value&) { return Outcome{}; }});
    registry.Add({.name = L"bloom-origin", .shape = Shape::OnOff, .help = L"the bloom fix",
                  .set = [](const Value&) { return Outcome{}; }, .get = [] { return Value{true}; }});
    registry.Add({.name = L"showflags", .shape = Shape::Hex, .max = 0xFFFF, .help = L"forced flags",
                  .set = [](const Value&) { return Outcome{}; }, .get = [] { return Value{std::int64_t{0x100A}}; }});
    registry.Add({.name = L"log-level", .shape = Shape::Word, .words = {L"info", L"debug", L"trace"}, .help = L"the log level",
                  .set = [](const Value&) { return Outcome{}; }, .get = [] { return Value{std::wstring(L"info")}; }});
    registry.Add({.name = L"fov", .shape = Shape::Float, .minFloat = 0.5, .maxFloat = 2.0, .help = L"a factor",
                  .set = [](const Value&) { return Outcome{}; }, .get = [] { return Value{1.0}; }});
    registry.Add({.name = L"console", .shape = Shape::Text, .help = L"an engine console command",
                  .set = [](const Value&) { return Outcome{}; }});
    registry.Add({.name = L"tick", .shape = Shape::Integer, .min = 0, .max = INT64_MAX, .help = L"the engine tick",
                  .get = [] { return Value{std::int64_t{7}}; }});
    registry.Add({.name = L"count", .shape = Shape::Integer, .min = 1, .max = 10, .help = L"a count",
                  .set = [](const Value&) { return Outcome{}; }, .get = [] { return Value{std::int64_t{3}}; }});
    return registry;
}

bool Rejected(const Registry& registry, const wchar_t* line)
{
    const auto parsed = ParseLine(registry, line);
    return !parsed.command && !parsed.error.empty();
}

std::wstring RoundTrip(const Registry& registry, const wchar_t* line)
{
    const auto parsed = ParseLine(registry, line);
    return parsed.command ? Describe(registry, *parsed.command) : L"<" + parsed.error + L">";
}

void TestRegistry()
{
    Registry registry;
    EXPECT(registry.Add({.name = L"a", .set = [](const Value&) { return Outcome{}; }}));
    EXPECT(!registry.Add({.name = L"a", .set = [](const Value&) { return Outcome{}; }}));  // taken
    EXPECT(!registry.Add({.name = L"b"}));                                                    // neither set nor get
    EXPECT(!registry.Add({.name = L"c", .shape = Shape::Word, .set = [](const Value&) { return Outcome{}; }}));  // no words
    EXPECT(!registry.Add({.name = L"d d", .set = [](const Value&) { return Outcome{}; }}));  // a space
    EXPECT(!registry.Add({.name = L"e", .shape = Shape::Integer, .min = 5, .max = 1, .set = [](const Value&) { return Outcome{}; }}));
    EXPECT(registry.Rejected().size() == 5);
    EXPECT(registry.Find(L"a") != nullptr && registry.Find(L"b") == nullptr);
}

void TestBlankAndUnknown()
{
    const auto registry = FakeRegistry();
    const auto blank    = ParseLine(registry, L"   ");
    EXPECT(!blank.command && blank.error.empty());
    const auto comment = ParseLine(registry, L"# a comment");
    EXPECT(!comment.command && comment.error.empty());
    EXPECT(Rejected(registry, L"frobnicate"));
    EXPECT(Rejected(registry, L"status now"));
    EXPECT(Rejected(registry, L"abort 1"));
}

void TestEntries()
{
    const auto registry = FakeRegistry();
    EXPECT(RoundTrip(registry, L"join") == L"join");
    EXPECT(Rejected(registry, L"join now"));
    EXPECT(RoundTrip(registry, L"bloom-origin on") == L"bloom-origin on");
    EXPECT(RoundTrip(registry, L"  bloom-origin   off ") == L"bloom-origin off");
    EXPECT(Rejected(registry, L"bloom-origin maybe"));
    EXPECT(Rejected(registry, L"bloom-origin"));
    EXPECT(Rejected(registry, L"bloom-origin on on"));
    EXPECT(RoundTrip(registry, L"showflags 100a") == L"showflags 100A");
    EXPECT(RoundTrip(registry, L"showflags 0") == L"showflags 0");
    EXPECT(Rejected(registry, L"showflags 10000"));
    EXPECT(Rejected(registry, L"showflags -1"));
    EXPECT(Rejected(registry, L"showflags zz"));
    EXPECT(RoundTrip(registry, L"log-level debug") == L"log-level debug");
    EXPECT(Rejected(registry, L"log-level loud"));
    EXPECT(RoundTrip(registry, L"fov 1.5") == L"fov 1.5");
    EXPECT(Rejected(registry, L"fov 3"));
    EXPECT(Rejected(registry, L"fov nan"));
    EXPECT(RoundTrip(registry, L"console r.SSR.Quality 0 ") == L"console r.SSR.Quality 0");
    EXPECT(Rejected(registry, L"console"));
    EXPECT(RoundTrip(registry, L"count 10") == L"count 10");
    EXPECT(Rejected(registry, L"count 11"));
    EXPECT(Rejected(registry, L"count 0"));
    EXPECT(Rejected(registry, L"tick 5"));  // read only
}

void TestQueries()
{
    const auto registry = FakeRegistry();
    EXPECT(RoundTrip(registry, L"get tick") == L"get tick");
    EXPECT(RoundTrip(registry, L"get bloom-origin") == L"get bloom-origin");
    EXPECT(Rejected(registry, L"get join"));  // an action has no value
    EXPECT(Rejected(registry, L"get nothing"));
    EXPECT(Rejected(registry, L"get"));
    EXPECT(RoundTrip(registry, L"status") == L"status");
    EXPECT(RoundTrip(registry, L"help") == L"help");
    EXPECT(RoundTrip(registry, L"help fov") == L"help fov");
    EXPECT(Rejected(registry, L"help nothing"));
    EXPECT(RoundTrip(registry, L"note hello   world") == L"note hello   world");
    EXPECT(Rejected(registry, L"note"));
}

void TestWaits()
{
    const auto registry = FakeRegistry();
    EXPECT(RoundTrip(registry, L"wait 120") == L"wait 120");
    EXPECT(Rejected(registry, L"wait 0"));
    EXPECT(Rejected(registry, L"wait 36001"));
    EXPECT(Rejected(registry, L"wait"));
    EXPECT(Rejected(registry, L"wait 1 2"));
    EXPECT(RoundTrip(registry, L"wait-until tick >= 100") == L"wait-until tick >= 100 600");
    EXPECT(RoundTrip(registry, L"wait-until tick == 100 900") == L"wait-until tick == 100 900");
    EXPECT(RoundTrip(registry, L"wait-until bloom-origin == on") == L"wait-until bloom-origin == on 600");
    EXPECT(RoundTrip(registry, L"wait-until log-level != trace") == L"wait-until log-level != trace 600");
    EXPECT(RoundTrip(registry, L"wait-until showflags == 1008") == L"wait-until showflags == 1008 600");
    EXPECT(Rejected(registry, L"wait-until bloom-origin < on"));   // on/off is not ordered
    EXPECT(Rejected(registry, L"wait-until log-level > info"));
    EXPECT(Rejected(registry, L"wait-until join == 1"));           // no value to read
    EXPECT(Rejected(registry, L"wait-until tick ~ 1"));
    EXPECT(Rejected(registry, L"wait-until tick == x"));
    EXPECT(Rejected(registry, L"wait-until tick == 1 0"));
    EXPECT(Rejected(registry, L"wait-until tick == 1 2 3"));
}

void TestHold()
{
    const auto registry = FakeRegistry();
    EXPECT(RoundTrip(registry, L"hold 2 stick 0 1 0 0 120") == L"hold 2 stick 0 1 0 0 120");
    EXPECT(RoundTrip(registry, L"hold 1 stick -1 0.5 0 -0.25 1") == L"hold 1 stick -1 0.5 0 -0.25 1");
    EXPECT(Rejected(registry, L"hold 3 stick 0 0 0 0 1"));
    EXPECT(Rejected(registry, L"hold 2 button 0 0 0 0 1"));
    EXPECT(Rejected(registry, L"hold 2 stick 0 2 0 0 1"));
    EXPECT(Rejected(registry, L"hold 2 stick 0 0 0 0 0"));
    EXPECT(Rejected(registry, L"hold 2 stick 0 0 0 0"));
}

void TestShapeText()
{
    const auto registry = FakeRegistry();
    EXPECT(ShapeText(*registry.Find(L"join")).empty());
    EXPECT(ShapeText(*registry.Find(L"bloom-origin")) == L"on|off");
    EXPECT(ShapeText(*registry.Find(L"showflags")) == L"<hex up to FFFF>");
    EXPECT(ShapeText(*registry.Find(L"log-level")) == L"info|debug|trace");
    EXPECT(ShapeText(*registry.Find(L"count")) == L"<1..10>");
    EXPECT(ShapeText(*registry.Find(L"console")) == L"<text>");
}

}  // namespace

int main()
{
    TestRegistry();
    TestBlankAndUnknown();
    TestEntries();
    TestQueries();
    TestWaits();
    TestHold();
    TestShapeText();
    if (g_failures == 0)
    {
        std::printf("control_script_test: all passed\n");
        return 0;
    }
    std::printf("control_script_test: %d failure(s)\n", g_failures);
    return 1;
}
