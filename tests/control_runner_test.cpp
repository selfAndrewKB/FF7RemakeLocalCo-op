// Host-side tests for control::Runner, run without the game, against a fake registry and fake holds.

#include "control/runner.h"

#include <cstdio>
#include <string>
#include <vector>

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

/// The fake mod: a join that can be made to fail, a switch, a counter that scripts wait on.
struct Fake final : Holds
{
    bool                      joinFails = false;
    bool                      bloom     = true;
    std::int64_t              counter   = 0;
    std::vector<std::wstring> calls;
    std::vector<std::int32_t> held;  // players whose stick is held now

    Outcome HoldStick(std::int32_t player, const float (&)[4]) override
    {
        calls.push_back(L"hold " + std::to_wstring(player));
        held.push_back(player);
        return {};
    }
    void ReleaseStick(std::int32_t player) override
    {
        calls.push_back(L"release " + std::to_wstring(player));
        std::erase(held, player);
    }

    Registry MakeRegistry()
    {
        Registry registry;
        registry.Add({.name = L"join", .help = L"join", .set = [this](const Value&) {
                          calls.push_back(L"join");
                          return joinFails ? Outcome{false, L"no"} : Outcome{};
                      }});
        registry.Add({.name = L"leave", .help = L"leave", .set = [this](const Value&) {
                          calls.push_back(L"leave");
                          return Outcome{true, L"removed"};
                      }});
        registry.Add({.name = L"bloom-origin", .shape = Shape::OnOff, .help = L"bloom",
                      .set = [this](const Value& v) { bloom = std::get<bool>(v); return Outcome{}; },
                      .get = [this] { return Value{bloom}; }});
        registry.Add({.name = L"counter", .shape = Shape::Integer, .min = 0, .max = 1000, .help = L"counter",
                      .get = [this] { return Value{counter}; }});
        return registry;
    }
};

struct Harness
{
    Fake     fake;
    Registry registry = fake.MakeRegistry();
    Runner   runner{registry, fake};
    std::uint64_t tick = 0;

    std::vector<std::wstring> Replies(ClientId client = 0)
    {
        std::vector<std::wstring> out;
        for (const auto& reply : runner.TakeReplies())
        {
            if (client == 0 || reply.client == client)
            {
                out.push_back(reply.text);
            }
        }
        return out;
    }
    void Ticks(int count)
    {
        for (int i = 0; i < count; ++i)
        {
            runner.Tick(++tick);
        }
    }
};

void TestActionsAndReplies()
{
    Harness h;
    h.runner.Submit(1, L"join");
    h.runner.Submit(1, L"leave");
    EXPECT(h.Replies().empty());  // nothing runs before a tick
    h.Ticks(1);
    const auto replies = h.Replies();
    EXPECT(replies.size() == 2);
    EXPECT(replies.size() == 2 && replies[0] == L"ok 1 join" && replies[1] == L"ok 2 leave -- removed");
    EXPECT((h.fake.calls == std::vector<std::wstring>{L"join", L"leave"}));
    EXPECT(h.runner.Idle());
}

void TestRejectedAndQueries()
{
    Harness h;
    h.runner.Submit(1, L"nonsense");
    h.runner.Submit(1, L"get counter");
    h.runner.Submit(1, L"note marker");
    auto replies = h.Replies();
    EXPECT(replies.size() == 4);  // rejected; value + ok; note
    EXPECT(replies.size() == 4 && replies[0].starts_with(L"rejected 1 nonsense -- "));
    EXPECT(replies.size() == 4 && replies[1] == L"value counter 0" && replies[2] == L"ok 2 get counter");
    EXPECT(replies.size() == 4 && replies[3] == L"note 3 marker");
    h.runner.Submit(1, L"status");
    replies = h.Replies();
    EXPECT(replies.size() == 3 && replies[0] == L"value bloom-origin on" && replies[1] == L"value counter 0" &&
           replies[2] == L"ok 4 status");
    h.runner.Submit(1, L"help counter");
    replies = h.Replies();
    EXPECT(replies.size() == 2 && replies[0] == L"help counter <0..1000> -- (read only) counter");
    h.runner.Submit(1, L"help");
    replies = h.Replies();
    EXPECT(replies.size() == 5 && replies[0] == L"help join -- (action) join" &&
           replies[2] == L"help bloom-origin on|off -- bloom");
}

