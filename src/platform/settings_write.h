#pragma once

// The settings file (platform/settings.h) as the mod writes it: the whole text from the options table (FileText), so
// every option, its words, its default and its help in the file are the code's own and never a stale copy. The values
// are the readings (the player's words, kept; a word the mod cannot read kept as written, with a note). Every byte is
// plain ASCII (a kept word's other characters written `?`), so the profile API, which reads the file in the system's
// code page, reads back exactly what was written; written through a temporary file moved over the old one.
// Inline so the host test builds and writes a real file (tests/settings_test.cpp).

#include "platform/settings.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::platform::settings {

/// The longest line the file's comments are wrapped to.
inline constexpr std::size_t kFileWidth = 118;

/// `text` as comment lines ("; " and the words), wrapped at kFileWidth, each line ended by CR LF. A line break in the
/// text (a Co-op page help line on two lines) is a space here: the file wraps on its own.
inline std::wstring CommentLines(std::wstring_view text)
{
    std::wstring out, line = L";";
    std::size_t  at = 0;
    while (at < text.size())
    {
        const std::size_t end  = text.find_first_of(L" \r\n", at);
        const auto        word = text.substr(at, end == std::wstring_view::npos ? std::wstring_view::npos : end - at);
        at = end == std::wstring_view::npos ? text.size() : end + 1;
        if (word.empty())
        {
            continue;
        }
        if (line.size() > 1 && line.size() + 1 + word.size() > kFileWidth)
        {
            out += line + L"\r\n";
            line = L";";
        }
        line += L" ";
        line += word;
    }
    return line.size() > 1 ? out + line + L"\r\n" : out;
}

/// The words an option takes, as the file lists them: "auto | on | off", or "0.1 to 10.0" for a long run of values.
inline std::wstring FileWords(const Option& option)
{
    constexpr std::size_t kListed = 8;  // more words than this are a range
    const std::size_t     count   = option.words.size();
    if (count > kListed)
    {
        return std::wstring(option.words[0]) + L" to " + option.words[count - 1];
    }
    std::wstring list;
    for (std::size_t i = 0; i < count; ++i)
    {
        list += (i == 0 ? L"" : L" | ") + std::wstring(option.words[i]);
    }
    return list;
}

/// A value the mod cannot read, as the file keeps it: each character outside plain ASCII written `?`. The profile API
/// reads the file in the system's code page, not UTF-8, so such a character would come back as other characters and the
/// value would change at every start; with `?` it reads back as written (and stays a value the mod cannot read).
inline std::wstring KeptRaw(std::wstring_view raw)
{
    std::wstring kept(raw);
    for (wchar_t& c : kept)
    {
        c = c >= 0x20 && c < 0x7F ? c : L'?';
    }
    return kept;
}

/// The whole file for these readings (one per row of kOptions, in its order): a header, then each section once, each
/// option with its Co-op page name and help, its words and default, and its value line.
inline std::wstring FileText(std::span<const Reading, kOptions.size()> readings)
{
    std::wstring text = CommentLines(
        L"LocalSplitScreen (FF7 Remake): the mod's settings, read once when the game starts. The mod writes this file "
        L"itself at every start, from its own list of options, so every option here is current: change a value after an "
        L"\"=\" and the next start reads it. Comments added here are not kept, and an option the mod no longer has is "
        L"dropped. A choice made on the Co-op page is saved here at once. Every option of the Co-op page is also a "
        L"console command of the same name, for the session only. A value's case, spaces and commas do not matter; a "
        L"value that is none of the "
        L"option's words is kept as written, with a note, and the option takes its default.");
    const wchar_t* section = nullptr;
    for (std::size_t i = 0; i < kOptions.size(); ++i)
    {
        const Option&  option  = *kOptions[i];
        const Reading& reading = readings[i];
        if (section == nullptr || std::wstring_view(section) != option.section)
        {
            section = option.section;
            text += std::wstring(L"\r\n[") + section + L"]\r\n";
        }
        text += L"\r\n";
        text += CommentLines(std::wstring(option.label) + L": " + option.menuHelp);
        if (!option.words.empty())
        {
            text += CommentLines(FileWords(option) + L" (the default: " + option.fallback + L")");
        }
        if (reading.source == Source::Invalid)
        {
            text += CommentLines(L"The value below is none of these words: read as " + std::wstring(option.fallback) + L".");
        }
        // An option of any word goes through KeptRaw too: its word is the file's own text, not one of the table's.
        const std::wstring value = reading.source == Source::Invalid ? KeptRaw(reading.raw)
                                   : option.words.empty()            ? KeptRaw(reading.word)
                                                                     : reading.word;
        text += std::wstring(option.key) + L" = " + value + L"\r\n";
    }
    return text;
}

/// `text` as UTF-8.
inline std::string Utf8(std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int   size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
    if (size > 0)
    {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    }
    return out;
}

/// The file's bytes now, or empty (no file, or unreadable).
inline std::string FileBytes(const wchar_t* path)
{
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return {};
    }
    std::string   bytes;
    char          buffer[4096];
    DWORD         read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0)
    {
        bytes.append(buffer, read);
    }
    CloseHandle(file);
    return bytes;
}

/// Writes `bytes` as the file at `path` (a temporary file beside it, moved over the old one). 0 when written, else the
/// Windows error.
inline unsigned long WriteFileBytes(const wchar_t* path, const std::string& bytes) noexcept
{
    const std::wstring temp = std::wstring(path) + L".tmp";
    const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return GetLastError();
    }
    DWORD      written = 0;
    const BOOL ok      = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    const unsigned long writeError = ok && written == bytes.size() ? 0 : (GetLastError() != 0 ? GetLastError() : ERROR_WRITE_FAULT);
    CloseHandle(file);
    if (writeError != 0)
    {
        DeleteFileW(temp.c_str());
        return writeError;
    }
    if (!MoveFileExW(temp.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        const unsigned long error = GetLastError();
        DeleteFileW(temp.c_str());
        return error != 0 ? error : ERROR_WRITE_FAULT;
    }
    return 0;
}

/// Every option's reading from the file at `path`; a missing file reads as every default. The mod's Load and the setup
/// app read through this, so they judge the file alike.
inline std::array<Reading, kOptions.size()> ReadFile(const std::wstring& path)
{
    const bool                           exists = !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    std::array<Reading, kOptions.size()> readings;
    for (std::size_t i = 0; i < kOptions.size(); ++i)
    {
        std::wstring text;
        if (exists)
        {
            wchar_t    buffer[1024]{};  // room for every monitor's device path
            const auto length = GetPrivateProfileStringW(kOptions[i]->section, kOptions[i]->key, L"", buffer,
                                                         static_cast<DWORD>(std::size(buffer)), path.c_str());
            text.assign(buffer, length);
        }
        readings[i] = Judge(*kOptions[i], text);
    }
    return readings;
}

/// Writes the file from the table with `readings`, unless it already reads exactly so. 0 when the file is as wanted, else
/// the Windows error.
inline unsigned long WriteFromTable(const std::wstring& path, std::span<const Reading, kOptions.size()> readings, bool* written)
{
    const std::string bytes = Utf8(FileText(readings));
    *written                = false;
    if (FileBytes(path.c_str()) == bytes)
    {
        return 0;
    }
    const unsigned long error = WriteFileBytes(path.c_str(), bytes);
    *written                  = error == 0;
    return error;
}

}  // namespace lss::platform::settings
