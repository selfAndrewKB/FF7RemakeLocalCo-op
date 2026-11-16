#include "render/frame_capture.h"

#include "battle/enemy_watch.h"
#include "control/registry.h"
#include "platform/log.h"
#include "platform/name_text.h"
#include "platform/settings.h"
#include "players/field_root.h"
#include "render/capture_rules.h"
#include "views/column_layout.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace lss::render::frame_capture {

namespace {

namespace rules = capture_rules;

constexpr std::size_t  kRing          = 4096;  // the frames kept before a start: 5 s up to 800 fps
constexpr std::size_t  kRowsReserved  = 16384;
constexpr std::size_t  kRowBytes      = 96;    // a CSV row's length, about
constexpr int          kMostCaptures  = 30;    // a launch's captures
constexpr std::size_t  kMostActions   = 8;     // the actions a capture names
constexpr double       kResolveEveryMs = 1000.0;
constexpr const wchar_t* kDefaultActions = L"Beam07,Beam08";  // the Airbuster's beams in its last phase
constexpr std::array<const char*, kEngineNumbers> kEngineColumns{"game_ms", "render_ms", "present_ms", "gpu_ms"};
constexpr std::array<const char*, kFrameThreads>  kThreadColumns{"game_cpu_ms", "render_cpu_ms", "rhi_cpu_ms"};
constexpr std::array<const wchar_t*, kEngineNumbers> kEngineNames{L"game", L"render", L"present", L"GPU"};
constexpr std::array<const wchar_t*, kFrameThreads>  kThreadNames{L"game", L"render", L"RHI"};

enum class Segment : std::uint8_t
{
    Before,
    During,
    After,
};
constexpr std::array<const char*, 3>    kSegmentColumn{"before", "during", "after"};
constexpr std::array<const wchar_t*, 3> kSegmentNames{L"before", L"during", L"after"};

/// A frame as kept: its numbers, when it ended (ms of frames since install), whether a watched action ran, the views.
struct Kept
{
    Sample sample;
    double atMs   = 0.0;
    bool   action = false;
    int    views  = 1;
};
struct Row
{
    Kept    kept;
    Segment segment = Segment::Before;
};

// Game thread.
bool                       g_installed = false;
rules::Config              g_config;
rules::State               g_state;
bool                       g_drops = true;
std::vector<std::wstring>  g_names;  // the watched actions
std::vector<std::uint64_t> g_bits;   // their engine names (0: the engine has no such name yet)
double                     g_resolvedAtMs = -kResolveEveryMs;
std::array<Kept, kRing>    g_ring{};
std::size_t                g_ringNext  = 0;
std::size_t                g_ringCount = 0;
double                     g_nowMs     = 0.0;
std::vector<Row>           g_rows;
std::vector<std::uint64_t> g_seen;  // the watched actions seen in the running capture
rules::Reason              g_reason     = rules::Reason::None;
double                     g_baselineMs = 0.0;  // the baseline at the start
SYSTEMTIME                 g_startTime{};
int                        g_capturesLeft = kMostCaptures;
std::wstring               g_last         = L"no capture yet";

std::vector<std::wstring> SplitNames(std::wstring_view text)
{
    std::vector<std::wstring> names;
    while (!text.empty())
    {
        const std::size_t      comma = text.find(L',');
        std::wstring_view      part  = text.substr(0, comma);
        const std::size_t      first = part.find_first_not_of(L' ');
        const std::size_t      last  = part.find_last_not_of(L' ');
        if (first != std::wstring_view::npos)
        {
            names.emplace_back(part.substr(first, last - first + 1));
        }
        text = comma == std::wstring_view::npos ? std::wstring_view() : text.substr(comma + 1);
    }
    return names;
}

std::wstring JoinNames()
{
    std::wstring text;
    for (std::size_t i = 0; i < g_names.size(); ++i)
    {
        text += std::format(L"{}{}{}", i == 0 ? L"" : L", ", g_names[i], g_bits[i] == 0 ? L" (not loaded yet)" : L"");
    }
    return text.empty() ? std::wstring(L"none") : text;
}

void SetNames(std::wstring_view text)
{
    g_names = text == L"off" ? std::vector<std::wstring>() : SplitNames(text);
    g_bits.assign(g_names.size(), 0);
    g_resolvedAtMs = g_nowMs - kResolveEveryMs;
    battle::enemy_watch::WantActions(!g_names.empty());
}

/// Looks up the names the engine did not have yet, once a second: an enemy's action names load with the enemy.
void ResolveNames()
{
    if (g_nowMs - g_resolvedAtMs < kResolveEveryMs)
    {
        return;
    }
    g_resolvedAtMs = g_nowMs;
    for (std::size_t i = 0; i < g_names.size(); ++i)
    {
        if (g_bits[i] == 0)
        {
            g_bits[i] = platform::FindName(g_names[i].c_str());
        }
    }
}

void Append(const Kept& kept, Segment segment)
{
    g_rows.push_back(Row{kept, segment});
}

/// Starts a capture with `kept`, its first frame: the ring's frames from the last beforeMs come first.
void Begin(const Kept& kept)
{
    g_rows.clear();
    g_rows.reserve(kRowsReserved);
    g_reason     = g_state.reason;
    g_baselineMs = g_state.baselineMs;
    GetLocalTime(&g_startTime);
    const double from  = kept.atMs - kept.sample.frameMs - g_config.beforeMs;
    const std::size_t first = (g_ringNext + kRing - g_ringCount) % kRing;
    for (std::size_t i = 0; i < g_ringCount; ++i)
    {
        const Kept& old = g_ring[(first + i) % kRing];
        if (old.atMs > from)
        {
            Append(old, Segment::Before);
        }
    }
    Append(kept, Segment::During);
}

/// A segment's sums, for the Info line.
struct Stats
{
    std::vector<double>                frames;
    double                             sumMs = 0.0;
    double                             worstMs = 0.0;
    std::array<double, kEngineNumbers> engineSum{};
    std::array<int, kEngineNumbers>    engineCount{};
    std::array<double, kFrameThreads>  cpuSum{};
    std::array<int, kFrameThreads>     cpuCount{};
    double                             modSum      = 0.0;
    int                                modCount    = 0;
    double                             ribbonSum   = 0.0;
    int                                ribbonCount = 0;
};

std::wstring Average(double sum, int count)
{
    return count != 0 ? std::format(L"{:.2f}", sum / count) : std::wstring(L"-");
}

std::wstring StatsText(Segment segment, Stats& s)
{
    if (s.frames.empty())
    {
        return std::format(L"{}: no frames", kSegmentNames[static_cast<std::size_t>(segment)]);
    }
    const std::size_t at = s.frames.size() * 95 / 100;
    std::nth_element(s.frames.begin(), s.frames.begin() + static_cast<std::ptrdiff_t>(at), s.frames.end());
    std::wstring engine;
    for (std::size_t i = 0; i < kEngineNumbers; ++i)
    {
        engine += std::format(L"{}{} {}", i == 0 ? L"" : L", ", kEngineNames[i], Average(s.engineSum[i], s.engineCount[i]));
    }
    std::wstring cpu;
    for (std::size_t i = 0; i < kFrameThreads; ++i)
    {
        cpu += std::format(L"{}{} {}", i == 0 ? L"" : L", ", kThreadNames[i], Average(s.cpuSum[i], s.cpuCount[i]));
    }
    return std::format(L"{}: {} frames, {:.1f} fps, frame {:.2f} ms (95 % under {:.1f}, worst {:.1f}); the engine's {} ms; CPU {} ms; "
                       L"the mod's tick {} ms; the ribbon trails' fill {} ms",
                       kSegmentNames[static_cast<std::size_t>(segment)], s.frames.size(), s.frames.size() * 1000.0 / s.sumMs,
                       s.sumMs / static_cast<double>(s.frames.size()), s.frames[at], s.worstMs, engine, cpu,
                       Average(s.modSum, s.modCount), Average(s.ribbonSum, s.ribbonCount));
}

std::wstring ReasonText()
{
    std::wstring actions;
    for (const std::uint64_t bits : g_seen)
    {
        actions += (actions.empty() ? L"" : L", ") + platform::NameOrBits(bits);
    }
    if (g_reason == rules::Reason::Action)
    {
        return L"an enemy action (" + actions + L")";
    }
    return std::format(L"a frame-rate drop in battle (against a {:.2f} ms baseline){}", g_baselineMs,
                       actions.empty() ? std::wstring() : L"; actions seen: " + actions);
}

/// The file's text is ASCII: engine names and the mod's words are; anything else becomes '?'.
std::string Ascii(const std::wstring& text)
{
    std::string ascii(text.size(), '?');
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] < 0x80)
        {
            ascii[i] = static_cast<char>(text[i]);
        }
    }
    return ascii;
}

