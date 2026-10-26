#pragma once

// The command registry: every switch, action and readable value the control console knows, as the modules
// register them at install. One entry gives a command its name, its argument's shape, what it does (set or
// act), what it reads back (get) and one line of help. The parser checks a line against the shape, and
// `help`, `get`, `status` and `wait-until` are generated from the table, so a module adds a command with one
// call beside the code it drives, and nothing else in the mod names it.
//
//   control::Register({.name = L"bloom-origin", .shape = Shape::OnOff, .help = L"...",
//                      .set = [](const Value& v) { ...; return Outcome{}; },
//                      .get = [] { return Value{Enabled()}; }});
//
// Entries are added on the game thread at install, before the console opens, and read on the game thread.
// The registry has no engine dependency, so the parser and runner are tested on the host with fake entries.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace lss::platform::settings {
struct Option;
}

namespace lss::control {

/// What a command did; `detail` says why when it failed, and may add a fact when it succeeded.
struct Outcome
{
    bool         ok = true;
    std::wstring detail;
};

/// The shape of a command's one argument, which the parser checks before anything runs.
enum class Shape : std::uint8_t
{
    None,     // no argument: an action (join), or a value that is only read (tick)
    OnOff,    // on | off
    Integer,  // a whole number in [min, max]
    Hex,      // a hexadecimal mask, at most max (e.g. 100A)
    Word,     // one of `words`
    Float,    // a number in [minFloat, maxFloat]
    Text,     // the rest of the line (console, note)
};

/// An argument or a read value: a bool (OnOff), an integer (Integer, Hex), a double (Float) or text (Word, Text).
using Value = std::variant<std::monostate, bool, std::int64_t, double, std::wstring>;

struct Entry
{
    std::wstring              name;
    Shape                     shape    = Shape::None;
    std::int64_t              min      = 0;      // Integer; Hex: 0
    std::int64_t              max      = 0;      // Integer, Hex
    double                    minFloat = 0.0;    // Float
    double                    maxFloat = 0.0;    // Float
    std::vector<std::wstring> words;             // Word
    std::wstring              help;

    /// Runs the command with its argument (an empty Value for Shape::None). Absent for a read-only value.
    std::function<Outcome(const Value&)> set;
    /// The current value, in the entry's shape (an OnOff reads as a bool, a Word as text). Absent for an action.
    std::function<Value()> get;
};

/// The value as a script writes it: on/off, a decimal, a hex mask, or the text itself.
std::wstring Format(const Entry& entry, const Value& value);

/// Checks one argument word (or the rest of the line, for Text) against the entry's shape; the message says
/// what the shape wants when it does not fit.
struct ParsedValue
{
    std::optional<Value> value;
    std::wstring         error;
};
ParsedValue ParseValue(const Entry& entry, std::wstring_view text);

/// The table. A class so tests build their own; the mod uses the one in Global().
class Registry
{
public:
    /// Adds an entry; false (and no change) if the name is taken or the entry is malformed (a Word shape with no
    /// words, a range with max below min, neither set nor get).
    bool Add(Entry entry);

    const Entry* Find(std::wstring_view name) const noexcept;

    /// Every entry, in registration order.
    const std::vector<Entry>& Entries() const noexcept { return entries_; }

    /// The names Add refused, for the console to log when it opens (the registry itself cannot log: it has no
    /// engine dependency, so the host tests can build it).
    const std::vector<std::wstring>& Rejected() const noexcept { return rejected_; }

private:
    std::vector<Entry>        entries_;
    std::vector<std::wstring> rejected_;
};

/// The mod's registry.
Registry& Global() noexcept;

/// `Global().Add(entry)`; a refusal is kept in Rejected().
inline void Register(Entry entry) { Global().Add(std::move(entry)); }

/// A command for one option of the settings file, built from its row of the table (platform/settings.h): the
/// option's key as the name, its words as a Word shape (exactly on|off: an OnOff, so scripts write `menu-slow off`
/// and read a bool), its help with the default appended. `set` takes one of the option's words and says what
/// happened; `get` answers the current word. The option's owner calls this at install beside the setter the file
/// and the keys call too.
void RegisterOption(const platform::settings::Option& option, std::function<Outcome(const std::wstring& word)> set,
                    std::function<std::wstring()> get);

}  // namespace lss::control
