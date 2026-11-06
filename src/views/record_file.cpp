#include "views/record_file.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace lss::views::record_file {

namespace {

constexpr DWORD kMostBytes = 65536;  // a record is one short line per monitor

std::uint64_t StartOf(const FILETIME& created) noexcept
{
    return static_cast<std::uint64_t>(created.dwHighDateTime) << 32 | created.dwLowDateTime;
}

}  // namespace

unsigned long Write(const std::wstring& path, const row_journal::Record& record) noexcept
try
{
    const std::wstring text = row_journal::Format(record);
    const std::wstring temp = path + L".new";
    const HANDLE       file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return GetLastError();
    }
    DWORD      written = 0;
    const auto bytes   = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    const bool wrote   = WriteFile(file, text.data(), bytes, &written, nullptr) && written == bytes && FlushFileBuffers(file);
    DWORD      error   = wrote ? 0 : GetLastError();
    CloseHandle(file);
    if (wrote && !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        error = GetLastError();
    }
    if (error != 0 || !wrote)
    {
        DeleteFileW(temp.c_str());
        return error != 0 ? error : static_cast<DWORD>(ERROR_WRITE_FAULT);
    }
    return 0;
}
catch (...)
{
    return ERROR_OUTOFMEMORY;
}

bool Exists(const std::wstring& path) noexcept
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void Delete(const std::wstring& path) noexcept
{
    DeleteFileW(path.c_str());
}

Found Read(const std::wstring& path)
{
    Found        found;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return found;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size))
    {
        if (size.QuadPart > kMostBytes)
        {
            found.read = true;  // no record is this long: a damaged one, not a short read of a good one
        }
        else
        {
            std::wstring text(static_cast<std::size_t>(size.QuadPart) / sizeof(wchar_t), L'\0');
            DWORD        got = 0;
            found.read = ReadFile(file, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &got, nullptr) && got == text.size() * sizeof(wchar_t);
            if (found.read)
            {
                found.record = row_journal::Parse(text);
            }
        }
    }
    CloseHandle(file);
    return found;
}

std::uint64_t ThisProcessStart() noexcept
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
    {
        return 0;
    }
    return StartOf(created);
}

bool OwnerAlive(const row_journal::Record& record) noexcept
{
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, record.ownerPid);
    if (process == nullptr)
    {
        return false;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT && GetProcessTimes(process, &created, &exited, &kernel, &user) &&
                         StartOf(created) == record.ownerStart;
    CloseHandle(process);
    return running;
}

}  // namespace lss::views::record_file
