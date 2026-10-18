#include "control/script.h"

#include <cmath>
#include <cwchar>
#include <format>
#include <vector>

namespace lss::control {

namespace {

constexpr std::wstring_view kSpace = L" \t\r\n";

std::wstring_view Trim(std::wstring_view text)
{
    const auto first = text.find_first_not_of(kSpace);
    if (first == std::wstring_view::npos)
    {
        return {};
    }
    return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
}

std::vector<std::wstring_view> Words(std::wstring_view text)
{
    std::vector<std::wstring_view> words;
    while (!(text = Trim(text)).empty())
    {
        const auto end = text.find_first_of(kSpace);
        words.push_back(text.substr(0, end));
        text = end == std::wstring_view::npos ? std::wstring_view{} : text.substr(end);
    }
    return words;
}

/// The text after the first `count` words, trimmed.
std::wstring_view After(std::wstring_view line, std::size_t count)
{
    line = Trim(line);
    for (std::size_t i = 0; i < count && !line.empty(); ++i)
    {
        const auto end = line.find_first_of(kSpace);
        line           = end == std::wstring_view::npos ? std::wstring_view{} : Trim(line.substr(end));
    }
    return line;
}

std::optional<std::uint32_t> ParseTicks(std::wstring_view word)
{
    const std::wstring text(word);
    wchar_t*           end   = nullptr;
    const auto         value = text.empty() ? 0ul : std::wcstoul(text.c_str(), &end, 10);
    if (text.empty() || text[0] == L'-' || end != text.c_str() + text.size() || value == 0 || value > kMaxTicks)
    {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

std::optional<float> ParseAxis(std::wstring_view word)
{
    const std::wstring text(word);
    wchar_t*           end   = nullptr;
    const float        value = text.empty() ? 0.0f : std::wcstof(text.c_str(), &end);
    if (text.empty() || end != text.c_str() + text.size() || !std::isfinite(value) || value < -1.0f || value > 1.0f)
    {
        return std::nullopt;
    }
    return value;
}

std::optional<Compare> ParseCompare(std::wstring_view word)
{
    if (word == L"==") return Compare::Equal;
    if (word == L"!=") return Compare::NotEqual;
    if (word == L"<") return Compare::Less;
    if (word == L">") return Compare::Greater;
    if (word == L"<=") return Compare::LessOrEqual;
    if (word == L">=") return Compare::GreaterOrEqual;
    return std::nullopt;
}

ParsedLine Error(std::wstring message)
{
    return {std::nullopt, std::move(message)};
}

ParsedLine Done(Command command)
{
    return {std::move(command), {}};
}

const wchar_t* kTicksMessage = L"the ticks are a whole number from 1 to 36000";

}  // namespace

std::wstring_view CompareText(Compare compare) noexcept
{
    switch (compare)
    {
    case Compare::Equal: return L"==";
    case Compare::NotEqual: return L"!=";
    case Compare::Less: return L"<";
    case Compare::Greater: return L">";
    case Compare::LessOrEqual: return L"<=";
    case Compare::GreaterOrEqual: return L">=";
    }
    return L"?";
}

std::wstring ShapeText(const Entry& entry)
{
    switch (entry.shape)
    {
    case Shape::None: return {};
    case Shape::OnOff: return L"on|off";
    case Shape::Integer: return std::format(L"<{}..{}>", entry.min, entry.max);
    case Shape::Hex: return std::format(L"<hex up to {:X}>", entry.max);
    case Shape::Word:
    {
        std::wstring out;
        for (const auto& word : entry.words)
        {
            out += out.empty() ? word : L"|" + word;
        }
        return out;
    }
    case Shape::Float: return std::format(L"<{}..{}>", entry.minFloat, entry.maxFloat);
    case Shape::Text: return L"<text>";
    }
    return L"?";
}

ParsedLine ParseLine(const Registry& registry, std::wstring_view line)
{
    const auto words = Words(line);
    if (words.empty() || words[0].starts_with(L"#"))
    {
        return {};
    }
    const auto& verb = words[0];
    Command     command;

    if (verb == L"status" || verb == L"abort")
    {
        if (words.size() != 1)
        {
            return Error(std::format(L"{} takes no argument", verb));
        }
        command.verb = verb == L"status" ? Verb::Status : Verb::Abort;
        return Done(command);
    }
    if (verb == L"help")
    {
        if (words.size() > 2 || (words.size() == 2 && registry.Find(words[1]) == nullptr))
        {
            return Error(L"help takes no argument, or one registered command");
        }
        command.verb = Verb::Help;
        command.name = words.size() == 2 ? std::wstring(words[1]) : std::wstring{};
        return Done(command);
    }
    if (verb == L"get")
    {
        const Entry* entry = words.size() == 2 ? registry.Find(words[1]) : nullptr;
        if (entry == nullptr || !entry->get)
        {
            return Error(L"get takes one registered command that can be read");
        }
        command.verb = Verb::Get;
        command.name = entry->name;
        return Done(command);
    }
    if (verb == L"note")
    {
        const auto text = After(line, 1);
        if (text.empty())
        {
            return Error(L"note needs text");
        }
        command.verb = Verb::Note;
        command.text = std::wstring(text);
        return Done(command);
    }
    if (verb == L"wait")
    {
        const auto ticks = words.size() == 2 ? ParseTicks(words[1]) : std::nullopt;
        if (!ticks)
        {
            return Error(std::format(L"wait: {}", kTicksMessage));
        }
        command.verb  = Verb::Wait;
        command.ticks = *ticks;
        return Done(command);
    }
    if (verb == L"wait-until")
    {
        const Entry* entry = words.size() >= 4 ? registry.Find(words[1]) : nullptr;
        if (entry == nullptr || !entry->get || words.size() > 5)
        {
            return Error(L"wait-until takes <readable command> <op> <value> [<ticks>]");
        }
        const auto compare = ParseCompare(words[2]);
        if (!compare)
        {
            return Error(L"wait-until: the op is one of == != < > <= >=");
        }
        const bool ordered = entry->shape == Shape::Integer || entry->shape == Shape::Hex || entry->shape == Shape::Float;
        if (!ordered && *compare != Compare::Equal && *compare != Compare::NotEqual)
        {
            return Error(std::format(L"wait-until: {} compares with == or != only", entry->name));
        }
        const auto value = ParseValue(*entry, words[3]);
        if (!value.value)
        {
            return Error(L"wait-until: " + value.error);
        }
        command.ticks = kDefaultWaitUntilTicks;
        if (words.size() == 5)
        {
            const auto ticks = ParseTicks(words[4]);
            if (!ticks)
            {
                return Error(std::format(L"wait-until: {}", kTicksMessage));
            }
            command.ticks = *ticks;
        }
        command.verb    = Verb::WaitUntil;
        command.name    = entry->name;
        command.compare = *compare;
        command.value   = *value.value;
        return Done(command);
    }
    if (verb == L"hold")
    {
        if (words.size() != 8 || (words[1] != L"1" && words[1] != L"2") || words[2] != L"stick")
        {
            return Error(L"hold takes <player 1|2> stick <lx> <ly> <rx> <ry> <ticks>");
        }
        command.verb   = Verb::Hold;
        command.player = words[1] == L"1" ? 1 : 2;
        for (std::size_t i = 0; i < 4; ++i)
        {
            const auto axis = ParseAxis(words[3 + i]);
            if (!axis)
            {
                return Error(std::format(L"hold: '{}' is not an axis value from -1 to 1", words[3 + i]));
            }
            command.axes[i] = *axis;
        }
        const auto ticks = ParseTicks(words[7]);
        if (!ticks)
        {
            return Error(std::format(L"hold: {}", kTicksMessage));
        }
        command.ticks = *ticks;
        return Done(command);
    }

    const Entry* entry = registry.Find(verb);
    if (entry == nullptr)
    {
        return Error(std::format(L"unknown command '{}' (try help)", verb));
    }
    if (!entry->set)
    {
        return Error(std::format(L"{} is only read (get {})", entry->name, entry->name));
    }
    const std::wstring_view argument = entry->shape == Shape::Text ? After(line, 1)
                                       : words.size() == 2        ? words[1]
                                       : words.size() == 1        ? std::wstring_view{}
                                                                  : std::wstring_view(L"?");
    if (entry->shape != Shape::Text && words.size() > 2)
    {
        return Error(std::format(L"{} takes one argument", entry->name));
    }
    const auto value = ParseValue(*entry, argument);
    if (!value.value)
    {
        return Error(value.error);
    }
    command.verb  = Verb::Entry;
    command.name  = entry->name;
    command.value = *value.value;
    return Done(command);
}

std::wstring Describe(const Registry& registry, const Command& command)
{
    switch (command.verb)
    {
    case Verb::Entry:
    {
        const Entry* entry = registry.Find(command.name);
        if (entry == nullptr || entry->shape == Shape::None)
        {
            return command.name;
        }
        return command.name + L" " + Format(*entry, command.value);
    }
    case Verb::Get: return L"get " + command.name;
    case Verb::Status: return L"status";
    case Verb::Help: return command.name.empty() ? L"help" : L"help " + command.name;
    case Verb::Wait: return std::format(L"wait {}", command.ticks);
    case Verb::WaitUntil:
    {
        const Entry* entry = registry.Find(command.name);
        return std::format(L"wait-until {} {} {} {}", command.name, CompareText(command.compare),
                           entry ? Format(*entry, command.value) : L"?", command.ticks);
    }
    case Verb::Hold:
        return std::format(L"hold {} stick {} {} {} {} {}", command.player, command.axes[0], command.axes[1],
                           command.axes[2], command.axes[3], command.ticks);
    case Verb::Note: return L"note " + command.text;
    case Verb::Abort: return L"abort";
    }
    return L"?";
}

}  // namespace lss::control
