#include "world/shop_act.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/game_globals.h"
#include "players/scopes.h"
#include "players/seats.h"
#include "world/field_targets.h"

#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace lss::world::shop_act {

namespace {

using platform::Read;
using players::SeatName;

using CtorFn     = void* (*)(void* runner, const std::uint64_t* act, std::uint32_t type, void* channel, bool flag);
using UpdateFn   = bool (*)(void* runner, float dt);
using OnFinishFn = char (*)(void* runner);
using DtorFn     = void* (*)(void* runner, bool free);
using FindRowFn  = void* (*)(const void* tables, std::uint64_t name);

constexpr std::size_t  kNotes    = 4;  // shop acts of further players alive at once (one shop UI: one at a time)
constexpr std::int32_t kShopKind = 1;
constexpr unsigned     kRowScopes    = players::scopes::Leader | players::scopes::Primary;  // the camera on the opener's manager
constexpr unsigned     kWindowScopes = players::scopes::Leader;  // the camera on the game's primary manager, player 1's

/// A further player's shop act: the runner, its player, the character that player held at the start (a later
/// call for another character, after a leave and a join on the same seat, is the game's) and the scopes it started
/// in (kept for the act's life).
struct Note
{
    void*    runner = nullptr;
    int      seat   = 0;
    void*    member = nullptr;
    unsigned scopes = 0;
};

// Set at install, then only read.
std::uintptr_t         g_ctorCall = 0, g_ctor = 0, g_actions = 0, g_vtable = 0;
FindRowFn              g_findRow  = nullptr;
platform::CallRedirect g_create;
platform::VTableEntry  g_update, g_finish, g_dtor;

// Game thread.
std::array<Note, kNotes> g_notes{};
bool                     g_oneWindow = false;  // the next shop's camera on the game's primary manager (SetOneWindow)
std::uint32_t g_created = 0, g_claimed = 0, g_notShops = 0, g_ended = 0, g_refused = 0, g_full = 0, g_stale = 0;

/// The note is still this act's: the runner still carries the act's vtable (else it was freed by a path that bypassed
/// the destructor slot, and the note is forgotten, with a warning).
bool StillAct(Note& note) noexcept
{
    std::uintptr_t vtable = 0;
    if (Read(note.runner, vtable) && vtable == g_vtable)
    {
        return true;
    }
    ++g_stale;
    platform::LogWarning(L"shop act: {}'s shop act is gone without its end (a teardown bypassed the destructor); forgotten",
                         SeatName(note.seat));
    note = {};
    return false;
}

Note* NoteOf(const void* runner) noexcept
{
    for (Note& note : g_notes)
    {
        if (runner != nullptr && note.runner == runner)
        {
            return StillAct(note) ? &note : nullptr;
        }
    }
    return nullptr;
}

/// The act row's kind (its IntegerArgumentList[0]), or -1 when the row or the list cannot be read.
std::int32_t KindOf(std::uint64_t act) noexcept
{
    void*               row   = nullptr;
    const std::uint8_t* list  = nullptr;
    std::int32_t        count = 0, kind = -1;
    if (act == 0 || !platform::CallGuarded(g_findRow, row, reinterpret_cast<const void*>(g_actions), act) || row == nullptr)
    {
        return -1;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(row);
    if (!Read(bytes + facts::kActionRowIntegers, list) || !Read(bytes + facts::kActionRowIntegers + 8, count) || count <= 0 ||
        list == nullptr || !Read(list, kind))
    {
        return -1;
    }
    return kind;
}

/// The note's scope is open: its player still holds the character it opened the shop with, and the scope opened (or
/// is already open for that player, each scope the note names).
bool Opened(const Note& note, const players::scopes::Scope& scope) noexcept
{
    using players::scopes::OpenPlayer;
    return players::scopes::Member(note.seat) == note.member &&
           (scope.Held() || (OpenPlayer(players::scopes::Leader) == note.seat &&
                             ((note.scopes & players::scopes::Primary) == 0 || OpenPlayer(players::scopes::Primary) == note.seat)));
}

/// A later call of a noted act whose scope did not open: the rest of the act is the game's (logged once).
void Refuse(Note& note, const wchar_t* call) noexcept
{
    ++g_refused;
    platform::LogWarning(L"shop act: {}'s scope could not open for the shop's {} (the player left or changed character); the "
                         L"rest of the shop runs as player 1's",
                         SeatName(note.seat), call);
    note = {};
}

void* Create(void* runner, const std::uint64_t* act, std::uint32_t type, void* channel, bool flag)
{
    const auto    original = reinterpret_cast<CtorFn>(g_ctor);
    std::uint64_t object = 0, actName = 0;
    Read(channel, object);
    Read(act, actName);
    ++g_created;
    for (Note& note : g_notes)
    {
        if (note.runner == runner)
        {
            // A new act at a noted address: the old one is gone without its end.
            ++g_stale;
            platform::LogWarning(L"shop act: {}'s shop act is gone without its end (a new act took its place); forgotten",
                                 SeatName(note.seat));
            note = {};
        }
    }
    const std::int32_t kind    = KindOf(actName);
    const int          talked  = field_targets::PeekTalkPress(object);
    // A shop no NPC press names: a vending machine's, whose act runs on the level script's channel (matched by time).
    std::uint64_t      oneName = 0, oneAt = 0;
    const bool         onesAct = field_targets::PlayerOnePress(oneName, oneAt) && oneName == object;  // player 1 pressed its channel
    const int          machine = talked == 0 && kind == kShopKind && !onesAct ? field_targets::PeekVendingPress() : 0;
    const bool         vending = machine != 0;
    const int          pressed = vending ? machine : talked;
    if (platform::Enabled(platform::Level::Debug))
    {
        try
        {
            platform::LogDebug(L"shop act: a show-menu act {} of kind {} starts on {}'s channel; {}{}",
                               field_targets::NameOf(actName), kind, field_targets::NameOf(object),
                               pressed == 0 ? L"no further player's press of it" : SeatName(pressed),
                               pressed == 0        ? L""
                               : vending           ? L"'s vending machine press: a shop"
                               : kind != kShopKind ? L"'s press of it, but not a shop: the game's"
                                                   : L"'s press of it: a shop");
        }
        catch (...)
        {
        }
    }
    if (pressed != 0 && kind != kShopKind)
    {
        ++g_notShops;
    }
    // The seat Peek named; 0 with no press, for an act that is no shop, or a press that lapsed between the peek and here.
    const int seat = pressed == 0 || kind != kShopKind ? 0
                     : vending                         ? field_targets::ClaimVendingPress()
                                                       : field_targets::ClaimTalkPress(object);
    if (seat == 0)
    {
        return original(runner, act, type, channel, flag);
    }
    Note*     free = nullptr;
    for (Note& note : g_notes)
    {
        if (note.runner == nullptr)
        {
            free = &note;
            break;
        }
    }
    if (free == nullptr)
    {
        ++g_full;
        platform::LogWarning(L"shop act: {}'s shop cannot be noted (more than {} at once); it runs as player 1's", SeatName(seat),
                             kNotes);
        return original(runner, act, type, channel, flag);
    }
    *free = {runner, seat, players::scopes::Member(seat), g_oneWindow ? kWindowScopes : kRowScopes};
    {
        const players::scopes::Scope scope(seat, free->scopes);
        if (free->member != nullptr && Opened(*free, scope))
        {
            void* const result = original(runner, act, type, channel, flag);
            ++g_claimed;
            const bool window = (free->scopes & players::scopes::Primary) == 0;
            platform::Log(L"shop act: {} opened a shop: its camera on {}{}, its character facing the shopkeeper", SeatName(seat),
                          window ? L"the screen's one view (player 1's manager, one window)" : SeatName(seat), window ? L"" : L"'s view");
            return result;
        }
    }
    // The scope did not open (whole): the start runs as player 1's, outside any of the seat's contexts, and so does the
    // rest of the act.
    *free = {};
    ++g_refused;
    platform::LogWarning(L"shop act: {}'s scope could not open for the shop's start; the shop runs as player 1's", SeatName(seat));
    return original(runner, act, type, channel, flag);
}

bool Update(void* runner, float dt)
{
    const auto original = reinterpret_cast<UpdateFn>(g_update.Original());
    Note*      note     = NoteOf(runner);
    if (note == nullptr)
    {
        return original(runner, dt);
    }
    {
        const players::scopes::Scope scope(note->seat, note->scopes);
        if (Opened(*note, scope))
        {
            return original(runner, dt);
        }
    }
    Refuse(*note, L"update");
    return original(runner, dt);  // outside any of the seat's contexts
}

char OnFinish(void* runner)
{
    const auto original = reinterpret_cast<OnFinishFn>(g_finish.Original());
    Note*      note     = NoteOf(runner);
    if (note == nullptr)
    {
        return original(runner);
    }
    {
        const players::scopes::Scope scope(note->seat, note->scopes);
        if (Opened(*note, scope))
        {
            return original(runner);
        }
    }
    Refuse(*note, L"finish");
    return original(runner);
}

void* Destroy(void* runner, bool free)
{
    const auto original = reinterpret_cast<DtorFn>(g_dtor.Original());
    Note*      note     = NoteOf(runner);
    if (note == nullptr)
    {
        return original(runner, free);
    }
    const int seat = note->seat;
    {
        const players::scopes::Scope scope(seat, note->scopes);
        if (Opened(*note, scope))
        {
            void* const result = original(runner, free);  // the end blend back to the field camera, on the shop camera's manager
            *note              = {};                      // after the original: the runner is freed by then, never read again
            ++g_ended;
            platform::Log(L"shop act: {}'s shop ended", SeatName(seat));
            return result;
        }
    }
    Refuse(*note, L"end");
    return original(runner, free);
}

std::wstring Report()
{
    std::wstring out = g_create.IsInstalled() ? L"on" : L"off";
    out += std::format(L"; show-menu acts {}, shops of further players {}, ended {}, other show-menu acts after a press {}, "
                       L"scopes refused {}, not noted {}, gone without an end {}",
                       g_created, g_claimed, g_ended, g_notShops, g_refused, g_full, g_stale);
    for (const Note& note : g_notes)
    {
        if (note.runner != nullptr)
        {
            out += std::format(L"; {}'s shop is open", SeatName(note.seat));
        }
    }
    return out;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto ctor    = platform::FindFunction(image, facts::kShowMenuActCtor);
    const auto call    = image.Rebase(facts::kShowMenuActCtorCall, 5);
    const auto actions = players::game_globals::Address(players::game_globals::Global::TableAction);
    const auto vtable  = image.Rebase(facts::kShowMenuActVtable, 0x28);
    const auto findRow = platform::FindFunction(image, facts::kFindRowInTables);
    if (!ctor || !call || !actions || !vtable || !findRow)
    {
        platform::Log(L"shop act: an engine check failed; a further player's shop is player 1's (camera, facing)");
        return false;
    }
    g_ctor     = *ctor;
    g_ctorCall = *call;
    g_actions  = actions;
    g_vtable   = *vtable;
    g_findRow  = reinterpret_cast<FindRowFn>(*findRow);
    if (!platform::ReplaceSlot(image, facts::kShowMenuActUpdate, reinterpret_cast<std::uintptr_t>(&Update), g_update) ||
        !platform::ReplaceSlot(image, facts::kShowMenuActFinish, reinterpret_cast<std::uintptr_t>(&OnFinish), g_finish) ||
        !platform::ReplaceSlot(image, facts::kShowMenuActDtor, reinterpret_cast<std::uintptr_t>(&Destroy), g_dtor) ||
        !g_create.Install(g_ctorCall, g_ctor, reinterpret_cast<std::uintptr_t>(&Create)))
    {
        g_create.Restore();
        g_dtor.Restore();
        g_finish.Restore();
        g_update.Restore();
        platform::Log(L"shop act: a slot or the creation call could not be redirected; a further player's shop is player 1's "
                      L"(camera, facing)");
        return false;
    }
    control::Register({.name  = L"shop-act",
                       .shape = control::Shape::Text,
                       .help  = L"further players' shops: the show-menu acts seen, the shops claimed and ended, the scopes refused",
                       .get   = [] { return control::Value{Report()}; }});
    platform::Log(L"shop act: installed; a shop a further player opens faces that player's character and zooms on that "
                  L"player's view on a monitor row, on the screen's one view in one window");
    return true;
}

void Remove() noexcept
{
    // Called with no seat in (mod.cpp): a noted act still alive then runs its remaining calls as the game's.
    g_create.Restore();
    g_dtor.Restore();
    g_finish.Restore();
    g_update.Restore();
    g_notes.fill({});
}

void SetOneWindow(bool oneWindow) noexcept
{
    g_oneWindow = oneWindow;
}

bool OwnsScene(int seat) noexcept
{
    for (Note& note : g_notes)
    {
        if (note.runner != nullptr && note.seat == seat && (note.scopes & players::scopes::Primary) != 0 && StillAct(note))
        {
            return true;
        }
    }
    return false;
}

}  // namespace lss::world::shop_act
