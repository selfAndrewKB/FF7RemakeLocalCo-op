#include "setup/guard.h"

#include "platform/settings_write.h"
#include "views/display_move.h"

#include <shellapi.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <string>

namespace lss::setup::guard {

namespace {

constexpr int   kTries = 30;  // about a minute of asking, with the waits inside each try
constexpr DWORD kPause = 1000;

/// One line of plain text, the file made anew: any editor and any tool reads it.
void WriteLog(const std::wstring& path, const std::wstring& line) noexcept
try
{
    const std::string bytes = platform::settings::Utf8(line);
    const HANDLE      file  = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }
    DWORD written = 0;
    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
}
catch (...)
{
}

}  // namespace

/// The game's handle is inherited, so a process number used again cannot fool it. Anything but what the mod passes changes
/// nothing and is gone at once.
bool Run(int& exitCode)
{
    int        count = 0;
    wchar_t**  args  = CommandLineToArgvW(GetCommandLineW(), &count);
    const bool asked = args != nullptr && count >= 2 && wcscmp(args[1], L"--guard") == 0;
    if (!asked)
    {
        if (args != nullptr)
        {
            LocalFree(args);
        }
        return false;
    }
    exitCode = 2;
    if (count != 4)
    {
        LocalFree(args);
        return true;
    }
    wchar_t*           end    = nullptr;
    const auto         value  = wcstoull(args[2], &end, 16);
    const bool         number = end != args[2] && *end == L'\0';
    const std::wstring record = args[3];
    LocalFree(args);
    const auto  game = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value));
    const DWORD pid  = number ? GetProcessId(game) : 0;  // zero for anything that is not a process
    namespace move = views::display_move;
    if (pid == 0 || std::filesystem::path(record).filename() != move::kRecordName)
    {
        return true;
    }
    if (WaitForSingleObject(game, INFINITE) != WAIT_OBJECT_0)
    {
        return true;
    }
    // A desktop that cannot be read now usually can in a moment, and the guard has nothing else to do.
    move::Healed healed = move::Heal(record, pid);
    for (int tries = 0; healed == move::Healed::Kept && tries < kTries; ++tries)
    {
        Sleep(kPause);
        healed = move::Heal(record, pid);
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    WriteLog(record + L".log", std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02} the game ended: {}\r\n", now.wYear, now.wMonth, now.wDay,
                                           now.wHour, now.wMinute, now.wSecond, move::Name(healed)));
    exitCode = healed == move::Healed::Failed ? 1 : 0;
    return true;
}

}  // namespace lss::setup::guard
