#include "control/registry.h"

#include "platform/settings.h"

#include <cmath>
#include <cwchar>
#include <format>

namespace lss::control {

namespace {

std::optional<std::int64_t> WholeNumber(std::wstring_view word, int base)
{
    const std::wstring text(word);
    if (text.empty() || text[0] == L'-' || text[0] == L'+')
    {
        return std::nullopt;
    }
    wchar_t*   end   = nullptr;
    const auto value = std::wcstoull(text.c_str(), &end, base);
    if (end != text.c_str() + text.size() || value > static_cast<unsigned long long>(INT64_MAX))
    {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(value);
}

std::optional<double> Number(std::wstring_view word)
{
    const std::wstring text(word);
    wchar_t*           end   = nullptr;
    const double       value = text.empty() ? 0.0 : std::wcstod(text.c_str(), &end);
    if (text.empty() || end != text.c_str() + text.size() || !std::isfinite(value))
    {
        return std::nullopt;
    }
    return value;
}

std::wstring Join(const std::vector<std::wstring>& words)
{
    std::wstring out;
    for (const auto& word : words)
    {
        out += out.empty() ? word : L"|" + word;
    }
    return out;
}

}  // namespace

std::wstring Format(const Entry& entry, const Value& value)
{
    switch (value.index())
    {
    case 1: return std::get<bool>(value) ? L"on" : L"off";
    case 2: return entry.shape == Shape::Hex ? std::format(L"{:X}", std::get<std::int64_t>(value))
                                             : std::to_wstring(std::get<std::int64_t>(value));
    case 3: return std::format(L"{}", std::get<double>(value));
    case 4: return std::get<std::wstring>(value);
    default: return {};
    }
}

ParsedValue ParseValue(const Entry& entry, std::wstring_view text)
{
    switch (entry.shape)
    {
    case Shape::None:
        return text.empty() ? ParsedValue{Value{}, {}} : ParsedValue{std::nullopt, entry.name + L" takes no argument"};
    case Shape::OnOff:
        if (text == L"on" || text == L"off")
        {
            return {Value{text == L"on"}, {}};
        }
        return {std::nullopt, entry.name + L" takes on or off"};
    case Shape::Integer:
    {
        const auto value = WholeNumber(text, 10);
        if (!value || *value < entry.min || *value > entry.max)
        {
            return {std::nullopt, std::format(L"{} takes a whole number from {} to {}", entry.name, entry.min, entry.max)};
        }
        return {Value{*value}, {}};
    }
    case Shape::Hex:
    {
        const auto value = WholeNumber(text, 16);
        if (!value || *value > entry.max)
        {
            return {std::nullopt, std::format(L"{} takes a hexadecimal mask up to {:X}", entry.name, entry.max)};
        }
        return {Value{*value}, {}};
    }
    case Shape::Word:
        for (const auto& word : entry.words)
        {
            if (text == word)
            {
                return {Value{word}, {}};
            }
        }
        return {std::nullopt, std::format(L"{} takes {}", entry.name, Join(entry.words))};
    case Shape::Float:
    {
        const auto value = Number(text);
        if (!value || *value < entry.minFloat || *value > entry.maxFloat)
        {
            return {std::nullopt,
                    std::format(L"{} takes a number from {} to {}", entry.name, entry.minFloat, entry.maxFloat)};
        }
        return {Value{*value}, {}};
    }
    case Shape::Text:
        if (text.empty())
        {
            return {std::nullopt, entry.name + L" needs text"};
        }
        return {Value{std::wstring(text)}, {}};
    }
    return {std::nullopt, L"unknown shape"};
}

bool Registry::Add(Entry entry)
{
    const bool malformed = entry.name.empty() || entry.name.find_first_of(L" \t\r\n") != std::wstring::npos ||
                           (!entry.set && !entry.get) || (entry.shape == Shape::Word && entry.words.empty()) ||
                           (entry.shape == Shape::Integer && entry.max < entry.min) ||
                           (entry.shape == Shape::Float && entry.maxFloat < entry.minFloat) ||
                           (entry.shape == Shape::Hex && entry.max < 0);
    if (malformed || Find(entry.name) != nullptr)
    {
        rejected_.push_back(entry.name.empty() ? L"(unnamed)" : entry.name);
        return false;
    }
    entries_.push_back(std::move(entry));
    return true;
}

const Entry* Registry::Find(std::wstring_view name) const noexcept
{
    for (const auto& entry : entries_)
    {
        if (entry.name == name)
        {
            return &entry;
        }
    }
    return nullptr;
}

void RegisterOption(const platform::settings::Option& option, std::function<Outcome(const std::wstring& word)> set,
                    std::function<std::wstring()> get)
{
    // In either order: the page lists an option's words as the table does.
    const bool onOff = option.words.size() == 2 &&
                       ((std::wstring_view(option.words[0]) == L"on" && std::wstring_view(option.words[1]) == L"off") ||
                        (std::wstring_view(option.words[0]) == L"off" && std::wstring_view(option.words[1]) == L"on"));
    Entry entry{.name  = option.key,
                .shape = onOff ? Shape::OnOff : Shape::Word,
                .help  = std::wstring(option.help) + L" (default: " + option.fallback + L")"};
    if (!onOff)
    {
        entry.words.assign(option.words.begin(), option.words.end());
    }
    entry.set = [set = std::move(set), onOff](const Value& v) {
        return set(onOff ? std::wstring(std::get<bool>(v) ? L"on" : L"off") : std::get<std::wstring>(v));
    };
    entry.get = [get = std::move(get), onOff]() -> Value {
        const std::wstring word = get();
        return onOff ? Value{word == L"on"} : Value{word};
    };
    Register(std::move(entry));
}

Registry& Global() noexcept
{
    static Registry registry;
    return registry;
}

}  // namespace lss::control
