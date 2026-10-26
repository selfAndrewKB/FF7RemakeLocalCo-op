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
#include "platform/settings.h"
#include "views/column_rules.h"

#include <cstdint>
#include <format>

namespace lss::views::column_layout {

namespace {

using column_rules::Action;
using column_rules::ListSeats;
using column_rules::Order;
using column_rules::Records;
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
bool           g_written        = false;    // the entry holds one of the mod's entries (columns, or arranged)
Order          g_order{};                   // the column order (the setting, the console)
Records        g_wanted{};                  // the entry the last Apply wanted
ListSeats      g_seats{0, -1, -1};          // whose view each place of the engine's list held at the last tick
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

std::wstring OrderText(const Order& order);

/// The one setter of the column order: the file at install, the console after (`why` names which).
void SetOrder(const Order& order, const wchar_t* why)
{
    g_order = order;
    platform::Log(L"column layout: the column order is {}{} ({})", OrderText(g_order),
                  g_order == column_rules::kJoinOrder ? L", the join order" : L"", why);
}

std::wstring OrderText(const Order& order)
{
    return std::format(L"{} {} {}", order.player[0], order.player[1], order.player[2]);
}

/// The screens left to right as the wanted entry shows them: "player 1, player 3, the mirror".
std::wstring ScreensText(const Records& wanted, const Record (&base)[kThreePlayerRecords])
{
    std::wstring text;
    for (int slot = 0; slot < kThreePlayerRecords; ++slot)
    {
        for (int place = 0; place < kThreePlayerRecords; ++place)
        {
            if (wanted.record[place] == base[slot])
            {
                const int seat = g_seats[static_cast<std::size_t>(place)];
                text += (slot == 0 ? L"" : L", ") +
                        (seat >= 0                          ? std::format(L"player {}", seat + 1)
                         : place < g_localPlayers           ? std::wstring(L"the mirror")
                                                            : std::wstring(L"an empty screen"));
            }
        }
    }
    return text;
}

/// The three-player layout setting held at FavorTop (the entry the mod writes) while the mod's records are in, and put
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

/// The entry as it should be: `wanted` written, the stock one restored, or left alone (logged).
void Apply(const Records& wanted) noexcept
{
    g_wanted                  = wanted;
    const bool  ours          = !column_rules::Same(wanted.record, kStockFavorTop);
    const bool  columns       = column_rules::ArrangementOf(wanted.record, kThreeColumns);
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
    const Action action = column_rules::Decide(entry, wanted.record);
    if (action != Action::Foreign && g_foreign)
    {
        g_foreign = false;
        platform::Log(L"column layout: the engine's three-player layout is {} again",
                      column_rules::Same(entry, kStockFavorTop) ? L"the stock one" : L"one of the mod's");
    }
    switch (action)
    {
        case Action::Write:
            if (platform::WriteGuarded(records, wanted.record, sizeof(wanted.record)))
            {
                g_written = true;
                g_fault   = nullptr;
                ++g_writes;
                HoldSetting(true);
                platform::Log(L"column layout: the engine's three-player layout now shows {}, screens {}: {} (was {}); the engine "
                              L"lays three local players out from it on the next redraw (write {})",
                              columns ? L"three equal columns side by side" : L"player 1 on top, two below",
                              columns ? L"left to right" : L"top, bottom left, bottom right",
                              ScreensText(wanted, columns ? kThreeColumns : kStockFavorTop),
                              column_rules::Same(entry, kStockFavorTop) ? L"the stock one" : L"another of the mod's", g_writes);
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
                Fault(L"the engine's three-player layout could not be put back; it keeps the mod's records; tried again twice a second");
            }
            break;
        case Action::Foreign:
            g_fault = nullptr;  // the entry was read: a read or write fault is over, the foreign content is the state now
            if (!g_foreign)
            {
                g_foreign = true;
                platform::LogWarning(L"column layout: the engine's three-player layout holds neither its stock values nor one of the "
                                     L"mod's ({}); left alone{}",
                                     RecordsText(entry), g_written ? L"; the mod's columns were replaced, the setting released" : L"");
                if (g_written)
                {
                    g_written = false;
                    HoldSetting(false);
                }
            }
            break;
        case Action::Nothing:
            g_written = ours;  // the mod's already, or the stock one wanted
            g_fault   = nullptr;
            HoldSetting(ours);
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
    g_wanted       = Records{{kStockFavorTop[0], kStockFavorTop[1], kStockFavorTop[2]}};
    // The file's word is one of the table's, so it parses.
    SetOrder(column_rules::ParseOrder(platform::settings::Of(platform::settings::kColumnOrder).word).value_or(column_rules::kJoinOrder),
             platform::settings::Why(platform::settings::kColumnOrder).c_str());
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
    control::RegisterOption(
        platform::settings::kColumnOrder,
        [](const std::wstring& word) {
            const auto order = column_rules::ParseOrder(word);
            if (!order)
            {
                return control::Outcome{false, L"the order must be 123 or 132"};  // unreachable: the shape checks the words
            }
            SetOrder(*order, L"the console");
            return control::Outcome{};
        },
        [] { return std::format(L"{}{}{}", g_order.player[0], g_order.player[1], g_order.player[2]); });
    Register({.name = L"column-layout.state", .shape = Shape::Text,
              .help = L"the columns wanted, the column order, the engine's three-player layout entry (stock, the mod's three columns, "
                      L"or the stock records arranged) and the setting",
              .get  = [] { return Value{Reading()}; }});
    platform::Log(L"column layout: ready (three columns through the engine's own layout table while three are wanted; "
                  L"view-columns 0..3; column order {})",
                  OrderText(g_order));
    return true;
}

void Tick(int columns, int localPlayers, int spanColumns, const ListSeats& seats) noexcept
{
    if (!g_installed)
    {
        return;
    }
    ++g_ticks;
    g_columns      = columns;
    g_localPlayers = localPlayers;
    g_seats        = seats;
    // Columns on a span or by the console; in one window the engine's own entry (column_rules::ThreeColumns); either
    // arranged by the column order while three views exist.
    const bool    threeWanted = column_rules::ThreeColumns(columns, localPlayers, spanColumns, g_override);
    const bool    threeViews  = columns >= kThreePlayerRecords || localPlayers >= kThreePlayerRecords;
    const Records wanted      = column_rules::EntryWanted(threeWanted, threeViews, g_order, seats);
    const bool    ours        = !column_rules::Same(wanted.record, kStockFavorTop);
    const bool    change      = !column_rules::Same(wanted.record, g_wanted.record) && !g_foreign && g_fault == nullptr;
    const bool    cadence     = g_ticks % kCheckTicks == 0 &&
                         (g_written || g_foreign || g_fault != nullptr || ours || g_settingByte != nullptr);
    if (change || cadence || !g_reached)
    {
        Apply(wanted);
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
        Apply(Records{{kStockFavorTop[0], kStockFavorTop[1], kStockFavorTop[2]}});  // the restore needs one of the mod's entries in it
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
        table = column_rules::Same(entry, kStockFavorTop)            ? std::wstring(L"stock (player 1 on top, two quadrants)")
                : column_rules::ArrangementOf(entry, kThreeColumns)  ? L"the mod's three columns (screens left to right: " +
                                                                           ScreensText({{entry[0], entry[1], entry[2]}}, kThreeColumns) + L")"
                : column_rules::ArrangementOf(entry, kStockFavorTop) ? L"player 1 on top, two below, arranged (top, bottom left, "
                                                                       L"bottom right: " +
                                                                           ScreensText({{entry[0], entry[1], entry[2]}}, kStockFavorTop) + L")"
                                                                     : std::format(L"other: {}", RecordsText(entry));
    }
    return std::format(L"{} column(s) wanted{}, {} local player(s); column order {}; the engine's three-player layout: {}{}; its "
                       L"setting held at player 1 on top: {}; writes {}, restores {}{}",
                       g_columns, g_override != 0 ? std::format(L" (view-columns {})", g_override) : L"", g_localPlayers,
                       OrderText(g_order), table,
                       records == nullptr && g_installed ? std::format(L" ({})", why) : L"", g_settingByte != nullptr ? L"yes" : L"no",
                       g_writes, g_restores, g_fault != nullptr ? std::format(L"; fault: {}", g_fault) : L"");
}

}  // namespace lss::views::column_layout
