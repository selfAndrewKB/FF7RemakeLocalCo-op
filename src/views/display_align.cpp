#include "views/display_align.h"

#include "platform/log.h"
#include "platform/settings.h"
#include "views/display_move.h"
#include "views/record_file.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <utility>
#include <vector>

namespace lss::views::display_align {

namespace {

using row_journal::Entry;
using row_journal::Seen;

using display_move::kGuardSwitch;
using display_move::kSetupExe;

std::vector<Entry> g_entries;       // the record while the monitors are moved; only those that moved say where to
HANDLE             g_guard = nullptr;  // the guard process, once started

std::filesystem::path ModFolder()
{
    return std::filesystem::path(platform::settings::Path()).parent_path();
}

std::wstring RecordPath()
{
    return (ModFolder() / record_file::kName).wstring();
}

/// One guard per game: it lives until the game ends and acts only if a record is there. It inherits a handle to this process
/// alone, so it can tell the game is gone without being fooled by a process number used again.
bool GuardRunning()
{
    if (g_guard != nullptr && WaitForSingleObject(g_guard, 0) == WAIT_TIMEOUT)
    {
        return true;
    }
    if (g_guard != nullptr)
    {
        CloseHandle(g_guard);
        g_guard = nullptr;
    }
    const std::wstring exe = (ModFolder() / kSetupExe).wstring();
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        platform::LogWarning(L"window span: {} is not there, so nothing could put the monitors back after a crash", exe);
        return false;
    }
    HANDLE self = nullptr;
    // Query access too: the guard asks the handle which process it is, to refuse anything else and to find its game's record.
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0))
    {
        platform::LogWarning(L"window span: the guard's handle to the game could not be made (error {})", GetLastError());
        return false;
    }
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> list(size);
    STARTUPINFOEXW    start{};
    start.StartupInfo.cb  = sizeof(start);
    start.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list.data());
    PROCESS_INFORMATION process{};
    bool                started = false, inJob = false;
    DWORD               error   = 0;
    if (InitializeProcThreadAttributeList(start.lpAttributeList, 1, 0, &size))
    {
        // Without the list the guard would inherit every inheritable handle of the game, so a failure here is no guard.
        if (UpdateProcThreadAttribute(start.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &self, sizeof(self), nullptr, nullptr))
        {
            std::wstring line = std::format(L"\"{}\" {} {:x} \"{}\"", exe, kGuardSwitch, reinterpret_cast<std::uintptr_t>(self), RecordPath());
            // Out of the game's job where Windows allows it: whoever closes the job should not take the guard with it.
            started = CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, TRUE,
                                     EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &start.StartupInfo, &process);
            inJob = !started;
            if (!started)
            {
                started = CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
                                         &start.StartupInfo, &process);
            }
        }
        error = started ? 0 : GetLastError();
        DeleteProcThreadAttributeList(start.lpAttributeList);
    }
    CloseHandle(self);
    if (!started)
    {
        platform::LogWarning(L"window span: the guard could not start (error {})", error);
        return false;
    }
    CloseHandle(process.hThread);
    g_guard = process.hProcess;
    platform::Log(L"window span: the guard is running (process {}{}): it puts the monitors back however the game ends", process.dwProcessId,
                  inJob ? L", in the game's job" : L"");
    return true;
}

}  // namespace

void HealEarlierRun() noexcept
try
{
    const std::wstring path = RecordPath();
    if (const auto healed = display_move::Heal(path, 0); healed != display_move::Healed::NoRecord)
    {
        platform::Log(L"window span: a record of moved monitors from an earlier run was found: {}", display_move::Name(healed));
    }
}
catch (...)
{
    platform::LogWarning(L"window span: an earlier run's record could not be read");
}

