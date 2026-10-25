#include "render/area_blend.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/weak_object.h"

#include <Unreal/NameTypes.hpp>

#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace lss::render::area_blend {

namespace {

using platform::Read;
using platform::ReadPointer;

using GetWorldFn = void* (*)(void* scene);

constexpr std::int32_t  kMaxControllers = 4096;  // a longer list is not one this code reads
constexpr std::size_t   kMaxBanks       = 8;     // view states (local players) kept at once
constexpr std::uint64_t kBankIdleFrames = 2;     // a view state missing from this many drawn frames is dropped (a leave)
constexpr std::uint32_t kObjectName     = 0x18;  // UObjectBase::NamePrivate (stock 4.18)

/// One controller's value, kept in the order of the world's list (looked up at the same place first).
template <class T>
struct Entry
{
    const void* controller = nullptr;
    T           value{};
};

template <class T>
const T* Find(const std::vector<Entry<T>>& entries, std::size_t at, const void* controller) noexcept
{
    if (at < entries.size() && entries[at].controller == controller)
    {
        return &entries[at].value;
    }
    for (const Entry<T>& entry : entries)
    {
        if (entry.controller == controller)
        {
            return &entry.value;
        }
    }
    return nullptr;
}

/// The frame's pending time and snap byte of one controller, as the first view found them.
struct Pending
{
    float        time = 0.0f;
    std::uint8_t snap = 0;
};

/// One view state's timers, rebuilt from the world's list after each of its evaluations.
struct Bank
{
    const void*                 state     = nullptr;
    std::uint64_t               lastFrame = 0;  // the drawn frame it was last evaluated in
    std::vector<Entry<float>>   timers;
};

bool                         g_enabled      = true;   // switch area-blend
bool                         g_several      = false;  // more than one view is drawn
bool                         g_active       = false;  // the banks hold the views' timers
bool                         g_restore      = false;  // the first view's timers go back at the next evaluation
bool                         g_frameStarted = false;  // the frame's first view was evaluated
std::uint64_t                g_drawn        = 0;  // frames drawn (a tick with no view evaluation does not count)
const void*                  g_firstState   = nullptr;
const void*                  g_world        = nullptr;
const void*                  g_applied      = nullptr;  // the view state Before set up (After keeps only that one)
std::vector<void*>           g_list;   // the world's controllers, read at the view's Before
std::vector<Bank>            g_banks;
std::vector<Entry<Pending>>  g_frame;  // in the first view's list order
std::uint64_t                g_views = 0, g_worldChanges = 0, g_dropped = 0;

template <class T>
T Field(const void* object, std::uint32_t offset) noexcept
{
    T value{};
    Read(static_cast<const std::uint8_t*>(object) + offset, value);
    return value;
}

template <class T>
void Put(void* object, std::uint32_t offset, T value) noexcept
{
    platform::WriteGuarded(static_cast<std::uint8_t*>(object) + offset, &value, sizeof(value));
}

void NameText(std::int64_t bits, std::wstring* out) noexcept
try
{
    *out = RC::Unreal::FName(bits).ToString();
}
catch (...)
{
    out->clear();
}

std::wstring Name(const void* object) noexcept
{
    const auto   bits = Field<std::uint64_t>(object, kObjectName);
    std::wstring text;
    if (!platform::CallGuarded(&NameText, static_cast<std::int64_t>(bits), &text) || text.empty())
    {
        return std::format(L"name#{:x}", bits);
    }
    return text;
}

const void* WorldOf(const void* view) noexcept
{
    const void* family = ReadPointer(view, facts::kViewFamily);
    void*       scene  = family != nullptr ? ReadPointer(family, facts::kFamilyScene) : nullptr;
    const void* vtable = scene != nullptr ? ReadPointer(scene, 0) : nullptr;
    const auto  fn     = vtable != nullptr ? reinterpret_cast<GetWorldFn>(ReadPointer(vtable, facts::kSceneGetWorldSlot)) : nullptr;
    void*       world  = nullptr;
    return fn != nullptr && platform::CallGuarded(fn, world, scene) ? world : nullptr;
}

/// The world's controllers into g_list (its capacity kept). False if unreadable or empty.
bool ReadList(const void* world)
{
    const void*  data  = ReadPointer(world, facts::kWorldAreaControllers);
    std::int32_t count = 0;
    g_list.clear();
    if (data == nullptr || !Read(static_cast<const std::uint8_t*>(world) + facts::kWorldAreaControllerCount, count) || count <= 0 ||
        count > kMaxControllers)
    {
        return false;
    }
    g_list.resize(static_cast<std::size_t>(count));
    if (!platform::CopyGuarded(g_list.data(), data, g_list.size() * sizeof(void*)))
    {
        g_list.clear();
        return false;
    }
    std::erase(g_list, nullptr);
    return !g_list.empty();
}

Bank* FindBank(const void* state) noexcept
{
    for (Bank& bank : g_banks)
    {
        if (bank.state == state)
        {
            return &bank;
        }
    }
    return nullptr;
}

/// A new view state's bank, seeded from `seed`'s (the first view's: a mirror shows player 1's camera, a member that
/// joins stands near player 1; for a rebuilt first view, the one before it); null when the banks are full.
Bank* NewBank(const void* state, const void* seed)
{
    if (g_banks.size() >= kMaxBanks)
    {
        return nullptr;
    }
    const Bank* from = FindBank(seed);
    Bank        bank{state, g_drawn, from != nullptr ? from->timers : std::vector<Entry<float>>{}};
    g_banks.push_back(std::move(bank));
    return &g_banks.back();
}

bool Timed(const void* controller) noexcept
{
    const auto type = Field<std::uint8_t>(controller, facts::kRscBlendType);
    return type == facts::kRscTimeInBox || type == facts::kRscTimeInSphere;
}

/// The banks dropped; the first view's timers go back at the next evaluation, into the controllers listed then.
void Stop(const wchar_t* why) noexcept
{
    if (!g_active)
    {
        return;
    }
    g_active  = false;
    g_restore = true;
    platform::Log(L"area blend: the game's one fade timer per area controller again ({})", why);
}

/// At the first evaluation after Stop: the first view's timers into the controllers the world lists now (only those: a
/// controller streamed out since may have left its address to another object), then the banks dropped.
void Restore(const void* view)
{
    const void* world = WorldOf(view);
    const Bank* first = FindBank(g_firstState);
    if (world != nullptr && world == g_world && first != nullptr && ReadList(world))
    {
        for (std::size_t i = 0; i < g_list.size(); ++i)
        {
            if (const float* timer = Find(first->timers, i, g_list[i]))
            {
                Put(g_list[i], facts::kRscTimer, *timer);
            }
        }
    }
    g_banks.clear();
    g_frame.clear();
    g_restore = false;
}

void SetEnabled(bool on) noexcept
{
    g_enabled = on;
    if (!on)
    {
        Stop(L"switched off");
    }
}

std::wstring Reading()
{
    std::wstring out = std::format(L"switch {}; {}; views blended {}, world changes {}, view states dropped {}",
                                   g_enabled ? L"on" : L"off", g_active ? L"each view its own fade timers" : L"the game's one timer",
                                   g_views, g_worldChanges, g_dropped);
    for (std::size_t i = 0; i < g_banks.size(); ++i)
    {
        out += std::format(L"; bank {}: view state {}{}", i + 1, g_banks[i].state, g_banks[i].state == g_firstState ? L" (first)" : L"");
    }
    int shown = 0;
    for (std::size_t at = 0; at < g_list.size(); ++at)
    {
        const void* controller = g_list[at];
        if (!players::weak_object::Live(controller) || !Timed(controller))
        {
            continue;
        }
        if (++shown > 24)
        {
            out += L"; ...";
            break;
        }
        const auto  blend = Field<float>(controller, facts::kRscBlendTime);
        const void* owner = ReadPointer(controller, facts::kComponentOwner);
        out += std::format(L"; {} on {} (kind {}, blend {:.2f} s, the last view's weight {:.2f}):", Name(controller),
                           owner != nullptr ? Name(owner) : std::wstring(L"?"), Field<std::uint8_t>(controller, facts::kRscBlendType),
                           blend, Field<float>(controller, facts::kRscWeight));
        if (g_banks.empty())
        {
            out += std::format(L" timer {:.2f}", Field<float>(controller, facts::kRscTimer));
        }
        for (std::size_t i = 0; i < g_banks.size(); ++i)
        {
            const float* timer = Find(g_banks[i].timers, at, controller);
            out += std::format(L" bank {} {}", i + 1,
                               timer == nullptr ? std::wstring(L"-")
                                                : std::format(L"{:.0f}%", blend > 0.0f ? 100.0f * *timer / blend : 0.0f));
        }
    }
    if (shown == 0)
    {
        out += L"; no timed area controller in the last view's world";
    }
    return out;
}

}  // namespace

