#include "hud/view_window.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/game_array.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/scopes.h"
#include "players/seats.h"
#include "players/weak_object.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace lss::hud::view_window {

namespace {

namespace weak = players::weak_object;

using CreateFn     = void* (*)(void* world);
using AddToRootFn  = bool (*)(void* widget, std::int32_t zOrder);
using InRootFn     = bool (*)(void* widget);
using SetTextFn    = void (*)(void* window, const void* text);
using SetOptionsFn = void (*)(void* window, const void* options, std::uint32_t leftAligned);
using OpenFn       = void (*)(void* window, std::int32_t startIndex);
using WindowFn     = void (*)(void* window);  // PopFromFocusStack, Close
using RemoveFn     = void* (*)(void* widget);  // out of the HUD overlay
using ListAddFn    = void* (*)(void* list, std::int32_t* id, void** window);
using PlaySoundFn  = void (*)(const void* args);
using SourceFn     = void* (*)();
using GetWorldFn   = void* (*)(void* object);

constexpr int kPlayers = players::scopes::kMaxPlayers;

using platform::GameArray;

/// The game's functions and globals, found at install.
struct Game
{
    CreateFn       create     = nullptr;
    AddToRootFn    addToRoot  = nullptr;
    InRootFn       inRoot     = nullptr;
    SetTextFn      setText    = nullptr;
    SetOptionsFn   setOptions = nullptr;
    OpenFn         open       = nullptr;
    WindowFn       pop        = nullptr;
    WindowFn       close      = nullptr;
    RemoveFn       remove     = nullptr;
    ListAddFn      listAdd    = nullptr;
    PlaySoundFn    playSound  = nullptr;
    SourceFn       source     = nullptr;
    std::uintptr_t ui         = 0;  // EndUISystem's array of managers
    std::uintptr_t engine     = 0;  // &GEngine
    std::array<std::uintptr_t, 3> cues{};  // the sounds' FNames, by Sound
};

/// A window the mod opened, by the seat whose view shows it. `object` is compared first; the weak pair confirms it is
/// still that object (a freed window's address may hold another widget).
struct Window
{
    int          seat = -1;
    void*        object = nullptr;
    weak::Weak   pair;
    std::wstring text;
};

// Set at install, then only read.
Game g;
bool g_installed = false;

// Game thread.
std::array<Window, kPlayers> g_open;     // each seat's open window (seat -1: none)
std::vector<Window>          g_closing;  // closed ones still playing Out
std::uint64_t                g_opened = 0, g_changes = 0, g_failed = 0, g_gameClosed = 0, g_inScope = 0;
bool                         g_failLogged = false;

void* Ui(std::uint32_t slot) noexcept
{
    return g.ui != 0 ? platform::ReadPointer(reinterpret_cast<const void*>(g.ui), slot * 8) : nullptr;
}

std::uint8_t* At(void* object, std::uint32_t offset) noexcept
{
    return static_cast<std::uint8_t*>(object) + offset;
}

/// The world the HUD lives in: the game viewport client's (UObject::GetWorld).
void* World() noexcept
{
    void* const engine = platform::ReadPointer(reinterpret_cast<const void*>(g.engine), 0);
    void* const client = engine != nullptr ? platform::ReadPointer(engine, facts::kEngineGameViewport) : nullptr;
    void* const table  = client != nullptr ? platform::ReadPointer(client, 0) : nullptr;
    const auto  get    = table != nullptr ? reinterpret_cast<GetWorldFn>(platform::ReadPointer(table, facts::kObjectGetWorldSlot)) : nullptr;
    void*       world  = nullptr;
    return get != nullptr && platform::CallGuarded(get, world, client) ? world : nullptr;
}

/// The focus context's layer count (-1 unreadable): the open's log shows the pop left them as they were.
std::int32_t FocusLayers() noexcept
{
    std::int32_t count = -1;
    void* const  owner = Ui(facts::kFocusContextSlot);
    return owner != nullptr && platform::Read(At(owner, facts::kFocusLayerCount), count) ? count : -1;
}

/// The window still lives as the one recorded, in the overlay, not closed by the game.
bool Alive(const Window& w) noexcept
{
    if (w.object == nullptr || weak::Resolve(w.pair) != w.object)
    {
        return false;
    }
    bool         inRoot = false;
    std::int32_t state  = 0;
    return platform::CallGuarded(g.inRoot, inRoot, w.object) && inRoot &&
           platform::Read(At(w.object, facts::kMessageWindowState), state) && state != 0;
}

bool SetText(void* window, const std::wstring& text) noexcept
{
    const GameArray view = platform::SourceString(text);  // EndText_ResolveTextIdOrLiteral copies it
    return platform::CallGuarded(g.setText, window, static_cast<const void*>(&view));
}

/// The game's sequence for one window (EndUI_ShowMessageWindow's, §12.9), its focus layer popped at once. Refused inside
/// a player's scope: Open pushes onto whichever focus context is current, and the swapped one is that player's copy.
void* OpenWindow(int seat, const std::wstring& text) noexcept
{
    if (players::scopes::OpenPlayer(players::scopes::All) != 0)
    {
        ++g_inScope;
        return nullptr;
    }
    void* const world = World();
    void*       w     = nullptr;
    if (world == nullptr || !platform::CallGuarded(g.create, w, world) || w == nullptr)
    {
        return nullptr;
    }
    const std::int32_t z = facts::kClaimWindowZOrder;
    bool               added = false;
    if (!platform::WriteGuarded(At(w, facts::kEndWidgetZOrder), &z, sizeof(z)) ||
        !platform::CallGuarded(g.addToRoot, added, w, z) || !added)
    {
        platform::LogWarning(L"view window: {}'s window was made but could not be added to the HUD overlay",
                             players::SeatName(seat));
        return nullptr;  // never opened: no layer, no sound; the game collects it
    }
    const GameArray    none{};
    const std::int32_t before = FocusLayers();
    const bool         filled = SetText(w, text) &&
                        platform::CallGuarded(g.setOptions, w, static_cast<const void*>(&none), std::uint32_t{1});
    const bool opened = filled && platform::CallGuarded(g.open, w, std::int32_t{0});
    const bool popped = opened && platform::CallGuarded(g.pop, w);
    const std::int32_t after = FocusLayers();
    if (!popped)
    {
        // Never opened (state 0): no layer, no Out, and the game's close would do nothing; taken out of the overlay
        // directly. Opened (a fault in or after the open): the game's close pops any layer it pushed and plays Out, and
        // the window is kept as closing so it stays in its own player's column until its tick removes it.
        std::int32_t state = 0;
        platform::Read(At(w, facts::kMessageWindowState), state);
        if (state == 0)
        {
            void* removed = nullptr;
            platform::CallGuarded(g.remove, removed, w);
        }
        else
        {
            const std::uint8_t remove = 1;
            platform::WriteGuarded(At(w, facts::kMessageWindowRemoveAfterOut), &remove, sizeof(remove));
            platform::CallGuarded(g.close, w);
            g_closing.push_back(Window{.seat = seat, .object = w, .pair = weak::MakeWeak(w)});
        }
        platform::LogWarning(L"view window: {}'s window faulted while opening ({}); {}", players::SeatName(seat),
                             !filled ? L"its text or options" : !opened ? L"its open" : L"its focus pop",
                             state == 0 ? L"taken out of the overlay" : L"closed again");
        return nullptr;
    }
    std::int32_t id     = -1;
    void*        object = w;
    void*        out    = nullptr;
    void* const  list   = Ui(facts::kMessageWindowListSlot);
    const bool   listed = list != nullptr && platform::CallGuarded(g.listAdd, out, list, &id, &object);
    ++g_opened;
    const wchar_t* const unlisted = listed ? L"" : L"; not in the game's window list";
    if (before == after)
    {
        platform::Log(L"view window: opened in {}'s view (the game's message window, z {}; focus layers {} before and "
                      L"after{})",
                      players::SeatName(seat), z, before, unlisted);
    }
    else
    {
        platform::LogWarning(L"view window: opened in {}'s view, but the focus layers are {} after and were {} before{}",
                             players::SeatName(seat), after, before, unlisted);
    }
    return w;
}

void Forget(Window& w) noexcept
{
    w = Window{};
}

std::wstring Reading()
{
    std::wstring open;
    for (const Window& w : g_open)
    {
        if (w.seat >= 0)
        {
            open += std::format(L"{}{}", open.empty() ? L"" : L", ", players::SeatName(w.seat));
        }
    }
    return std::format(L"{}; open in: {}; closing {}; opened {}, text changes {}, failed {} ({} inside a player's scope), "
                       L"closed by the game {}",
                       g_installed ? L"on" : L"not installed", open.empty() ? L"none" : open, g_closing.size(), g_opened,
                       g_changes, g_failed, g_inScope, g_gameClosed);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
try
{
    namespace f = facts;
    const auto create     = platform::FindFunction(image, f::kMessageWindowCreate);
    const auto addToRoot  = platform::FindFunction(image, f::kAddToRootOverlayFn);
    const auto inRoot     = platform::FindFunction(image, f::kIsInRootOverlay);
    const auto setText    = platform::FindFunction(image, f::kMessageWindowSetText);
    const auto setOptions = platform::FindFunction(image, f::kMessageWindowSetOptions);
    const auto open       = platform::FindFunction(image, f::kMessageWindowOpen);
    const auto pop        = platform::FindFunction(image, f::kPopFromFocusStack);
    const auto close      = platform::FindFunction(image, f::kMessageWindowClose);
    const auto remove     = platform::FindFunction(image, f::kRemoveFromRootOverlay);
    const auto listAdd    = platform::FindFunction(image, f::kMessageWindowListAdd);
    const auto playSound  = platform::FindFunction(image, f::kPlayUISound);
    const auto source     = platform::FindFunction(image, f::kUISoundSource);
    const auto ui         = image.Rebase(f::kEndUISystemSlots, 8 * 64);
    const auto engine     = image.Rebase(f::kGEngine, sizeof(void*));
    const auto cursor     = image.Rebase(f::kSoundCueCursor, 8);
    const auto decide     = image.Rebase(f::kSoundCueDecide, 8);
    const auto cancel     = image.Rebase(f::kSoundCueCancel, 8);
    if (!create || !addToRoot || !inRoot || !setText || !setOptions || !open || !pop || !close || !remove || !listAdd || !ui ||
        !engine)
    {
        platform::Log(L"view window: an engine check failed; the claim's prompts are drawn in the engine's own font instead");
        return false;
    }
    g.create     = reinterpret_cast<CreateFn>(*create);
    g.addToRoot  = reinterpret_cast<AddToRootFn>(*addToRoot);
    g.inRoot     = reinterpret_cast<InRootFn>(*inRoot);
    g.setText    = reinterpret_cast<SetTextFn>(*setText);
    g.setOptions = reinterpret_cast<SetOptionsFn>(*setOptions);
    g.open       = reinterpret_cast<OpenFn>(*open);
    g.pop        = reinterpret_cast<WindowFn>(*pop);
    g.close      = reinterpret_cast<WindowFn>(*close);
    g.remove     = reinterpret_cast<RemoveFn>(*remove);
    g.listAdd    = reinterpret_cast<ListAddFn>(*listAdd);
    g.ui         = *ui;
    g.engine     = *engine;
    if (playSound && source && cursor && decide && cancel)
    {
        g.playSound = reinterpret_cast<PlaySoundFn>(*playSound);
        g.source    = reinterpret_cast<SourceFn>(*source);
        g.cues      = {*cursor, *decide, *cancel};
    }
    else
    {
        platform::Log(L"view window: the game's menu sounds were not found as expected; the claim's steps play none");
    }
    g_installed = true;
    control::Register({.name  = L"views.window",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the game's message windows the mod shows in the players' views (the pad "
                                L"claim's)",
                       .get   = [] {
                           try
                           {
                               return control::Value{Reading()};
                           }
                           catch (...)
                           {
                               return control::Value{std::wstring(L"unreadable (an exception)")};
                           }
                       }});
    platform::Log(L"view window: the game's message window can be shown in one player's view (z {})", f::kClaimWindowZOrder);
    return true;
}
catch (...)
{
    return false;
}

void Remove() noexcept
{
    const bool left = Any();
    g_open.fill(Window{});
    g_closing.clear();
    g_installed = false;
    if (left)
    {
        platform::Log(L"view window: unloaded with a window still shown; the game's next reset closes it");
    }
}

bool Show(int seat, const std::wstring& markup) noexcept
try
{
    if (!g_installed || seat < 0 || seat >= kPlayers)
    {
        return false;
    }
    Window& w = g_open[static_cast<std::size_t>(seat)];
    if (w.seat >= 0 && Alive(w))
    {
        if (w.text != markup)
        {
            if (!SetText(w.object, markup))
            {
                ++g_failed;
                return false;
            }
            w.text = markup;
            ++g_changes;
            platform::LogDebug(L"view window: {}'s text changed", players::SeatName(seat));
        }
        return true;
    }
    Forget(w);
    void* const object = OpenWindow(seat, markup);
    if (object == nullptr)
    {
        ++g_failed;
        if (!g_failLogged)
        {
            g_failLogged = true;
            platform::LogWarning(L"view window: no game window could be shown in {}'s view (no world, or the game made "
                                 L"none); the caller falls back",
                                 players::SeatName(seat));
        }
        return false;
    }
    w = Window{.seat = seat, .object = object, .pair = weak::MakeWeak(object), .text = markup};
    return true;
}
catch (...)
{
    return false;
}

void Close(int seat) noexcept
try
{
    if (seat < 0 || seat >= kPlayers)
    {
        return;
    }
    Window& w = g_open[static_cast<std::size_t>(seat)];
    if (w.seat < 0)
    {
        return;
    }
    if (Alive(w))
    {
        // The game's own byte: its tick takes the window out of the overlay when Out ends.
        const std::uint8_t remove = 1;
        if (platform::WriteGuarded(At(w.object, facts::kMessageWindowRemoveAfterOut), &remove, sizeof(remove)) &&
            platform::CallGuarded(g.close, w.object))
        {
            g_closing.push_back(w);
            platform::Log(L"view window: closed in {}'s view", players::SeatName(seat));
        }
        else
        {
            platform::LogWarning(L"view window: {}'s window could not be closed", players::SeatName(seat));
        }
    }
    Forget(w);
}
catch (...)
{
}

void CloseAll() noexcept
{
    for (int seat = 0; seat < kPlayers; ++seat)
    {
        Close(seat);
    }
}

int SeatOf(const void* widget) noexcept
{
    if (widget == nullptr)
    {
        return -1;
    }
    for (const Window& w : g_open)
    {
        if (w.seat >= 0 && w.object == widget && weak::Resolve(w.pair) == widget)
        {
            return w.seat;
        }
    }
    for (const Window& w : g_closing)
    {
        if (w.object == widget && weak::Resolve(w.pair) == widget)
        {
            return w.seat;
        }
    }
    return -1;
}

std::uint64_t ClosedByGame() noexcept
{
    return g_gameClosed;
}

bool Any() noexcept
{
    return !g_closing.empty() || std::any_of(g_open.begin(), g_open.end(), [](const Window& w) { return w.seat >= 0; });
}

void Play(Sound sound) noexcept
{
    const auto at = static_cast<std::size_t>(sound);
    if (!g_installed || g.playSound == nullptr || at >= g.cues.size())
    {
        return;
    }
    std::uint64_t cue = 0;
    void*         src = nullptr;
    if (!platform::Read(reinterpret_cast<const void*>(g.cues[at]), cue) || cue == 0 || !platform::CallGuarded(g.source, src) ||
        src == nullptr)
    {
        return;
    }
    // The 0x34-byte arguments as the message window builds them (§12.6).
    alignas(16) std::uint8_t args[0x38]{};
    const std::int32_t       index    = -1;
    const float              mix[4]   = {0.0f, 1.0f, 1.0f, 0.0f};
    const float              one      = 1.0f;
    std::memcpy(args + 0x00, &src, sizeof(src));
    std::memcpy(args + 0x08, &cue, sizeof(cue));
    std::memcpy(args + 0x10, &index, sizeof(index));
    std::memcpy(args + 0x14, mix, sizeof(mix));
    std::memcpy(args + 0x30, &one, sizeof(one));
    platform::CallGuarded(g.playSound, static_cast<const void*>(args));
}

void Tick() noexcept
try
{
    if (!g_installed)
    {
        return;
    }
    for (Window& w : g_open)
    {
        if (w.seat >= 0 && !Alive(w))
        {
            ++g_gameClosed;
            platform::Log(L"view window: the game closed {}'s window (a load, a map change or an overlay rebuild)",
                          players::SeatName(w.seat));
            Forget(w);
        }
    }
    std::erase_if(g_closing, [](const Window& w) { return !Alive(w); });
}
catch (...)
{
}

}  // namespace lss::hud::view_window
