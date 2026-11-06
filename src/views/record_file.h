#pragma once

// The record of moved monitors as a file beside the settings file, and the process that owns it. What a record holds and what
// it means is views/row_journal; who acts on it is views/display_move. Shared by the mod, which writes it before a move, and
// the setup app, whose guard mode reads it when the game ends. No logging and no state: callers say what happened.

#include "views/row_journal.h"

#include <cstdint>
#include <optional>
#include <string>

namespace lss::views::record_file {

/// The record's file name, beside the settings file: the mod writes it there and the guard takes no other.
inline constexpr const wchar_t* kName = L"LocalSplitScreen.desktop";

/// Written whole or not at all. 0, or the Windows error.
unsigned long Write(const std::wstring& path, const row_journal::Record& record) noexcept;
bool          Exists(const std::wstring& path) noexcept;
void          Delete(const std::wstring& path) noexcept;

/// What the file holds. `read` is false when it could not be opened or read, which says nothing about what is in it; a file
/// that was read and makes no record has `read` true and no record.
struct Found
{
    bool                               read = false;
    std::optional<row_journal::Record> record;
};

Found Read(const std::wstring& path);

/// When this process started, as a record names its owner.
std::uint64_t ThisProcessStart() noexcept;

/// The record's owner is running: the same process number and the same start time.
bool OwnerAlive(const row_journal::Record& record) noexcept;

}  // namespace lss::views::record_file
