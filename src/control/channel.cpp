#include "control/channel.h"

#include "platform/log.h"

#include <cmath>
#include <cwchar>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::control {

namespace {

namespace fs = std::filesystem;
using platform::Log;

/// How often an idle channel looks in its inbox (about four times a second at 60 ticks per second).
constexpr std::uint64_t kScanTicks = 15;

/// A script without waits still spreads over ticks, so it never stalls one frame for long.
constexpr std::size_t kMaxCommandsPerTick = 32;

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

/// The text after the first word, trimmed.
std::wstring_view Rest(std::wstring_view line)
{
    line           = Trim(line);
    const auto end = line.find_first_of(kSpace);
    return end == std::wstring_view::npos ? std::wstring_view{} : Trim(line.substr(end));
}

std::optional<std::uint32_t> ParseTicks(std::wstring_view word)
{
    const std::wstring text(word);
    wchar_t*           end   = nullptr;
    const auto         value = std::wcstoul(text.c_str(), &end, 10);
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
    const float        value = std::wcstof(text.c_str(), &end);
    if (text.empty() || end != text.c_str() + text.size() || !std::isfinite(value) || value < -1.0f || value > 1.0f)
    {
        return std::nullopt;
    }
    return value;
}

ParsedLine Error(std::wstring message)
{
    return {std::nullopt, std::move(message)};
}

ParsedLine Simple(CommandKind kind, const std::vector<std::wstring_view>& words)
{
    if (words.size() != 1)
    {
        return Error(std::format(L"{} takes no arguments", words[0]));
    }
    Command command;
    command.kind = kind;
    return {command, {}};
}

std::wstring FromUtf8(std::string_view text)
{
    if (text.starts_with("\xEF\xBB\xBF"))
    {
        text.remove_prefix(3);
    }
    if (text.empty())
    {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string ToUtf8(const std::wstring& wide)
{
    if (wide.empty())
    {
        return {};
    }
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), bytes.data(), length, nullptr, nullptr);
    return bytes;
}

/// The folder holding this DLL (Mods/LocalSplitScreen/dlls), or empty.
fs::path DllFolder()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&DllFolder), &module))
    {
        return {};
    }
    std::wstring path(MAX_PATH, L'\0');
    const DWORD  length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
    {
        return {};
    }
    path.resize(length);
    return fs::path(path).parent_path();
}

/// The inbox's first script by file name, or empty.
std::optional<fs::path> FirstScript(const fs::path& inbox)
{
    std::error_code         error;
    std::optional<fs::path> first;
    for (fs::directory_iterator it(inbox, error), end; !error && it != end; it.increment(error))
    {
        const auto& path = it->path();
        if (it->is_regular_file(error) && path.extension() == L".txt" && (!first || path.filename() < first->filename()))
        {
            first = path;
        }
    }
    return first;
}

}  // namespace

ParsedLine ParseLine(std::wstring_view line)
{
    const auto words = Words(line);
    if (words.empty() || words[0].starts_with(L"#"))
    {
        return {};
    }
    const auto& verb = words[0];
    if (verb == L"join") return Simple(CommandKind::Join, words);
    if (verb == L"leave") return Simple(CommandKind::Leave, words);
    if (verb == L"seat") return Simple(CommandKind::Seat, words);
    if (verb == L"unseat") return Simple(CommandKind::Unseat, words);
    if (verb == L"status") return Simple(CommandKind::Status, words);
    if (verb == L"quit") return Simple(CommandKind::Quit, words);
    if (verb == L"wait")
    {
        const auto ticks = words.size() == 2 ? ParseTicks(words[1]) : std::nullopt;
        if (!ticks)
        {
            return Error(std::format(L"wait takes a whole number of ticks from 1 to {}", kMaxTicks));
        }
        Command command;
        command.kind  = CommandKind::Wait;
        command.ticks = *ticks;
        return {command, {}};
    }
    if (verb == L"stick")
    {
        if (words.size() != 7 || (words[1] != L"1" && words[1] != L"2"))
        {
            return Error(L"stick takes <player 1|2> <lx> <ly> <rx> <ry> <ticks>");
        }
        Command command;
        command.kind   = CommandKind::Stick;
        command.player = words[1] == L"1" ? 1 : 2;
        for (std::size_t i = 0; i < 4; ++i)
        {
            const auto axis = ParseAxis(words[2 + i]);
            if (!axis)
            {
                return Error(std::format(L"stick: '{}' is not an axis value from -1 to 1", words[2 + i]));
            }
            command.axes[i] = *axis;
        }
        const auto ticks = ParseTicks(words[6]);
        if (!ticks)
        {
            return Error(std::format(L"stick: the ticks are a whole number from 1 to {}", kMaxTicks));
        }
        command.ticks = *ticks;
        return {command, {}};
    }
    if (verb == L"ao-origin" || verb == L"bloom-origin")
    {
        if (words.size() != 2 || (words[1] != L"on" && words[1] != L"off"))
        {
            return Error(std::format(L"{} takes on or off", verb));
        }
        Command command;
        command.kind  = verb == L"ao-origin" ? CommandKind::AoOrigin : CommandKind::BloomOrigin;
        command.ticks = words[1] == L"on" ? 1u : 0u;
        return {command, {}};
    }
    if (verb == L"showflags")
    {
        const std::wstring text = words.size() == 2 ? std::wstring(words[1]) : std::wstring{};
        wchar_t*           end  = nullptr;
        const auto         mask = text.empty() ? 0ul : std::wcstoul(text.c_str(), &end, 16);
        if (text.empty() || text[0] == L'-' || end != text.c_str() + text.size() || mask > 0xFFFFFFFFul)
        {
            return Error(L"showflags takes one hexadecimal mask (e.g. 100A, or 0)");
        }
        Command command;
        command.kind  = CommandKind::ShowFlags;
        command.ticks = static_cast<std::uint32_t>(mask);
        return {command, {}};
    }
    if (verb == L"console" || verb == L"note")
    {
        const auto text = Rest(line);
        if (text.empty())
        {
            return Error(std::format(L"{} needs text", verb));
        }
        Command command;
        command.kind = verb == L"console" ? CommandKind::Console : CommandKind::Note;
        command.text = std::wstring(text);
        return {command, {}};
    }
    return Error(std::format(L"unknown command '{}'", verb));
}