bool Apply(std::span<const row_journal::Seen> places) noexcept
try
{
    if (places.empty())
    {
        return true;
    }
    // A record still here is an earlier move's, and its old places are the player's: the desktop read now would lose them.
    const std::wstring path = RecordPath();
    display_move::Heal(path, 0);                       // a game that has ended
    display_move::Heal(path, GetCurrentProcessId());  // this game's own, from a return that got no answer
    if (record_file::Exists(path))
    {
        platform::LogWarning(L"window span: a record of moved monitors stands and cannot be settled now; nothing moves");
        return false;
    }
    const auto now = display_move::ReadSteady();
    if (now.empty())
    {
        platform::LogWarning(L"window span: the display configuration could not be read; nothing moves");
        return false;
    }
    std::vector<Entry> entries;
    std::size_t        active = 0;  // the monitors to move that are active outputs
    for (const Seen& seen : now)
    {
        if (seen.monitor.find(L'#') == std::wstring::npos)
        {
            platform::Log(L"window span: the monitor {} has no device path to go by: it is named by its display number, and each read of "
                          L"the desktop for a move waits about a second for the path",
                          seen.monitor);
        }
        Entry entry{seen.monitor, seen.x, seen.y, seen.x, seen.y};
        for (const Seen& place : places)
        {
            if (place.monitor == seen.monitor)
            {
                entry.toX = place.x;
                entry.toY = place.y;
                ++active;
            }
        }
        entries.push_back(std::move(entry));
    }
    if (active != places.size())
    {
        platform::LogWarning(L"window span: {} of the {} monitors to move are not active outputs; nothing moves", places.size() - active, places.size());
        return false;
    }
    if (!GuardRunning())
    {
        platform::LogWarning(L"window span: with no guard nothing moves, as a crash would leave the desktop moved");
        return false;
    }
    const row_journal::Record record{GetCurrentProcessId(), record_file::ThisProcessStart(), entries};
    if (const unsigned long error = record_file::Write(path, record); error != 0)
    {
        platform::LogWarning(L"window span: the record {} could not be written (error {}); nothing moves", path, error);
        return false;
    }
    if (const long result = display_move::Place(places); result != ERROR_SUCCESS)
    {
        record_file::Delete(path);
        platform::LogWarning(L"window span: Windows refused the move (SetDisplayConfig error {}); nothing changed", result);
        return false;
    }
    g_entries = std::move(entries);
    platform::Log(L"window span: monitors moved for as long as the row lasts ({}); Windows' saved arrangement is untouched", Reading());
    return true;
}
catch (...)
{
    platform::LogWarning(L"window span: the move failed for lack of memory");
    return false;
}

void Restore() noexcept
try
{
    if (g_entries.empty())
    {
        return;
    }
    const std::vector<Entry> entries = std::move(g_entries);
    g_entries.clear();  // whatever happens here, the guard still has the record: this is never tried twice
    const auto healed = display_move::PutBack(RecordPath(), entries);
    // Windows going back to its saved arrangement by itself, when a monitor comes or goes, is a normal end to a row.
    if (healed == display_move::Healed::PutBack || healed == display_move::Healed::AlreadyBack || healed == display_move::Healed::Changed)
    {
        platform::Log(L"window span: {}", display_move::Name(healed));
    }
    else
    {
        platform::LogWarning(L"window span: {}", display_move::Name(healed));
    }
}
catch (...)
{
    platform::LogWarning(L"window span: the monitors could not be put back; the guard does it when the game ends");
}

bool Moved() noexcept
{
    return !g_entries.empty();
}

std::wstring Reading() noexcept
try
{
    std::wstring text;
    for (const Entry& entry : g_entries)
    {
        if (entry.fromX != entry.toX || entry.fromY != entry.toY)
        {
            text += std::format(L"{}{} {},{} -> {},{}", text.empty() ? L"" : L", ", row_journal::ShortName(entry.monitor), entry.fromX, entry.fromY, entry.toX, entry.toY);
        }
    }
    return text;
}
catch (...)
{
    return {};
}

}  // namespace lss::views::display_align