/// Appends one cell: a comma, then the ms (empty when unread).
void Cell(std::string& text, double ms)
{
    text += ',';
    if (ms >= 0.0)
    {
        std::format_to(std::back_inserter(text), "{:.3f}", ms);
    }
}

std::string CsvText(int views)
{
    std::string text = std::format("# frame capture {:04}-{:02}-{:02} {:02}:{:02}:{:02}\n", g_startTime.wYear, g_startTime.wMonth,
                                   g_startTime.wDay, g_startTime.wHour, g_startTime.wMinute, g_startTime.wSecond);
    text += "# trigger: " + Ascii(ReasonText()) + "\n";
    text +=std::format("# views at the start: {}\n", views);
    text += "ms,segment,frame_ms";
    for (const char* column : kEngineColumns)
    {
        text += std::string(",") + column;
    }
    for (const char* column : kThreadColumns)
    {
        text += std::string(",") + column;
    }
    text += ",mod_tick_ms,ribbon_fill_ms,action,views\n";
    text.reserve(text.size() + g_rows.size() * kRowBytes);
    auto         out    = std::back_inserter(text);
    const double origin = g_rows.empty() ? 0.0 : g_rows.front().kept.atMs;
    for (const Row& row : g_rows)
    {
        const Sample& s = row.kept.sample;
        std::format_to(out, "{:.3f},{},{:.3f}", row.kept.atMs - origin, kSegmentColumn[static_cast<std::size_t>(row.segment)], s.frameMs);
        for (const double ms : s.engineMs)
        {
            Cell(text, ms);
        }
        for (const double ms : s.cpuMs)
        {
            Cell(text, ms);
        }
        Cell(text, s.modMs);
        Cell(text, s.ribbonMs);
        std::format_to(out, ",{},{}\n", row.kept.action ? 1 : 0, row.kept.views);
    }
    return text;
}