std::wstring Describe(const Command& command)
{
    switch (command.kind)
    {
    case CommandKind::Join: return L"join";
    case CommandKind::Leave: return L"leave";
    case CommandKind::Seat: return L"seat";
    case CommandKind::Unseat: return L"unseat";
    case CommandKind::Status: return L"status";
    case CommandKind::Quit: return L"quit";
    case CommandKind::Wait: return std::format(L"wait {}", command.ticks);
    case CommandKind::Stick:
        return std::format(L"stick {} {} {} {} {} {}", command.player, command.axes[0], command.axes[1], command.axes[2],
                           command.axes[3], command.ticks);
    case CommandKind::Console: return L"console " + command.text;
    case CommandKind::ShowFlags: return std::format(L"showflags {:X}", command.ticks);
    case CommandKind::AoOrigin: return command.ticks != 0 ? L"ao-origin on" : L"ao-origin off";
    case CommandKind::BloomOrigin: return command.ticks != 0 ? L"bloom-origin on" : L"bloom-origin off";
    case CommandKind::Note: return L"note " + command.text;
    }
    return L"?";
}

bool CommandChannel::Open() noexcept
try
{
    const fs::path dlls = DllFolder();
    if (dlls.empty())
    {
        Log(L"control: the mod's own folder cannot be found; the command channel is closed");
        return false;
    }
    const fs::path root = dlls.parent_path() / L"control";
    inbox_              = root / L"inbox";
    done_               = root / L"done";
    std::error_code error;
    fs::create_directories(inbox_, error);
    fs::create_directories(done_, error);
    if (!fs::is_directory(inbox_, error) || !fs::is_directory(done_, error))
    {
        Log(L"control: could not create {}; the command channel is closed", root.wstring());
        return false;
    }
    open_ = true;
    Log(L"control: command channel open; scripts go in {}", inbox_.wstring());
    return true;
}
catch (...)
{
    Log(L"control: the command channel could not be opened");
    return false;
}

void CommandChannel::Tick(Target& target) noexcept
{
    if (!open_)
    {
        return;
    }
    ++tick_;
    try
    {
        if (!running_ && tick_ % kScanTicks == 0)
        {
            StartNext();
        }
        if (running_ && tick_ % kScanTicks == 0)
        {
            std::error_code error;
            const fs::path  abort = inbox_ / L"abort";
            if (fs::exists(abort, error))
            {
                fs::remove(abort, error);
                target.ReleaseStick(1);
                target.ReleaseStick(2);
                Append(std::format(L"tick {}: aborted by the abort file", tick_));
                Append(L"finished: aborted");
                running_.reset();
                return;
            }
        }
        if (running_)
        {
            Step(target);
        }
    }
    catch (...)
    {
        Log(L"control: an exception stopped the command channel");
        if (running_)
        {
            target.ReleaseStick(1);
            target.ReleaseStick(2);
            running_.reset();
        }
        open_ = false;
    }
}

