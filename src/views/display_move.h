#pragma once

// The desktop's monitor positions through Windows' display-configuration API, the one Display settings itself uses, and the
// record kept while they are moved (views/row_journal). Shared by the mod, which moves them, and the setup app, whose guard
// mode puts them back when the game dies. No logging and no state: callers say what happened.
//
// What was measured on Windows 11 with four monitors and two virtual ones [CONFIRMED live]:
//   - SetDisplayConfig with the supplied configuration and without SDC_SAVE_TO_DATABASE moves any number of monitors at once in
//     100-400 ms and leaves Windows' saved arrangement as it was: only the running session changes.
//   - A layout with a gap or an overlap returns success and changes nothing, so a move counts only when read back.
//   - The move outlives the process that made it: a killed process leaves the desktop moved. Hence the guard.
//   - A monitor coming or going returns the desktop to Windows' saved arrangement by itself.
// Positions only: no display mode, resolution or scaling is touched.

#include "views/row_journal.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lss::views::display_move {

/// The record's file name, beside the settings file: the mod writes it there and the guard takes no other.
inline constexpr const wchar_t* kRecordName = L"LocalSplitScreen.desktop";

/// The setup app's file, beside the settings file. The same exe is the guard, so the mod starts it by this name. The build
/// names the exe from this line (CMakeLists.txt), and so do its file properties and the install tool.
inline constexpr const wchar_t* kSetupExe = L"MultiMonitorSetup.exe";

/// Every active monitor's position now; empty if it could not be read.
std::vector<row_journal::Seen> Read() noexcept;

/// Puts each named monitor at its position, all at once, for this Windows session. ERROR_SUCCESS (0) does not prove it
/// happened: read it back. A device that is not an active source fails with ERROR_NOT_FOUND and changes nothing.
long Place(std::span<const row_journal::Seen> places) noexcept;

/// Windows' saved arrangement, now.
long BackToSaved() noexcept;

/// The desktop reads as Windows' saved arrangement. True when that cannot be read: nothing is then offered to fix.
bool IsSavedLayout() noexcept;

/// Whether every named monitor reads at its position within `timeoutMs`.
bool WaitForPlaces(std::span<const row_journal::Seen> places, unsigned long timeoutMs) noexcept;

/// When this process started, as the record names its owner.
std::uint64_t ThisProcessStart() noexcept;

/// The record's file beside the settings file, written whole or not at all. 0, or the Windows error.
unsigned long WriteRecord(const std::wstring& path, const row_journal::Record& record) noexcept;
bool RecordExists(const std::wstring& path) noexcept;
void DeleteRecord(const std::wstring& path) noexcept;

/// What a record left behind led to.
enum class Healed
{
    NoRecord,     // nothing was moved
    AlreadyBack,  // the monitors were back or were never moved
    Changed,      // the desktop has moved on since; left as it is
    PutBack,      // the monitors are back as they were
    Failed,       // Windows would not put them back at their old places: its saved arrangement was asked for instead
    Kept,         // no definite answer: the record stays for a later try
    NotOurs,      // the record is another game's: it stays
    Damaged,      // the record does not read: Windows' saved arrangement was asked for if the desktop differs from it
};

/// Reads the record and, on a definite answer, puts the monitors back if the desktop still reads as the mod left it, then removes
/// the record. `game` as row_journal::Judge takes it: the process number of the game that has just ended, or 0 at a later start.
Healed Heal(const std::wstring& path, std::uint32_t game) noexcept;

/// The same for the game that made the record and holds its entries; the file is then only removed.
Healed PutBack(const std::wstring& path, std::span<const row_journal::Entry> entries) noexcept;

const wchar_t* Name(Healed healed) noexcept;

}  // namespace lss::views::display_move