/// Writes the capture's file; its path, or the reason it was not written.
std::wstring Write(int views)
{
    const std::wstring settings = platform::settings::Path();
    if (settings.empty())
    {
        return L"not written (the mod's folder is unread)";
    }
    const std::filesystem::path folder = std::filesystem::path(settings).parent_path() / L"captures";
    CreateDirectoryW(folder.c_str(), nullptr);
    const std::filesystem::path path =
        folder / std::format(L"frames-{:04}{:02}{:02}-{:02}{:02}{:02}-{}-{}view{}.csv", g_startTime.wYear, g_startTime.wMonth,
                             g_startTime.wDay, g_startTime.wHour, g_startTime.wMinute, g_startTime.wSecond,
                             g_reason == rules::Reason::Action ? L"action" : L"drop", views, views == 1 ? L"" : L"s");
    const std::string text = CsvText(views);
    const HANDLE      file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return std::format(L"not written (error {} creating {})", GetLastError(), path.wstring());
    }
    DWORD      written = 0;
    const auto bytes   = static_cast<DWORD>(text.size());
    const bool wrote   = WriteFile(file, text.data(), bytes, &written, nullptr) && written == bytes;
    const DWORD error  = wrote ? 0 : GetLastError();
    CloseHandle(file);
    return wrote ? path.wstring() : std::format(L"not fully written (error {}) to {}", error, path.wstring());
}

/// Drops whatever the running capture holds (its count stays taken). Never throws.
void Clear() noexcept
{
    g_rows.clear();
    g_rows.shrink_to_fit();
    g_seen.clear();
}

/// The capture's file and its Info line. May throw (allocation): the caller clears either way.
void Report()
{
    std::array<Stats, 3> stats;
    int                  fewest = 3;
    int                  most   = 1;
    for (const Row& row : g_rows)
    {
        Stats&        s  = stats[static_cast<std::size_t>(row.segment)];
        const Sample& in = row.kept.sample;
        s.frames.push_back(in.frameMs);
        s.sumMs += in.frameMs;
        s.worstMs = std::max(s.worstMs, in.frameMs);
        for (std::size_t i = 0; i < kEngineNumbers; ++i)
        {
            if (in.engineMs[i] >= 0.0)
            {
                s.engineSum[i] += in.engineMs[i];
                ++s.engineCount[i];
            }
        }
        for (std::size_t i = 0; i < kFrameThreads; ++i)
        {
            if (in.cpuMs[i] >= 0.0)
            {
                s.cpuSum[i] += in.cpuMs[i];
                ++s.cpuCount[i];
            }
        }
        if (in.modMs >= 0.0)
        {
            s.modSum += in.modMs;
            ++s.modCount;
        }
        if (in.ribbonMs >= 0.0)
        {
            s.ribbonSum += in.ribbonMs;
            ++s.ribbonCount;
        }
        if (row.segment != Segment::Before)
        {
            fewest = std::min(fewest, row.kept.views);
            most   = std::max(most, row.kept.views);
        }
    }
    const int          views = most;
    const std::wstring where = Write(views);
    const std::wstring viewsText =
        fewest == most ? std::format(L"{} view{}", most, most == 1 ? L"" : L"s") : std::format(L"{} to {} views", fewest, most);
    g_last = std::format(L"{} with {}, {:.1f} s from the start; {}; {}; {}; file: {}", ReasonText(), viewsText,
                         g_state.lengthMs / 1000.0, StatsText(Segment::Before, stats[0]), StatsText(Segment::During, stats[1]),
                         StatsText(Segment::After, stats[2]), where);
    platform::Log(L"frame capture: {}", g_last);
}

