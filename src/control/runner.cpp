#include "control/runner.h"

#include <algorithm>
#include <format>

namespace lss::control {

namespace {

template <class T>
bool Compares(Compare compare, const T& left, const T& right)
{
    switch (compare)
    {
    case Compare::Equal: return left == right;
    case Compare::NotEqual: return left != right;
    case Compare::Less: return left < right;
    case Compare::Greater: return left > right;
    case Compare::LessOrEqual: return left <= right;
    case Compare::GreaterOrEqual: return left >= right;
    }
    return false;
}

}  // namespace

Runner::Client& Runner::Find(ClientId id)
{
    for (auto& client : clients_)
    {
        if (client.id == id)
        {
            return client;
        }
    }
    clients_.push_back({id});
    return clients_.back();
}

void Runner::Send(ClientId client, std::wstring text)
{
    replies_.push_back({client, std::move(text)});
}

std::vector<Reply> Runner::TakeReplies()
{
    std::vector<Reply> out;
    out.swap(replies_);
    return out;
}

bool Runner::Idle() const noexcept
{
    return held_.empty() && std::all_of(clients_.begin(), clients_.end(), [](const Client& c) { return c.queue.empty(); });
}

void Runner::Submit(ClientId id, std::wstring_view line)
{
    auto parsed = ParseLine(registry_, line);
    if (!parsed.command)
    {
        if (!parsed.error.empty())
        {
            Send(id, std::format(L"rejected {} {} -- {}", ++Find(id).lines, line, parsed.error));
        }
        return;  // a blank line or a comment
    }
    Client&             client  = Find(id);
    const std::uint32_t number  = ++client.lines;
    const Command&      command = *parsed.command;
    switch (command.verb)
    {
    case Verb::Get:
    case Verb::Status:
    case Verb::Help:
    case Verb::Note:
        Answer(client, number, command);
        return;
    case Verb::Abort:
        for (auto& each : clients_)
        {
            DropQueue(each, L"aborted");
        }
        ReleaseHolds(0, true);
        Send(id, std::format(L"ok {} abort", number));
        return;
    default:
        client.queue.push_back({number, command});
        return;
    }
}

void Runner::Answer(const Client& client, std::uint32_t line, const Command& command)
{
    switch (command.verb)
    {
    case Verb::Get:
        if (const Entry* entry = registry_.Find(command.name); entry != nullptr && entry->get)
        {
            Send(client.id, std::format(L"value {} {}", entry->name, Format(*entry, entry->get())));
        }
        break;
    case Verb::Status:
        for (const auto& entry : registry_.Entries())
        {
            if (entry.get)
            {
                Send(client.id, std::format(L"value {} {}", entry.name, Format(entry, entry.get())));
            }
        }
        break;
    case Verb::Help:
        for (const auto& entry : registry_.Entries())
        {
            if (command.name.empty() || entry.name == command.name)
            {
                const std::wstring shape = ShapeText(entry);
                const wchar_t*     kind  = entry.set ? (entry.get ? L"" : L"(action) ") : L"(read only) ";
                Send(client.id, std::format(L"help {}{}{} -- {}{}", entry.name, shape.empty() ? L"" : L" ", shape, kind,
                                            entry.help));
            }
        }
        break;
    case Verb::Note:
        Send(client.id, std::format(L"note {} {}", line, command.text));
        return;
    default:
        break;
    }
    Send(client.id, std::format(L"ok {} {}", line, Describe(registry_, command)));
}

void Runner::ClientClosed(ClientId id)
{
    for (auto it = clients_.begin(); it != clients_.end(); ++it)
    {
        if (it->id == id)
        {
            DropQueue(*it, L"the client went away");
            clients_.erase(it);
            break;
        }
    }
    ReleaseHolds(id, false);
}

void Runner::Shutdown()
{
    for (auto& client : clients_)
    {
        DropQueue(client, L"the console closed");
    }
    ReleaseHolds(0, true);
}

void Runner::DropQueue(Client& client, std::wstring_view reason)
{
    for (const auto& queued : client.queue)
    {
        Send(client.id, std::format(L"dropped {} {} -- {}", queued.line, Describe(registry_, queued.command), reason));
    }
    client.queue.clear();
    client.waiting   = false;
    client.waitTicks = 0;
}

void Runner::ReleaseHolds(ClientId client, bool everyClient)
{
    std::vector<Hold> kept;
    for (const auto& hold : held_)
    {
        if (everyClient || hold.client == client)
        {
            holds_.ReleaseStick(hold.player);
        }
        else
        {
            kept.push_back(hold);
        }
    }
    held_.swap(kept);
}

void Runner::Fail(Client& client, const Queued& failed, const std::wstring& reason)
{
    Send(client.id, std::format(L"failed {} {} -- {}", failed.line, Describe(registry_, failed.command), reason));
    DropQueue(client, L"after a failure");
    ReleaseHolds(client.id, false);
}

bool Runner::ConditionHolds(const Command& command) const
{
    const Entry* entry = registry_.Find(command.name);
    if (entry == nullptr || !entry->get)
    {
        return false;
    }
    const Value current = entry->get();
    if (current.index() != command.value.index())
    {
        return false;
    }
    switch (current.index())
    {
    case 1: return Compares(command.compare, std::get<bool>(current), std::get<bool>(command.value));
    case 2: return Compares(command.compare, std::get<std::int64_t>(current), std::get<std::int64_t>(command.value));
    case 3: return Compares(command.compare, std::get<double>(current), std::get<double>(command.value));
    case 4: return Compares(command.compare, std::get<std::wstring>(current), std::get<std::wstring>(command.value));
    default: return false;
    }
}

void Runner::Tick(std::uint64_t)
{
    for (std::size_t i = 0; i < held_.size();)
    {
        if (--held_[i].ticks == 0)
        {
            holds_.ReleaseStick(held_[i].player);
            held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        else
        {
            ++i;
        }
    }
    for (std::size_t i = 0; i < clients_.size(); ++i)
    {
        Advance(clients_[i]);  // by index: a reply never adds a client, but keep clients_ stable here anyway
    }
}

void Runner::Advance(Client& client)
{
    if (client.waiting)
    {
        const Queued head = client.queue.front();
        if (head.command.verb == Verb::WaitUntil && ConditionHolds(head.command))
        {
            client.waiting = false;
        }
        else if (--client.waitTicks > 0)
        {
            return;
        }
        else if (head.command.verb == Verb::WaitUntil)
        {
            client.queue.pop_front();
            client.waiting = false;
            Fail(client, head, std::format(L"timed out after {} ticks", head.command.ticks));
            return;
        }
        else
        {
            client.waiting = false;
        }
        client.queue.pop_front();
        Send(client.id, std::format(L"ok {} {}", head.line, Describe(registry_, head.command)));
    }
    for (std::size_t ran = 0; ran < kMaxCommandsPerTick && !client.queue.empty(); ++ran)
    {
        if (!Run(client))
        {
            return;
        }
    }
}

bool Runner::Run(Client& client)
{
    const Queued   head    = client.queue.front();  // a copy: pop_front below ends the element
    const Command& command = head.command;
    if (command.verb == Verb::Wait || (command.verb == Verb::WaitUntil && !ConditionHolds(command)))
    {
        client.waiting   = true;
        client.waitTicks = command.ticks;
        return false;
    }
    client.queue.pop_front();

    Outcome outcome;
    switch (command.verb)
    {
    case Verb::Entry:
        if (const Entry* entry = registry_.Find(command.name); entry != nullptr && entry->set)
        {
            outcome = entry->set(command.value);
        }
        else
        {
            outcome = {false, L"no longer registered"};
        }
        break;
    case Verb::Hold:
        outcome = holds_.HoldStick(command.player, command.axes);
        if (outcome.ok)
        {
            std::erase_if(held_, [&](const Hold& hold) { return hold.player == command.player; });
            held_.push_back({client.id, command.player, command.ticks});
        }
        break;
    default:
        break;  // a wait-until that held at once
    }
    const std::wstring text = Describe(registry_, command);
    if (!outcome.ok)
    {
        Fail(client, head, outcome.detail);
        return true;
    }
    Send(client.id, outcome.detail.empty() ? std::format(L"ok {} {}", head.line, text)
                                           : std::format(L"ok {} {} -- {}", head.line, text, outcome.detail));
    return true;
}

}  // namespace lss::control