void TestWaitBlocksButQueriesAnswer()
{
    Harness h;
    h.runner.Submit(1, L"wait 3");
    h.runner.Submit(1, L"join");
    h.Ticks(1);
    EXPECT(h.Replies().empty());
    h.runner.Submit(2, L"get counter");  // another client's query answers during the wait
    EXPECT(h.Replies(2).size() == 2);
    h.Ticks(1);
    EXPECT(h.Replies().empty());
    h.Ticks(1);
    EXPECT(h.Replies().empty());
    h.Ticks(1);  // a wait of N ticks ends N ticks after the tick it started on
    const auto replies = h.Replies();
    EXPECT(replies.size() == 2 && replies[0] == L"ok 1 wait 3" && replies[1] == L"ok 2 join");
}

void TestQueriesWaitTheirTurn()
{
    Harness h;
    // A script sent whole: the note and the get stand behind the wait-until and the join, in order.
    h.runner.Submit(1, L"note before");
    EXPECT((h.Replies() == std::vector<std::wstring>{L"note 1 before"}));  // nothing pending: at once
    h.runner.Submit(1, L"wait-until counter >= 2 10");
    h.runner.Submit(1, L"note after the wait");
    h.runner.Submit(1, L"get counter");
    h.runner.Submit(1, L"join");
    h.runner.Submit(1, L"status");
    EXPECT(h.Replies().empty());
    h.Ticks(1);
    EXPECT(h.Replies().empty());
    h.fake.counter = 2;
    h.Ticks(1);
    const auto replies = h.Replies();
    EXPECT((replies == std::vector<std::wstring>{L"ok 2 wait-until counter >= 2 10", L"note 3 after the wait",
                                                 L"value counter 2", L"ok 4 get counter", L"ok 5 join",
                                                 L"value bloom-origin on", L"value counter 2", L"ok 6 status"}));
    EXPECT(h.runner.Idle());

    // A failure ahead of a queued query drops it like any command.
    h.fake.joinFails = true;
    h.runner.Submit(1, L"join");
    h.runner.Submit(1, L"get counter");
    h.Ticks(1);
    EXPECT((h.Replies() == std::vector<std::wstring>{L"failed 7 join -- no", L"dropped 8 get counter -- after a failure"}));

    // So does a closed connection.
    h.runner.Submit(1, L"wait 5");
    h.runner.Submit(1, L"note never");
    h.runner.ClientClosed(1);
    EXPECT((h.Replies() == std::vector<std::wstring>{L"dropped 9 wait 5 -- the client went away",
                                                     L"dropped 10 note never -- the client went away"}));
}

void TestWaitUntil()
{
    Harness h;
    h.runner.Submit(1, L"wait-until counter >= 2 5");
    h.runner.Submit(1, L"join");
    h.Ticks(2);
    EXPECT(h.Replies().empty());
    h.fake.counter = 2;
    h.Ticks(1);
    auto replies = h.Replies();
    EXPECT(replies.size() == 2 && replies[0] == L"ok 1 wait-until counter >= 2 5" && replies[1] == L"ok 2 join");

    // Already true: completes on the tick it runs.
    h.runner.Submit(1, L"wait-until bloom-origin == on");
    h.Ticks(1);
    replies = h.Replies();
    EXPECT(replies.size() == 1 && replies[0] == L"ok 3 wait-until bloom-origin == on 600");

    // Times out: fails, and the commands queued behind it are dropped.
    h.runner.Submit(1, L"wait-until counter == 99 3");
    h.runner.Submit(1, L"leave");
    h.Ticks(3);
    EXPECT(h.Replies().empty());
    h.Ticks(1);
    replies = h.Replies();
    EXPECT(replies.size() == 2 && replies[0] == L"failed 4 wait-until counter == 99 3 -- timed out after 3 ticks" &&
           replies[1] == L"dropped 5 leave -- after a failure");
    EXPECT(h.runner.Idle());
}