void CommandChannel::StartNext()
{
    const auto script = FirstScript(inbox_);
    if (!script)
    {
        return;
    }
    std::string bytes;
    bool        unreadable = false;
    {
        std::ifstream file(*script, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        unreadable = !file.is_open() || file.bad();
    }
    // The script leaves the inbox before anything runs, so it can never run twice.
    std::error_code error;
    const fs::path  taken = done_ / script->filename();
    Running         running;
    running.results = done_ / (script->stem().wstring() + L".result.txt");
    running.name    = script->filename().wstring();
    fs::remove(taken, error);
    fs::remove(running.results, error);
    fs::rename(*script, taken, error);
    if (error)
    {
        Log(L"control: could not move {} out of the inbox; it is skipped until it can be", running.name);
        return;
    }
    running_.emplace(std::move(running));

    bool          rejected = unreadable || bytes.starts_with("\xFF\xFE") || bytes.starts_with("\xFE\xFF");
    std::uint32_t number   = 0;
    if (rejected)
    {
        Append(unreadable ? L"the script could not be read" : L"the script is UTF-16; save it as UTF-8");
    }
    const std::wstring text = rejected ? std::wstring{} : FromUtf8(bytes);
    for (std::size_t start = 0; !rejected && start <= text.size();)
    {
        const auto end = text.find(L'\n', start);
        auto parsed = ParseLine(std::wstring_view(text).substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        ++number;
        if (parsed.command)
        {
            running_->lines.push_back({number, std::move(*parsed.command)});
        }
        else if (!parsed.error.empty())
        {
            Append(std::format(L"line {}: {}", number, parsed.error));
            rejected = true;
        }
        if (end == std::wstring::npos)
        {
            break;
        }
        start = end + 1;
    }
    if (rejected)
    {
        Append(L"finished: rejected");
        Log(L"control: {} rejected; nothing ran", running_->name);
        running_.reset();
        return;
    }
    Log(L"control: running {} ({} command(s))", running_->name, running_->lines.size());
}

void CommandChannel::Step(Target& target)
{
    Running& run = *running_;
    for (std::int32_t player = 1; player <= 2; ++player)
    {
        auto& ticks = run.holdTicks[player - 1];
        if (ticks > 0 && --ticks == 0)
        {
            target.ReleaseStick(player);
        }
    }
    if (run.waitTicks > 0)
    {
        if (--run.waitTicks > 0)
        {
            return;
        }
        Append(std::format(L"tick {}: wait ended", tick_));
    }
    for (std::size_t done = 0; done < kMaxCommandsPerTick && run.next < run.lines.size(); ++done)
    {
        const Line& line = run.lines[run.next++];
        const Command& command = line.command;
        Outcome outcome;
        switch (command.kind)
        {
        case CommandKind::Join: outcome = target.Join(); break;
        case CommandKind::Leave: outcome = target.Leave(); break;
        case CommandKind::Seat: outcome = target.Seat(true); break;
        case CommandKind::Unseat: outcome = target.Seat(false); break;
        case CommandKind::Status: outcome = target.Status(); break;
        case CommandKind::Quit: outcome = target.Quit(); break;
        case CommandKind::Console: outcome = target.Console(command.text); break;
        case CommandKind::ShowFlags: outcome = target.ShowFlags(command.ticks); break;
        case CommandKind::AoOrigin: outcome = target.AoOrigin(command.ticks != 0); break;
        case CommandKind::BloomOrigin: outcome = target.BloomOrigin(command.ticks != 0); break;
        case CommandKind::Note: Log(L"control note: {}", command.text); break;
        case CommandKind::Stick:
            outcome = target.HoldStick(command.player, command.axes);
            if (outcome.ok)
            {
                run.holdTicks[command.player - 1] = command.ticks;
            }
            break;
        case CommandKind::Wait: run.waitTicks = command.ticks; break;
        }
        const std::wstring detail = outcome.detail.empty() ? L"" : L" (" + outcome.detail + L")";
        Append(std::format(L"tick {} line {}: {} -> {}{}", tick_, line.number, Describe(command), outcome.ok ? L"ok" : L"FAILED", detail));
        if (!outcome.ok)
        {
            Finish(target, true);
            return;
        }
        if (command.kind == CommandKind::Wait)
        {
            return;
        }
    }
    if (run.next >= run.lines.size() && run.holdTicks[0] == 0 && run.holdTicks[1] == 0)
    {
        Finish(target, false);
    }
}

void CommandChannel::Finish(Target& target, bool failed)
{
    target.ReleaseStick(1);
    target.ReleaseStick(2);
    Append(failed ? L"finished: failed" : L"finished: ok");
    Log(L"control: {} finished {}", running_->name, failed ? L"with a failure" : L"ok");
    running_.reset();
}

void CommandChannel::Append(const std::wstring& line) noexcept
{
    try
    {
        Log(L"control {}: {}", running_ ? running_->name : std::wstring{}, line);
        if (running_)
        {
            std::ofstream out(running_->results, std::ios::binary | std::ios::app);
            out << ToUtf8(line) << "\r\n";
        }
    }
    catch (...)
    {
    }
}

}  // namespace lss::control
