#include "render/occlusion_exempt.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/name_text.h"
#include "platform/verified_code.h"
#include "render/family_now.h"
#include "render/index_ranges.h"
#include "render/view_places.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <format>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace lss::render::occlusion_exempt {

namespace {

using RangeFn = std::uintptr_t (*)(void* params);  // the game's result (none is used) passes through

constexpr std::size_t kMostNamed = 32;  // a set this small or smaller is logged with its objects' names
constexpr int kMainSceneGone = 64;  // passes without the main scene after which another may take its place
constexpr std::int32_t kThirdColumn = 2;  // the third column's index (its rectangle's left edge / its width)

// Set once at install, on the game thread, before the redirect is live; read on the render thread afterwards.
platform::CallRedirect g_call;
RangeFn                g_range = nullptr;
std::atomic<bool>      g_installed{false};

/// One set of objects: as the command gave it (written on the game thread, taken by the render thread when the
/// generation moves) and as the render thread uses it.
struct Slot
{
    const wchar_t* label = L"";  // after "occlusion exempt" in its log lines

    std::mutex                       lock;
    std::vector<index_ranges::Range> set;            // guarded by lock
    std::wstring                     text = L"none";  // guarded by lock
    std::atomic<std::uint32_t>       generation{0};

    // Render thread only.
    std::vector<index_ranges::Range> ranges;          // the set in use
    std::uint32_t                    taken  = 0;      // the generation ranges came from
    bool                             report = false;  // the next main-scene pass logs the new set