void TestHoldsRunAlongsideAndRelease()
{
    Harness h;
    h.runner.Submit(1, L"hold 2 stick 0 1 0 0 2");
    h.runner.Submit(1, L"join");
    h.Ticks(1);
    auto replies = h.Replies();
    EXPECT(replies.size() == 2 && replies[0] == L"ok 1 hold 2 stick 0 1 0 0 2" && replies[1] == L"ok 2 join");
    EXPECT((h.fake.held == std::vector<std::int32_t>{2}));
    EXPECT(!h.runner.Idle());
    h.Ticks(2);
    EXPECT(h.fake.held.empty());
    EXPECT(h.runner.Idle());

    // A failure releases the client's holds and drops what it queued.
    h.fake.joinFails = true;
    h.runner.Submit(1, L"hold 1 stick 1 0 0 0 100");
    h.runner.Submit(1, L"join");
    h.runner.Submit(1, L"leave");
    h.Ticks(1);
    replies = h.Replies();
    EXPECT(replies.size() == 3 && replies[1] == L"failed 4 join -- no" && replies[2] == L"dropped 5 leave -- after a failure");
    EXPECT(h.fake.held.empty());
}

void TestClientsAreSeparate()
{
    Harness h;
    h.fake.joinFails = true;
    h.runner.Submit(1, L"hold 1 stick 1 0 0 0 100");
    h.runner.Submit(2, L"hold 2 stick 1 0 0 0 100");
    h.runner.Submit(1, L"join");    // fails
    h.runner.Submit(2, L"leave");   // another client's command still runs
    h.runner.Submit(1, L"leave");   // dropped
    h.Ticks(1);
    const auto one = h.Replies(1);
    EXPECT(one.size() == 3 && one[1] == L"failed 2 join -- no" && one[2] == L"dropped 3 leave -- after a failure");
    EXPECT((h.fake.held == std::vector<std::int32_t>{2}));  // client 2's hold stays

    h.runner.ClientClosed(2);
    EXPECT(h.fake.held.empty());
}

void TestClientsQueuesAreIndependent()
{
    Harness h;
    h.runner.Submit(1, L"wait 100");
    h.runner.Submit(1, L"leave");
    h.runner.Submit(2, L"join");  // not behind client 1's wait
    h.Ticks(1);
    const auto two = h.Replies(2);
    EXPECT(two.size() == 1 && two[0] == L"ok 1 join");
    EXPECT(h.Replies(1).empty());
    h.runner.ClientClosed(1);  // its wait and the leave behind it are dropped
    const auto one = h.Replies(1);
    EXPECT(one.size() == 2 && one[0] == L"dropped 1 wait 100 -- the client went away" &&
           one[1] == L"dropped 2 leave -- the client went away");
    EXPECT(h.runner.Idle());
}

void TestAbortAndBudget()
{
    Harness h;
    for (int i = 0; i < 40; ++i)
    {
        h.runner.Submit(1, L"join");
    }
    h.Ticks(1);
    EXPECT(h.Replies().size() == Runner::kMaxCommandsPerTick);
    h.runner.Submit(2, L"wait 50");
    h.runner.Submit(1, L"abort");  // every client's queue: client 1's 8 left, client 2's wait
    const auto replies = h.Replies();
    EXPECT(replies.size() == 10 && replies.back() == L"ok 41 abort");  // 8 dropped, client 2's wait, the abort's ok
    EXPECT(h.Replies(2).empty() && h.runner.Idle());
    EXPECT(h.runner.Idle());
}

}  // namespace

int main()
{
    TestActionsAndReplies();
    TestRejectedAndQueries();
    TestWaitBlocksButQueriesAnswer();
    TestQueriesWaitTheirTurn();
    TestWaitUntil();
    TestHoldsRunAlongsideAndRelease();
    TestClientsAreSeparate();
    TestClientsQueuesAreIndependent();
    TestAbortAndBudget();
    if (g_failures == 0)
    {
        std::printf("control_runner_test: all passed\n");
        return 0;
    }
    std::printf("control_runner_test: %d failure(s)\n", g_failures);
    return 1;
}
