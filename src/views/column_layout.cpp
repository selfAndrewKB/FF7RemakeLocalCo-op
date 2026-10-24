#include "views/column_layout.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "views/column_rules.h"

#include <cstdint>
#include <format>

namespace lss::views::column_layout {

namespace {

using column_rules::Action;
using column_rules::kStockFavorTop;
using column_rules::kThreeColumns;
using column_rules::kThreePlayerRecords;
using column_rules::Record;
using platform::Read;

static_assert(sizeof(Record) == facts::kPerPlayerSplitBytes, "a record is the engine's FPerPlayerSplitscreenData");

constexpr std::uint32_t  kCheckTicks           = 30;   // the entry re-read twice a second at 60 while held, foreign, faulted or wanted
constexpr std::uint32_t  kStatusTicks          = 300;
constexpr std::uint8_t   kLayoutFavorTop       = 0;    // EThreePlayerSplitScreenType::FavorTop: the entry written
constexpr const wchar_t* kGameMapsSettingsPath = L"/Script/EngineSettings.Default__GameMapsSettings";

// Set at install, then only read.
std::uintptr_t g_engineGlobal = 0;
bool           g_installed    = false;

// Game thread.
int            g_override       = 0;        // the console's columns, 0 for none
int            g_columns        = 0;        // wanted at the last tick
int            g_localPlayers   = 0;        // the engine's local players at the last tick (the mod's count)
bool           g_written        = false;    // the entry holds the mod's columns
bool           g_reached        = false;    // the entry was read once (the first reconcile is done)
bool           g_foreign        = false;    // the entry holds neither the stock records nor the mod's (logged once)
std::uint8_t*  g_settingByte    = nullptr;  // the three-player layout setting, while held
std::uint8_t   g_settingWas     = 0;
bool           g_settingRefused = false;    // the setting could not be held (logged once, until the columns leave)
const wchar_t* g_waiting        = nullptr;  // why the entry cannot be reached, logged once per reason
const wchar_t* g_fault          = nullptr;  // a read or write that failed, logged once until it succeeds
std::uint32_t  g_ticks = 0, g_writes = 0, g_restores = 0;

/// A failed read or write, logged once per reason (state-change logging: the retry is the cadence's).
void Fault(const wchar_t* why) noexcept
{
    if (g_fault != why)
    {
        g_fault = why;
        platform::LogWarning(L"column layout: {}", why);
    }
}

/// The three-player entry's records in the live viewport client, or null with the reason. The table is the
/// constructor's: six entries, the three-player ones of three records each.
std::uint8_t* FindRecords(const wchar_t** why) noexcept
{
    std::uintptr_t engine = 0, client = 0, entries = 0, records = 0;
    std::int32_t   count  = 0;
    if (!Read(reinterpret_cast<const void*>(g_engineGlobal), engine) || engine == 0)
    {
        *why = L"GEngine is not set";
        return nullptr;
    }
    if (!Read(reinterpret_cast<const void*>(engine + facts::kEngineGameViewport), client) || client == 0)
    {
        *why = L"the engine has no game viewport client yet";
        return nullptr;
    }
    if (!Read(reinterpret_cast<const void*>(client + facts::kViewportClientSplitscreenInfo), entries) ||
        !Read(reinterpret_cast<const void*>(client + facts::kViewportClientSplitscreenInfo + facts::kTArrayNum), count) ||
        entries == 0 || count != facts::kSplitscreenTypes)
    {
        *why = L"the viewport client's layout table is not the constructor's six entries";
        return nullptr;
    }
    const std::uintptr_t entry = entries + facts::kSplitTypeThreePlayerFavorTop * facts::kSplitscreenDataBytes;
    if (!Read(reinterpret_cast<const void*>(entry), records) || !Read(reinterpret_cast<const void*>(entry + facts::kTArrayNum), count) ||
        records == 0 || count != kThreePlayerRecords)
    {
        *why = L"the three-player entry does not hold three records";
        return nullptr;
    }
    return reinterpret_cast<std::uint8_t*>(records);
}

std::wstring RecordsText(const Record (&records)[kThreePlayerRecords])
{
    std::wstring text;
    for (const Record& r : records)
    {
        text += std::format(L"{}({:.3f}, {:.3f} at {:.3f}, {:.3f})", text.empty() ? L"" : L" ", r.sizeX, r.sizeY, r.originX, r.originY);
    }
    return text;
}

/// The three-player layout setting held at FavorTop (the entry the mod writes) while the columns are in, and put
/// back when they leave if it still holds the mod's value. The engine re-reads it every layout pass.
void HoldSetting(bool hold) noexcept
{
    if (hold)
    {
        if (g_settingByte != nullptr || g_settingRefused)
        {
            return;
        }
        auto* settings = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UObject*>(nullptr, nullptr, kGameMapsSettingsPath);
        auto* byte     = settings == nullptr ? nullptr : reinterpret_cast<std::uint8_t*>(settings) + facts::kGameMapsThreePlayerLayout;
        std::uint8_t was = 0;
        if (byte == nullptr || !Read(byte, was))
        {
            g_settingRefused = true;
            platform::LogWarning(L"column layout: the game's layout settings were not found; the three-player setting is left as it is");
            return;
        }
        if (was > 1)
        {
            // The setting is a two-value enum (FavorTop, FavorBottom); anything else is not the byte the research read.
            g_settingRefused = true;
            platform::LogWarning(L"column layout: the three-player layout setting reads {}, not one of its two values; left alone", was);
            return;
        }
        if (was != kLayoutFavorTop && !platform::WriteGuarded(byte, &kLayoutFavorTop, 1))
        {
            g_settingRefused = true;
            platform::LogWarning(L"column layout: the three-player layout setting could not be written (it is {}); the columns need "
                                 L"player 1 on top's entry",
                                 was);
            return;
        }
        g_settingByte = byte;
        g_settingWas  = was;
        if (was != kLayoutFavorTop)
        {
            platform::Log(L"column layout: the engine's three-player layout setting set to player 1 on top (was {})", was);
        }
    }
    else
    {
        g_settingRefused = false;
        if (g_settingByte == nullptr)
        {
            return;
        }
        std::uint8_t now = 0;
        if (g_settingWas != kLayoutFavorTop)
        {
            if (Read(g_settingByte, now) && now == kLayoutFavorTop)
            {
                if (!platform::WriteGuarded(g_settingByte, &g_settingWas, 1))
                {
                    platform::LogWarning(L"column layout: the three-player layout setting could not be put back to {}; tried again at "
                                         L"the next leave",
                                         g_settingWas);
                    return;  // kept for the next release
                }
                platform::Log(L"column layout: the engine's three-player layout setting put back ({})", g_settingWas);
            }
            else
            {
                platform::Log(L"column layout: the three-player layout setting is {} now, not the mod's value; left as set", now);
            }
        }
        g_settingByte = nullptr;
    }
}

/// The entry as it should be for the columns wanted: written, restored, or left alone (logged).
void Apply(bool threeWanted) noexcept
{
    const wchar_t* why     = L"";
    std::uint8_t*  records = FindRecords(&why);
    if (records == nullptr)
    {
        if (g_waiting != why)
        {
            g_waiting = why;
            platform::LogDebug(L"column layout: the layout table cannot be reached: {}", why);
        }
        return;
    }
    g_waiting = nullptr;
    g_reached = true;
    Record entry[kThreePlayerRecords]{};
    if (!platform::CopyGuarded(entry, records, sizeof(entry)))
    {
        Fault(L"the engine's three-player layout could not be read; tried again twice a second");
        return;
    }
    const Action action = column_rules::Decide(entry, threeWanted);
    if (action != Action::Foreign && g_foreign)
    {
        g_foreign = false;
        platform::Log(L"column layout: the engine's three-player layout is {} again", action == Action::Restore || (action == Action::Nothing && threeWanted) ? L"the mod's three columns" : L"the stock one");
    }
    switch (action)
    {
        case Action::Write:
            if (platform::WriteGuarded(records, kThreeColumns, sizeof(kThreeColumns)))
            {
                g_written = true;
                g_fault   = nullptr;
                ++g_writes;
                HoldSetting(true);
                platform::Log(L"column layout: the engine's three-player layout now shows three equal columns side by side (was "
                              L"player 1 on top, two below); the engine lays three local players out from it on the next redraw "
                              L"(write {})",
                              g_writes);
            }
            else
            {
                Fault(L"the engine's three-player layout could not be written; three local players would take player 1 on top, two "
                      L"below; tried again twice a second");
            }
            break;
        case Action::Restore:
            if (platform::WriteGuarded(records, kStockFavorTop, sizeof(kStockFavorTop)))
            {
                g_written = false;
                g_fault   = nullptr;
                ++g_restores;
                HoldSetting(false);
                platform::Log(L"column layout: the engine's three-player layout put back to player 1 on top, two below (restore {})",
                              g_restores);
            }
            else
            {
                Fault(L"the engine's three-player layout could not be put back; it keeps the three columns; tried again twice a second");
            }
            break;
        case Action::Foreign:
            g_fault = nullptr;  // the entry was read: a read or write fault is over, the foreign content is the state now
            if (!g_foreign)
            {
                g_foreign = true;
                platform::LogWarning(L"column layout: the engine's three-player layout holds neither its stock values nor the mod's "
                                     L"three columns ({}); left alone{}",
                                     RecordsText(entry), g_written ? L"; the mod's columns were replaced, the setting released" : L"");
                if (g_written)
                {
                    g_written = false;
                    HoldSetting(false);
                }
            }
            break;
        case Action::Nothing:
            g_written = threeWanted;  // ours already, or the stock one with fewer wanted
            g_fault   = nullptr;
            HoldSetting(threeWanted);
            break;
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto engineGlobal = image.Rebase(facts::kGEngine, sizeof(std::uintptr_t));
    if (!engineGlobal)
    {
        platform::LogWarning(L"column layout: GEngine is outside the image; three columns are unavailable");
        return false;
    }
    g_engineGlobal = *engineGlobal;
    g_installed    = true;
    using control::Register;
    using control::Shape;
    using control::Value;
    Register({.name  = L"view-columns",
              .shape = Shape::Integer,
              .min   = 0,
              .max   = column_rules::kMaxColumns,
              .help  = L"the columns the window shows (full-height views side by side; 0: as the span and the players decide; "
                       L"never fewer than the players in): the mirror player fills the ones no player has",
              .set   = [](const Value& v) {
                  g_override             = static_cast<int>(std::get<std::int64_t>(v));
                  const std::wstring what = g_override == 0 ? L"as the span and the players decide" : std::format(L"{} asked for", g_override);
                  platform::Log(L"view columns: {}", what);
                  return control::Outcome{};
              },
              .get = [] { return Value{static_cast<std::int64_t>(g_override)}; }});
    Register({.name = L"column-layout.state", .shape = Shape::Text,
              .help = L"the columns wanted, the engine's three-player layout entry (stock, or the mod's three columns) and the setting",
              .get  = [] { return Value{Reading()}; }});
    platform::Log(L"column layout: ready (three columns through the engine's own layout table while three are wanted; "
                  L"view-columns 0..3)");
    return true;
}

void Tick(int columns, int localPlayers, int spanColumns) noexcept
{
    if (!g_installed)
    {
        return;
    }
    ++g_ticks;
    g_columns      = columns;
    g_localPlayers = localPlayers;
    // Columns on a span or by the console; in one window the engine's own entry (column_rules::ThreeColumns).
    const bool threeWanted = column_rules::ThreeColumns(columns, localPlayers, spanColumns, g_override);
    const bool change      = threeWanted != g_written && !g_foreign && g_fault == nullptr;
    const bool cadence     = g_ticks % kCheckTicks == 0 &&
                         (g_written || g_foreign || g_fault != nullptr || threeWanted || g_settingByte != nullptr);
    if (change || cadence || !g_reached)
    {
        Apply(threeWanted);
    }
    if (g_written && g_ticks % kStatusTicks == 0 && platform::Enabled(platform::Level::Debug))
    {
        platform::LogDebug(L"column layout: {}", Reading());
    }
}

int Override() noexcept
{
    return g_override;
}

void Remove() noexcept
{
    if (g_installed)
    {
        Apply(false);  // whatever the record says: the restore needs the exact mod's columns in the entry
    }
    HoldSetting(false);
}

std::wstring Reading()
{
    const wchar_t* why     = L"";
    std::uint8_t*  records = g_installed ? FindRecords(&why) : nullptr;
    Record         entry[kThreePlayerRecords]{};
    std::wstring   table = L"unread";
    if (records != nullptr && platform::CopyGuarded(entry, records, sizeof(entry)))
    {
        table = column_rules::Same(entry, kThreeColumns)  ? L"the mod's three columns"
                : column_rules::Same(entry, kStockFavorTop) ? L"stock (player 1 on top, two quadrants)"
                                                            : std::format(L"other: {}", RecordsText(entry));
    }
    return std::format(L"{} column(s) wanted{}, {} local player(s); the engine's three-player layout: {}{}; its setting held at player 1 "
                       L"on top: {}; writes {}, restores {}{}",
                       g_columns, g_override != 0 ? std::format(L" (view-columns {})", g_override) : L"", g_localPlayers, table,
                       records == nullptr && g_installed ? std::format(L" ({})", why) : L"", g_settingByte != nullptr ? L"yes" : L"no",
                       g_writes, g_restores, g_fault != nullptr ? std::format(L"; fault: {}", g_fault) : L"");
}

}  // namespace lss::views::column_layout