void Install(const platform::GameImage&) noexcept
{
    control::Register({.name  = L"area-blend",
                       .shape = control::Shape::OnOff,
                       .help  = L"each view takes the indoor or outdoor look of its own camera: the area controllers' fade timers "
                                L"per view (render/area_blend.h; on; it needs the shadows' call, shadows_per_view); off: the game's "
                                L"one timer, player 1's for every view",
                       .set   = [](const control::Value& value) {
                           SetEnabled(std::get<bool>(value));
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled}; }});
    control::Register({.name = L"render.area-blend", .shape = control::Shape::Text,
                       .help = L"(read only) the view states' banks, the timed area controllers of the world (kind 3 box, 4 "
                               L"sphere), their blend time and each bank's fade",
                       .get  = [] {
                           try
                           {
                               return control::Value{Reading()};
                           }
                           catch (...)
                           {
                               return control::Value{std::wstring(L"unread")};
                           }
                       }});
}

void Tick(bool severalViews) noexcept
{
    if (g_frameStarted)
    {
        ++g_drawn;  // the frame that ended drew views
    }
    g_frameStarted = false;
    // A view state missing from the last drawn frames is a player who left (each join makes new local players and
    // mirrors); ticks with nothing drawn (a minimized window, a load) age nothing.
    const auto idle = std::erase_if(g_banks, [](const Bank& bank) { return g_drawn - bank.lastFrame > kBankIdleFrames; });
    g_dropped += idle;
    if (idle != 0)
    {
        platform::LogDebug(L"area blend: {} view state(s) no longer drawn; their fade timers dropped", idle);
    }
    if (severalViews == g_several)
    {
        return;
    }
    g_several = severalViews;
    if (!severalViews)
    {
        Stop(L"one view drawn");
    }
}