/// Ends the capture: counted first, then reported (a report that fails to allocate loses only itself), then cleared.
void Finish() noexcept
{
    if (--g_capturesLeft == 0)
    {
        platform::Log(L"frame capture: {} captures this launch, the most; no more until the next", kMostCaptures);
    }
    try
    {
        Report();
    }
    catch (...)
    {
        platform::LogWarning(L"frame capture: the capture could not be reported (out of memory)");
    }
    Clear();
}

void Keep(const Kept& kept)
{
    g_ring[g_ringNext] = kept;
    g_ringNext         = (g_ringNext + 1) % kRing;
    g_ringCount        = std::min(g_ringCount + 1, kRing);
}

std::wstring Reading()
{
    const wchar_t* phase = g_state.phase == rules::Phase::During ? L"capturing"
                           : g_state.phase == rules::Phase::After ? L"capturing the frames after"
                                                                  : L"waiting";
    return std::format(L"{}; watched actions: {}; drops in battle {}; baseline {:.2f} ms; {} captures left; last: {}", phase, JoinNames(),
                       g_drops ? L"on" : L"off", g_state.baselineMs, g_capturesLeft, g_last);
}

}  // namespace

bool Install(const platform::GameImage& /*image*/) noexcept
try
{
    SetNames(kDefaultActions);
    control::Register({.name  = L"frame-capture.actions",
                       .shape = control::Shape::Text,
                       .help  = L"the enemy actions that start a frame capture, comma-separated (engine names, as the enemy watch "
                                L"logs them; the Airbuster's beams at start), or off",
                       .set   = [](const control::Value& v) {
                           SetNames(std::get<std::wstring>(v));
                           return control::Outcome{true, L"watched actions: " + JoinNames()};
                       },
                       .get = [] { return control::Value{JoinNames()}; }});
    control::Register({.name  = L"frame-capture.drops",
                       .shape = control::Shape::OnOff,
                       .help  = L"whether a frame-rate drop in battle starts a frame capture (on at start)",
                       .set   = [](const control::Value& v) {
                           g_drops = std::get<bool>(v);
                           return control::Outcome{true, g_drops ? L"drops in battle start a capture" : L"drops start nothing"};
                       },
                       .get = [] { return control::Value{g_drops}; }});
    control::Register({.name  = L"frame-capture",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the frame capture: waiting or capturing, the watched actions, the baseline, and the "
                                L"last capture's comparison and file",
                       .get   = [] { return control::Value{Reading()}; }});
    g_installed = true;
    platform::Log(L"frame capture: installed; watched actions {}, and drops in battle: each capture's frames go to captures\\ "
                  L"beside the settings file with an Info line",
                  JoinNames());
    return true;
}
catch (...)
{
    platform::LogWarning(L"frame capture: could not be installed (out of memory)");
    return false;
}

void Add(const Sample& sample) noexcept
try
{
    if (!g_installed)
    {
        return;
    }
    g_nowMs += sample.frameMs;
    ResolveNames();
    const std::uint64_t action = battle::enemy_watch::RunningOneOf(g_bits);
    const Kept          kept{sample, g_nowMs, action != 0, views::column_layout::Views()};
    rules::Step         step = rules::Step::Idle;
    if (g_capturesLeft > 0)
    {
        const bool battle = g_drops && players::field_root::InBattle();
        step = rules::CaptureStep(g_state, g_config, {.frameMs = sample.frameMs, .action = action != 0, .battle = battle, .drops = g_drops});
    }
    if (step != rules::Step::Idle && action != 0 && g_seen.size() < kMostActions &&
        std::find(g_seen.begin(), g_seen.end(), action) == g_seen.end())
    {
        g_seen.push_back(action);
    }
    switch (step)
    {
    case rules::Step::Start:
        Begin(kept);
        platform::LogDebug(L"frame capture: started by {} with {} view(s)", ReasonText(), kept.views);
        break;
    case rules::Step::Capture:
        Append(kept, g_state.phase == rules::Phase::During ? Segment::During : Segment::After);
        break;
    case rules::Step::End:
        Append(kept, g_state.afterForMs > 0.0 ? Segment::After : Segment::During);
        Finish();
        break;
    case rules::Step::Idle:
        break;
    }
    Keep(kept);
}
catch (...)
{
    // Out of memory: the running capture is dropped whole (its baseline kept), and the next starts afresh.
    const double baselineMs = g_state.baselineMs;
    g_state                 = rules::State{};
    g_state.baselineMs = g_state.recentMs = baselineMs;
    Clear();
}

}  // namespace lss::render::frame_capture