    std::atomic<std::int64_t> lastExempted{0};  // objects the main scene's last pass exempted (read by the command)
    std::atomic<std::int64_t> lastObjects{0};   // objects in it
};

Slot g_main;  // the views occlusion-exempt.views allows (not a third column with a set of its own)
Slot g_third{.label = L" (third column)"};  // the third column's view, while it has a set of its own
std::atomic<bool> g_thirdOwn{false};

/// Which views the main set applies in: player 1's is the first view of the engine's list, told by its state
/// (render/view_places), or, with no list known, the view whose rectangle starts at the window's top-left corner.
enum class Views : int
{
    All,
    PlayerOne,
    Others,
};
constexpr const wchar_t* kViewWords[] = {L"all", L"player-1", L"others"};
static_assert(std::size(kViewWords) == static_cast<std::size_t>(Views::Others) + 1,
              "one word per Views value, in order");
std::atomic<Views> g_views{Views::All};

// Render thread only.
std::vector<std::uint8_t> g_flags;                  // the scene's flags, copied each pass
std::vector<std::int32_t> g_cleared;                // the objects this pass cleared, to put back
std::uint8_t*             g_clearedIn   = nullptr;  // the flag array they were cleared in
bool                      g_warnedMoved = false;
// The sets' indices are the main scene's: the one with the most objects (scene and reflection captures of the same
// world share it; a preview scene would have its own, smaller). Only its passes are reported and read.
const void*  g_mainScene   = nullptr;
std::int32_t g_mainObjects = 0;
int          g_sinceMain   = 0;  // passes since the main scene's last

template <class T>
bool ReadAt(const void* base, std::size_t offset, T& out) noexcept
{
    return base != nullptr && platform::CopyGuarded(&out, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
}

/// The scene's occlusion flags: one byte per object, in the order of the scene's object list.
struct Flags
{
    std::uint8_t* data  = nullptr;
    std::int32_t  count = 0;
};

Flags SceneFlags(const void* scene) noexcept
{
    Flags flags;
    if (!ReadAt(scene, facts::kSceneOcclusionFlags, flags.data) ||
        !ReadAt(scene, facts::kSceneOcclusionFlagCount, flags.count) || flags.data == nullptr || flags.count < 0)
    {
        return {};
    }
    return flags;
}

/// "index owner (resource)" of a scene object, for the log.
std::wstring Describe(const void* scene, std::int32_t index)
{
    const void*  const* infos = nullptr;
    std::int32_t        count = 0;
    const void*         info  = nullptr;
    const void*         proxy = nullptr;
    std::int64_t        owner = 0;
    std::int64_t        res   = 0;
    std::wstring        ownerText;
    std::wstring        resText;
    if (ReadAt(scene, facts::kScenePrimitives, infos) && ReadAt(scene, facts::kScenePrimitives + 8, count) &&
        index < count && ReadAt(infos, static_cast<std::size_t>(index) * sizeof(void*), info) &&
        ReadAt(info, facts::kSceneInfoProxy, proxy))
    {
        if (ReadAt(proxy, facts::kProxyOwnerName, owner))
        {
            ownerText = platform::NameText(static_cast<std::uint64_t>(owner));
        }
        if (ReadAt(proxy, facts::kProxyResourceName, res))
        {
            resText = platform::NameText(static_cast<std::uint64_t>(res));
        }
    }
    return std::format(L"{} {} ({})", index, ownerText.empty() ? L"?" : ownerText, resText.empty() ? L"?" : resText);
}

/// Takes the command's newest set for the slot, if it changed.
void TakeSet(Slot& slot)
{
    if (slot.generation.load(std::memory_order_acquire) == slot.taken)
    {
        return;
    }
    const std::lock_guard lock(slot.lock);
    slot.ranges = slot.set;
    slot.taken  = slot.generation.load(std::memory_order_relaxed);  // under the lock: the set and its generation match
    slot.report = true;
}

/// Whether this pass is the main scene's (adopting a bigger scene, or any scene once the main one is gone).
bool IsMainScene(const void* scene, std::int32_t objects) noexcept
{
    if (scene != g_mainScene && (objects > g_mainObjects || ++g_sinceMain > kMainSceneGone))
    {
        g_mainScene = scene;
    }
    if (scene != g_mainScene)
    {
        return false;
    }
    g_mainObjects = objects;
    g_sinceMain   = 0;
    return true;
}

/// The views a slot's set applies in, for the log.
std::wstring Where(const Slot& slot)
{
    if (&slot == &g_third)
    {
        return L"in the third column's view";
    }
    const bool third = g_thirdOwn.load(std::memory_order_relaxed);
    switch (g_views.load(std::memory_order_relaxed))
    {
    case Views::PlayerOne: return L"in player 1's view";
    case Views::Others:
        return third ? L"in the views other than player 1's and the third column's"
                     : L"in the views other than player 1's";
    default: return third ? L"in every view but the third column's" : L"in every view";
    }
}

/// For the main scene's passes: the new set's line (Info), a change in how many objects a pass exempts (Trace: the
/// game rewrites some flags every frame, so the count may move with it).
void Report(const void* scene, std::int32_t objects, Slot& slot)
{
    if (!IsMainScene(scene, objects))
    {
        return;
    }
    const auto exempted = static_cast<std::int64_t>(g_cleared.size());
    const auto previous = slot.lastExempted.exchange(exempted);
    slot.lastObjects.store(objects);
    if (slot.report)
    {
        slot.report = false;
        if (slot.ranges.empty())
        {
            platform::Log(L"occlusion exempt{}: none; culling hides objects as the game decides", slot.label);
            return;
        }
        std::wstring names;
        if (g_cleared.size() <= kMostNamed)
        {
            for (std::size_t i = 0; i < g_cleared.size(); ++i)
            {
                names += (i == 0 ? L": " : L", ") + Describe(scene, g_cleared[i]);
            }
        }
        platform::Log(L"occlusion exempt{}: {} of {} objects skip their occlusion test and count as visible, {} "
                      L"(test; objects the game never tests are not counted){}",
                      slot.label, exempted, objects, Where(slot), names);
        return;
    }
    if (exempted != previous)
    {
        platform::LogTrace(L"occlusion exempt{}: a pass now exempts {} of {} objects", slot.label, exempted, objects);
    }
}

/// Clears "can be occluded" on the slot's objects that have it, recording them for PutBack.
void Clear(const void* scene, Slot& slot)
{
    g_cleared.clear();
    g_clearedIn = nullptr;
    TakeSet(slot);
    if (slot.ranges.empty() && !slot.report)
    {
        return;
    }
    const Flags flags = SceneFlags(scene);
    g_flags.resize(static_cast<std::size_t>(flags.count));
    if (flags.count == 0 || !platform::CopyGuarded(g_flags.data(), flags.data, g_flags.size()))
    {
        return;
    }
    g_cleared.reserve(g_flags.size());  // before any write: a cleared bit is always recorded, so always put back
    g_clearedIn = flags.data;
    for (const index_ranges::Range& range : slot.ranges)
    {
        const std::int32_t end = std::min(range.to, flags.count);
        for (std::int32_t i = range.from; i < end; ++i)
        {
            std::uint8_t& byte = g_flags[static_cast<std::size_t>(i)];
            if ((byte & facts::kOcclusionCanBeOccluded) == 0)
            {
                continue;  // never tested anyway, or already cleared by an overlapping range
            }
            const auto cleared = static_cast<std::uint8_t>(byte & ~facts::kOcclusionCanBeOccluded);
            if (platform::WriteGuarded(flags.data + i, &cleared, 1))
            {
                byte = cleared;
                g_cleared.push_back(i);
            }
        }
    }
    Report(scene, flags.count, slot);
}

/// Puts back the bits Clear took, in the array they were taken from.
void PutBack(const void* scene)
{
    if (g_cleared.empty())
    {
        return;
    }
    const Flags flags = SceneFlags(scene);
    if (flags.data != g_clearedIn)
    {
        if (!g_warnedMoved)
        {
            g_warnedMoved = true;
            platform::LogWarning(L"occlusion exempt: the scene's flags moved during the game's pass; {} objects keep "
                                 L"\"never hidden\" until the game writes their flags again",
                                 g_cleared.size());
        }
        g_cleared.clear();
        return;
    }
    for (const std::int32_t i : g_cleared)
    {
        std::uint8_t byte = 0;
        if (i < flags.count && platform::CopyGuarded(&byte, flags.data + i, 1))
        {
            byte |= facts::kOcclusionCanBeOccluded;
            platform::WriteGuarded(flags.data + i, &byte, 1);
        }
    }
    g_cleared.clear();
}

void ClearGuarded(const void* scene, Slot& slot) noexcept
try
{
    Clear(scene, slot);
}
catch (...)
{
}

void PutBackGuarded(const void* scene) noexcept
try
{
    PutBack(scene);
}
catch (...)
{
}

/// The slot whose set applies in this pass's view (params +8, its rectangle), or none. The third column's view takes
/// its own set while it has one; otherwise a view takes the main set if occlusion-exempt.views allows it. A failed
/// read: none.
Slot* SlotFor(const void* params) noexcept
{
    const Views views    = g_views.load(std::memory_order_relaxed);
    const bool  thirdOwn = g_thirdOwn.load(std::memory_order_relaxed);
    if (views == Views::All && !thirdOwn)
    {
        return &g_main;
    }
    const void* view = nullptr;
    if (!ReadAt(params, 8, view))
    {
        return nullptr;
    }
    // A split frame (render/family_now.h): every player's view is at (0, 0) of its own family; its place says who it is.
    // A reflection drawn inside the family is not the player's view and keeps the rule below.
    if (const family_now::Family family = family_now::For(view); family.index >= 0)
    {
        if (thirdOwn && family.index == kThirdColumn)
        {
            return &g_third;
        }
        const bool first = family.index == 0;
        return views == Views::All || first == (views == Views::PlayerOne) ? &g_main : nullptr;
    }
    // Outside a split frame: the view's place in the engine's list; a view no player's (a scene capture) is one of the others.
    if (const int place = view_places::PlaceOf(view); place != view_places::kUnknown)
    {
        if (thirdOwn && place == kThirdColumn)
        {
            return &g_third;
        }
        const bool first = place == 0;
        return views == Views::All || first == (views == Views::PlayerOne) ? &g_main : nullptr;
    }
    std::int32_t minX = 0;
    std::int32_t minY = 0;
    std::int32_t maxX = 0;
    if (!ReadAt(view, facts::kViewRectMinX, minX) ||
        !ReadAt(view, facts::kViewRectMinY, minY) || !ReadAt(view, facts::kViewRectMaxX, maxX))
    {
        return nullptr;
    }
    const std::int64_t width = std::int64_t{maxX} - minX;  // 64-bit: a torn read cannot overflow
    // The third column: a top-row rectangle starting on the boundary two widths in (a pixel either way for rounding).
    if (thirdOwn && width > 0 && minY == 0 && minX / width == kThirdColumn && minX % width <= 1)
    {
        return &g_third;
    }
    const bool playerOne = minX == 0 && minY == 0;
    return views == Views::All || playerOne == (views == Views::PlayerOne) ? &g_main : nullptr;
}

/// The game's call for one view, between the set's flags cleared and put back. Params +0 is the scene, +8 the view.
/// (A fault inside the game's function would leave the bits cleared, but it ends the process anyway.)
std::uintptr_t FetchRange(void* params)
{
    Slot*       slot  = SlotFor(params);
    const void* scene = nullptr;
    if (slot == nullptr || !ReadAt(params, 0, scene))
    {
        return g_range(params);
    }
    ClearGuarded(scene, *slot);
    const std::uintptr_t result = g_range(params);
    PutBackGuarded(scene);
    return result;
}

/// Stores a new set in the slot for the render thread. False when the text is not a set.
control::Outcome SetSlot(Slot& slot, const std::wstring& text)
{
    auto ranges = index_ranges::Parse(text);
    if (!ranges)
    {
        return {false, L"wants pairs of object indices \"a b\" (from a, up to but not including b) separated by "
                       L"commas, or none"};
    }
    std::wstring shown;
    for (const index_ranges::Range& range : *ranges)
    {
        shown += std::format(L"{}{} {}", shown.empty() ? L"" : L", ", range.from, range.to);
    }
    if (shown.empty())
    {
        shown = L"none";
    }
    else
    {
        shown += std::format(L" ({} indices)", index_ranges::Count(*ranges, INT32_MAX));
    }
    {
        const std::lock_guard lock(slot.lock);
        slot.set  = std::move(*ranges);
        slot.text = shown;
    }
    slot.generation.fetch_add(1, std::memory_order_release);
    platform::Log(L"occlusion exempt{}: set to {} (taken at the next frame)", slot.label, shown);
    return {};
}

std::wstring Reading(Slot& slot)
{
    std::wstring set;
    {
        const std::lock_guard lock(slot.lock);
        set = slot.text;
    }
    const std::int64_t objects = slot.lastObjects.load();
    if (objects == 0)
    {
        return set + L"; no pass yet";
    }
    return std::format(L"{}; its last pass ({}) exempted {} of {} objects", set, Where(slot), slot.lastExempted.load(),
                       objects);
}

control::Outcome SetMain(const control::Value& value)
try
{
    return SetSlot(g_main, std::get<std::wstring>(value));
}
catch (...)
{
    return {false, L"the set could not be stored"};
}

/// `main` gives the third column back to the main set; anything else is a set of its own (none included).
control::Outcome SetThird(const control::Value& value)
try
{
    const std::wstring& text = std::get<std::wstring>(value);
    if (text == L"main")
    {
        if (g_thirdOwn.exchange(false, std::memory_order_relaxed))
        {
            platform::Log(L"occlusion exempt (third column): takes the main set again");
        }
        return {};
    }
    control::Outcome outcome = SetSlot(g_third, text);
    if (outcome.ok)
    {
        g_thirdOwn.store(true, std::memory_order_relaxed);
    }
    return outcome;
}
catch (...)
{
    return {false, L"the set could not be stored"};
}

control::Value GetMain()
try
{
    return control::Value{Reading(g_main)};
}
catch (...)
{
    return control::Value{std::wstring{}};
}

control::Value GetThird()
try
{
    return control::Value{g_thirdOwn.load() ? Reading(g_third) : std::wstring(L"main")};
}
catch (...)
{
    return control::Value{std::wstring{}};
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto target = platform::FindFunction(image, facts::kFetchVisibilityRange);
    const auto site   = image.Rebase(facts::kFetchVisibilityRangeCall, 5);
    if (!target || !site)
    {
        platform::LogWarning(L"occlusion exempt: an engine check failed; the occlusion-exempt test is not available");
        return false;
    }
    g_range = reinterpret_cast<RangeFn>(*target);
    if (!g_call.Install(*site, *target, reinterpret_cast<std::uintptr_t>(&FetchRange)))
    {
        platform::LogWarning(L"occlusion exempt: the call is not the call the research read; the occlusion-exempt test "
                             L"is not available");
        return false;
    }
    g_installed.store(true);
    control::Register({.name  = L"occlusion-exempt",
                       .shape = control::Shape::Text,
                       .help  = L"test: objects never hidden by occlusion culling, in the views occlusion-exempt.views "
                                L"allows (all by default; a third column with a set of its own takes that): scene "
                                L"object indices \"a b, c d\" (from a, up to but not including b), or none "
                                L"(render/occlusion_exempt.h)",
                       .set   = &SetMain,
                       .get   = &GetMain});
    control::Register({.name  = L"occlusion-exempt.views",
                       .shape = control::Shape::Word,
                       .words = {std::begin(kViewWords), std::end(kViewWords)},
                       .help  = L"test: the views occlusion-exempt applies in: all, player-1 (the view at the window's "
                                L"top-left corner) or others (render/occlusion_exempt.h)",
                       .set   = [](const control::Value& value) {
                           const auto& word = std::get<std::wstring>(value);
                           const auto  views = word == kViewWords[1] ? Views::PlayerOne
                                               : word == kViewWords[2] ? Views::Others
                                                                       : Views::All;
                           g_views.store(views, std::memory_order_relaxed);  // guards no other data
                           platform::Log(L"occlusion exempt: applies in {} views", word);
                           return control::Outcome{};
                       },
                       .get   = [] {
                           return control::Value{std::wstring(kViewWords[static_cast<int>(g_views.load())])};
                       }});
    control::Register({.name  = L"occlusion-exempt.third-column",
                       .shape = control::Shape::Text,
                       .help  = L"test: a set of its own for the third column's view (\"a b, c d\" or none), so one "
                                L"look tests two sets; main gives it the main set again (render/occlusion_exempt.h)",
                       .set   = &SetThird,
                       .get   = &GetThird});
    platform::Log(L"occlusion exempt: installed (command occlusion-exempt <a b, c d> | none, off until set; "
                  L"occlusion-exempt.views all | player-1 | others; occlusion-exempt.third-column <a b, c d> | none | main)");
    return true;
}

void Remove() noexcept
{
    if (!g_installed.exchange(false))
    {
        return;
    }
    if (!g_call.Restore())
    {
        platform::LogWarning(L"occlusion exempt: the call was changed after the test; it is left as found");
    }
}

}  // namespace lss::render::occlusion_exempt