void Before(const void* view, const void* state) noexcept
try
{
    g_applied = nullptr;
    if (g_restore)
    {
        Restore(view);
    }
    if (!g_enabled || !g_several || state == nullptr)
    {
        return;
    }
    const void* world = WorldOf(view);
    if (world == nullptr)
    {
        return;
    }
    if (world != g_world)
    {
        g_world = world;
        g_banks.clear();
        g_frame.clear();
        g_frameStarted = false;
        ++g_worldChanges;
        platform::LogDebug(L"area blend: a new world; every view's fade timers start from the game's");
    }
    if (!ReadList(world))
    {
        return;
    }
    const bool  first    = !g_frameStarted;  // the frame's first view: the one evaluated after the engine tick
    const void* previous = g_firstState;
    if (first)
    {
        g_firstState   = state;
        g_frameStarted = true;  // even if this view goes no further: a later view must not take the zeroed pending time
        g_frame.clear();
    }
    Bank* bank = FindBank(state);
    if (bank == nullptr)
    {
        bank = NewBank(state, first ? previous : g_firstState);
    }
    if (bank == nullptr)
    {
        return;  // more view states than kept: this view takes the controllers as they are
    }
    if (!g_active)
    {
        g_active = true;
        platform::Log(L"area blend: each view's area fades (indoor and outdoor looks) follow its own camera");
    }
    bank->lastFrame = g_drawn;
    if (first)
    {
        g_frame.resize(g_list.size());
    }
    ++g_views;
    for (std::size_t i = 0; i < g_list.size(); ++i)
    {
        void* controller = g_list[i];
        if (first)
        {
            g_frame[i] = {controller, {Field<float>(controller, facts::kRscPending), Field<std::uint8_t>(controller, facts::kRscSnap)}};
        }
        else if (const Pending* pending = Find(g_frame, i, controller))
        {
            Put(controller, facts::kRscPending, pending->time);
            Put(controller, facts::kRscSnap, pending->snap);
        }
        if (const float* timer = Find(bank->timers, i, controller))
        {
            Put(controller, facts::kRscTimer, *timer);
        }
        else if (!first)
        {
            Put(controller, facts::kRscTimer, 0.0f);  // a controller new to this view starts as the game's constructor does
        }
    }
    g_applied = state;
}
catch (...)
{
    g_applied = nullptr;
    platform::LogWarning(L"area blend: a view's timers could not be set (out of memory)");
}

void After(const void*, const void* state) noexcept
try
{
    if (state == nullptr || state != g_applied)
    {
        return;  // Before did not set this view up: nothing of it to keep
    }
    g_applied = nullptr;
    Bank* bank = FindBank(state);
    if (bank == nullptr)
    {
        return;
    }
    bank->timers.resize(g_list.size());
    for (std::size_t i = 0; i < g_list.size(); ++i)
    {
        bank->timers[i] = {g_list[i], Field<float>(g_list[i], facts::kRscTimer)};
    }
    platform::LogTrace(L"area blend: view state {} kept {} controller timer(s)", state, g_list.size());
}
catch (...)
{
    platform::LogWarning(L"area blend: a view's timers could not be kept (out of memory)");
}

}  // namespace lss::render::area_blend
