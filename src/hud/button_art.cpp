#include "hud/button_art.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "hud/button_art_rules.h"
#include "hud/label_states.h"
#include "hud/text_blocks.h"
#include "platform/call_redirect.h"
#include "platform/game_thread.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/steady_ms.h"
#include "platform/verified_code.h"
#include "players/game_globals.h"
#include "players/pad_rules.h"
#include "players/pad_table.h"
#include "players/seats.h"
#include "players/weak_object.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <string>

namespace lss::hud::button_art {

namespace {

namespace rules = button_art_rules;
using label_states::LabelAccess;
using label_states::LabelBook;
using rules::DeviceArt;

using RefreshFn     = void (*)(void* uiSystem);
using RelayoutAllFn = void (*)();

constexpr std::size_t  kHashedChars     = 256;  // a text's first characters, hashed for its state's stamp
constexpr std::size_t  kMaxButtonBlocks = LabelBook::kMaxLabels;  // button blocks followed through one pass
constexpr std::size_t  kMaxRelaid       = 64;   // player 1's states laid out again after one pass
constexpr std::int64_t kCensusEveryMs   = 10'000;
constexpr int          kPlayers         = players::kSeatsBuilt + 1;
constexpr std::size_t  kMaps            = facts::kFontKeyMaps.size();

// Set once at install.
bool                   g_installed = false;
RefreshFn              g_refresh   = nullptr;
platform::CallRedirect g_relayoutAllTail;  // the listener's closing relayout of every button text
std::uintptr_t         g_fontIds   = 0;    // the listener's three font ids

// Game thread.
std::atomic<bool> g_on{true};        // the console's switch (any thread)
bool g_fontsOnly = false;  // the listener runs for a seat's art: its closing relayout is skipped
std::atomic<bool> g_refused{false};  // a swap or the listener faulted: the game's art for everyone

/// One button font's bytes as the game left them: its pad family and keyboard flag, and its five key maps (each map's
/// data, written in place by the listener).
struct FontBytes
{
    std::byte*                                                           font = nullptr;
    std::uint8_t                                                         family = 0, keyboard = 0;
    std::array<std::byte*, kMaps>                                        maps{};
    std::array<std::array<std::uint8_t, facts::kFontKeyMapBytes>, kMaps> bytes{};
};

/// The open pass: its seat (0 for none), the seat's art and the fonts it was written into, with the game's bytes.
struct Pass
{
    int                                          seat = 0;
    DeviceArt                                    art{};
    std::array<FontBytes, facts::kButtonFonts>   fonts{};
    std::size_t                                  fontCount = 0;
};
Pass g_pass;

/// A button block at the pass's start, by address: what its layout depends on outside the swapped ranges, player 1's
/// sizes (a fresh seat state is sized from them), and whether the seat's state went in. At the pass's end it tells
/// which of player 1's states the pass made stale.
struct Seen
{
    void*         block    = nullptr;
    const void*   text     = nullptr;
    std::int32_t  num      = 0;
    std::uint64_t textHash = 0;  // the text's characters (a buffer reused for a text of the same length still differs)
    std::uint64_t allotted = 0;  // player 1's allotted size (two floats)
    float         wrap     = 0.0f;
    std::uint8_t  pending  = 0;
    bool          swapped  = false;
};
std::array<Seen, kMaxButtonBlocks> g_seen{};
std::size_t                        g_seenCount = 0;

/// Each player's art as last logged (index 0: the game's own, player 1's).
std::array<DeviceArt, kPlayers> g_loggedArt{};
std::array<bool, kPlayers>      g_loggedKnown{};
std::array<bool, kPlayers>      g_loggedOwn{};  // the seat's art differed from the game's

struct Counts
{
    std::uint64_t passes = 0, ownArt = 0, buttonBlocks = 0, unswapped = 0, notFollowed = 0, laidOut = 0, relaidForOne = 0,
                  staleForOne = 0;
};
std::array<Counts, kPlayers> g_passCounts{};  // each seat's passes so far ([0] unused)
std::array<Counts, kPlayers> g_logged{};
std::int64_t                 g_censusAt      = 0;
bool                         g_loggedNoFonts = false;

template <class T>
bool ReadAt(const void* base, std::uint32_t offset, T& out) noexcept
{
    return platform::Read(static_cast<const std::byte*>(base) + offset, out);
}

template <class T>
bool WriteAt(void* base, std::uint32_t offset, const T& value) noexcept
{
    return platform::WriteGuarded(static_cast<std::byte*>(base) + offset, &value, sizeof(value));
}

// --- The book's engine side ---

bool Usable(void* block) noexcept
{
    return text_blocks::Unswappable(block) == nullptr;
}

/// The text's first characters and its length, hashed (FNV-1a): a seat state laid out for another text is stale.
std::uint64_t TextHash(void* block) noexcept
{
    const wchar_t* data = nullptr;
    std::int32_t   num  = 0;
    std::uint64_t  hash = 1469598103934665603ull;
    if (!ReadAt(block, facts::kSlateTextText, data) || !ReadAt(block, facts::kSlateTextTextNum, num) ||
        data == nullptr || num <= 1)
    {
        return hash;
    }
    std::array<wchar_t, kHashedChars> chars{};
    const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(num - 1), kHashedChars);
    if (!platform::CopyGuarded(chars.data(), data, count * sizeof(wchar_t)))
    {
        return hash;
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        hash = (hash ^ static_cast<std::uint64_t>(chars[i])) * 1099511628211ull;
    }
    return hash ^ static_cast<std::uint64_t>(num);
}

/// What a seat's state depends on outside its ranges: the block's fonts, its text, and the seat's art.
std::uint64_t Stamp(void* block) noexcept
{
    const std::uint64_t art = (static_cast<std::uint64_t>(g_pass.art.deviceClass) << 16) |
                              (static_cast<std::uint64_t>(g_pass.art.subClass) << 8) | g_pass.art.dualShock4;
    return text_blocks::FontStamp(block) ^ std::rotl(TextHash(block), 7) ^ std::rotl(art, 48);
}

constexpr LabelAccess kAccess{&Usable,
                              &text_blocks::Alive,
                              &text_blocks::SwapLayout,
                              &Stamp,
                              &text_blocks::HoldWeak,
                              &text_blocks::ReleaseWeak,
                              &text_blocks::FreeParked};
static_assert(kPlayers == 3, "one book per player, listed below");
std::array<LabelBook, kPlayers> g_books{LabelBook(kAccess), LabelBook(kAccess), LabelBook(kAccess)};

LabelBook& BookOf(int seat) noexcept
{
    return g_books[static_cast<std::size_t>(seat)];
}

Counts& CountsOf(int seat) noexcept
{
    return g_passCounts[static_cast<std::size_t>(seat)];
}

// --- Each player's art ---

/// The listener's closing relayout of every button text: skipped while it runs for a seat's art (the seat's states are
/// laid out by the pass, player 1's need none).
void RelayoutAll()
{
    if (g_fontsOnly)
    {
        return;
    }
    reinterpret_cast<RelayoutAllFn>(g_relayoutAllTail.Original())();
}

/// The device bytes the listener reads, from the input object (false while it is unread).
bool ReadDeviceArt(DeviceArt& art) noexcept
{
    const void* owner = players::game_globals::Object(players::game_globals::Global::SquareInputOwner);
    return owner != nullptr && ReadAt(owner, facts::kInputDeviceClass, art.deviceClass) &&
           ReadAt(owner, facts::kInputDeviceSubClass, art.subClass) && ReadAt(owner, facts::kInputDualShock4, art.dualShock4);
}

void WriteDeviceArt(const DeviceArt& art) noexcept
{
    void* owner = players::game_globals::Object(players::game_globals::Global::SquareInputOwner);
    if (owner != nullptr)
    {
        WriteAt(owner, facts::kInputDeviceClass, art.deviceClass);
        WriteAt(owner, facts::kInputDeviceSubClass, art.subClass);
        WriteAt(owner, facts::kInputDualShock4, art.dualShock4);
    }
}

/// The three button fonts the listener writes, their bytes read into `pass` (a font not loaded is skipped); false
/// before the fonts load.
bool SaveFonts(Pass& pass) noexcept
{
    pass.fontCount = 0;
    const void*  holder = players::game_globals::UiSlot(facts::kUiSlotFonts);
    const void*  pairs  = nullptr;
    std::int32_t num    = 0;
    if (holder == nullptr || !ReadAt(holder, facts::kFontHolderData, pairs) || !ReadAt(holder, facts::kFontHolderNum, num) || pairs == nullptr ||
        num <= 0)
    {
        return false;
    }
    for (std::size_t i = 0; i < facts::kButtonFonts; ++i)
    {
        std::uint8_t id = 0;
        std::int32_t pair[2]{};
        if (!platform::Read(reinterpret_cast<const std::byte*>(g_fontIds) + i, id) || id >= num ||
            !platform::CopyGuarded(pair, static_cast<const std::byte*>(pairs) + sizeof(pair) * id, sizeof(pair)))
        {
            continue;
        }
        FontBytes saved;
        saved.font   = static_cast<std::byte*>(players::weak_object::ObjectOf(pair));
        bool readAll = saved.font != nullptr && ReadAt(saved.font, facts::kFontPadFamily, saved.family) &&
                       ReadAt(saved.font, facts::kFontKeyboard, saved.keyboard);
        for (std::size_t m = 0; readAll && m < kMaps; ++m)
        {
            readAll = ReadAt(saved.font, facts::kFontKeyMaps[m], saved.maps[m]) && saved.maps[m] != nullptr &&
                      platform::CopyGuarded(saved.bytes[m].data(), saved.maps[m], facts::kFontKeyMapBytes);
        }
        if (readAll)
        {
            pass.fonts[pass.fontCount++] = saved;
        }
    }
    return pass.fontCount != 0;
}

/// The game's bytes back in the fonts the pass wrote.
void RestoreFonts() noexcept
{
    for (std::size_t i = 0; i < g_pass.fontCount; ++i)
    {
        const FontBytes& saved = g_pass.fonts[i];
        WriteAt(saved.font, facts::kFontPadFamily, saved.family);
        WriteAt(saved.font, facts::kFontKeyboard, saved.keyboard);
        for (std::size_t m = 0; m < kMaps; ++m)
        {
            // Into the map the listener wrote, read again: a map the font no longer holds is not written.
            std::byte* map = nullptr;
            if (ReadAt(saved.font, facts::kFontKeyMaps[m], map) && map == saved.maps[m])
            {
                platform::WriteGuarded(map, saved.bytes[m].data(), facts::kFontKeyMapBytes);
            }
        }
    }
    g_pass.fontCount = 0;
}

/// The device that plays `seat`: the slot's playing pad by its reader, the keyboard when no pad plays it.
rules::Device DeviceOf(int seat) noexcept
{
    const int pad = players::pad_rules::PlayingPad(players::pad_table::Table(), seat, players::pad_table::PresentPads());
    return pad < 0                                                          ? rules::Device::Keyboard
           : players::pad_rules::ReaderOf(pad) == players::pad_rules::kXInput ? rules::Device::XInputPad
                                                                              : rules::Device::PlayStationPad;
}

const wchar_t* ArtName(const DeviceArt& art) noexcept
{
    switch (art.deviceClass)
    {
    case rules::kClassKeyboard: return L"keycaps";
    case rules::kClassXInput: return L"Xbox art";
    case rules::kClassPlayStation:
        return art.dualShock4 != 0 ? L"PlayStation art (DualShock 4)" : L"PlayStation art (DualSense)";
    default: return L"DirectInput art";
    }
}

/// An Info line when a player's art changes (player 1's: the game's own).
void NoteArt(int seat, const DeviceArt& art, bool own) noexcept
{
    const auto s = static_cast<std::size_t>(seat);
    if (g_loggedKnown[s] && rules::SameArt(g_loggedArt[s], art) && g_loggedOwn[s] == own)
    {
        return;
    }
    g_loggedKnown[s] = true;
    g_loggedArt[s]   = art;
    g_loggedOwn[s]   = own;
    if (seat == 0)
    {
        platform::LogDebug(L"button art: the game's own (player 1's view, and every view while one shows): {}", ArtName(art));
        return;
    }
    platform::Log(L"button art: {}'s view shows {}{}", players::SeatName(seat), ArtName(art),
                  own ? L" (its own state of each button text)" : L", as the game's own");
}

// --- The passes ---

bool IsButtonBlock(void* block) noexcept
{
    std::uint8_t button = 0;
    return ReadAt(block, facts::kSlateTextButtonTag, button) && button != 0;
}

Seen SeenOf(void* block) noexcept
{
    Seen s;
    s.block = block;
    ReadAt(block, facts::kSlateTextText, s.text);
    ReadAt(block, facts::kSlateTextTextNum, s.num);
    ReadAt(block, facts::kSlateTextSize, s.allotted);
    ReadAt(block, facts::kSlateTextWrapWidth, s.wrap);
    ReadAt(block, facts::kSlateTextFlags, s.pending);
    s.textHash = TextHash(block);
    return s;
}

bool SameText(const Seen& a, const Seen& b) noexcept
{
    return a.text == b.text && a.num == b.num && a.textHash == b.textHash;
}

/// Nothing the layout reads changed and no relayout was pending: the pass did not lay the block out.
bool SameLayoutInputs(const Seen& a, const Seen& b) noexcept
{
    return SameText(a, b) && std::bit_cast<std::uint32_t>(a.wrap) == std::bit_cast<std::uint32_t>(b.wrap) &&
           a.pending == 0 && b.pending == 0;
}

const Seen* FindSeen(const void* block) noexcept
{
    const Seen* const begin = g_seen.data();
    const Seen* const end   = begin + g_seenCount;
    const Seen* const at    = std::lower_bound(begin, end, block,
                                               [](const Seen& s, const void* b) { return std::less<>{}(s.block, b); });
    return at != end && at->block == block ? at : nullptr;
}

/// A fresh seat state, swapped in: sized as player 1's state was (so the block's Tick finds no width change) and laid
/// out now, with the seat's fonts, by the game's relayout.
void LayOutFresh(const Seen& seen) noexcept
{
    WriteAt(seen.block, facts::kSlateTextSize, seen.allotted);
    WriteAt(seen.block, facts::kSlateTextWrapWidth, seen.wrap);
    text_blocks::Relayout(seen.block);
}

bool CollectButtonBlock(void* block, void* context) noexcept
{
    Counts& counts = *static_cast<Counts*>(context);
    if (IsButtonBlock(block))
    {
        ++counts.buttonBlocks;
        if (g_seenCount < g_seen.size())
        {
            g_seen[g_seenCount++] = SeenOf(block);
        }
        else
        {
            ++counts.notFollowed;
        }
    }
    return true;
}

/// The pass's leftovers: player 1's states whose text the pass changed, and button blocks laid out in the pass without
/// a seat state (with the seat's fonts).
struct Leftovers
{
    LabelBook*                    book = nullptr;
    std::array<void*, kMaxRelaid> relaid{};
    std::size_t                   relaidCount = 0;
};

bool CollectRelaid(void* block, void* context) noexcept
{
    Leftovers& left = *static_cast<Leftovers*>(context);
    if (!IsButtonBlock(block) || left.book->Holds(text_blocks::ControllerOf(block)))
    {
        return true;
    }
    const Seen* const seen = FindSeen(block);
    if (seen == nullptr || !SameLayoutInputs(*seen, SeenOf(block)))
    {
        left.relaid[left.relaidCount++] = block;
    }
    return left.relaidCount < left.relaid.size();
}

void Census() noexcept
{
    const std::int64_t now = platform::NowMs();
    if (now < g_censusAt)
    {
        return;
    }
    g_censusAt = now + kCensusEveryMs;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto    s = static_cast<std::size_t>(seat);
        const Counts& c = g_passCounts[s];
        Counts&       l = g_logged[s];
        if (c.passes == l.passes)
        {
            continue;
        }
        const label_states::LabelCounts& b = g_books[s].Counts();
        platform::LogDebug(L"button art (10 s), {}: passes {} (with its own art {}), button blocks per pass {:.1f}; states "
                           L"{} (so far: new {}, laid out again {}, refused {}, book full {}, faults {}); laid out in the "
                           L"pass {}; kept player 1's state {} (not followed {}); player 1's laid out again after a pass "
                           L"{}, marked stale {}",
                           players::SeatName(seat), c.passes - l.passes, c.ownArt - l.ownArt,
                           c.ownArt != l.ownArt
                               ? static_cast<double>(c.buttonBlocks - l.buttonBlocks) / static_cast<double>(c.ownArt - l.ownArt)
                               : 0.0,
                           g_books[s].Count(), b.created, b.resets, b.refused, b.full, b.faults, c.laidOut - l.laidOut,
                           c.unswapped - l.unswapped, c.notFollowed - l.notFollowed, c.relaidForOne - l.relaidForOne,
                           c.staleForOne - l.staleForOne);
        l = c;
    }
}

std::wstring Reading()
{
    std::wstring text = g_installed ? (g_on ? (g_refused ? L"refused (a fault)" : L"on") : L"off") : L"not installed";
    for (int seat = 0; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto s = static_cast<std::size_t>(seat);
        text += std::format(L"; {}: {}{}", players::SeatName(seat), g_loggedKnown[s] ? ArtName(g_loggedArt[s]) : L"not seen",
                            seat != 0 ? std::format(L" ({} states)", g_books[s].Count()) : std::wstring());
    }
    return text;
}

void RegisterCommands()
{
    control::Register({.name  = L"button-art",
                       .shape = control::Shape::OnOff,
                       .help  = L"players 2 and 3's views show their own device's button art (off: the game's own in every "
                                L"view)",
                       .set   = [](const control::Value& v) {
                           const bool on = std::get<bool>(v);
                           g_on          = on;
                           if (on)
                           {
                               g_refused = false;  // on again: a past fault is tried afresh
                           }
                           platform::Log(L"button art {}", on ? L"on" : L"off");
                           return control::Outcome{true, on ? L"each view's own button art" : L"the game's button art everywhere"};
                       },
                       .get   = [] { return control::Value{g_on.load()}; }});
    control::Register({.name  = L"players.button-art",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) each player's button art as last logged, and the states each player keeps",
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
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto refresh = platform::FindFunction(image, facts::kRefreshButtonIcons);
    const auto tail    = image.Rebase(facts::kRefreshRelayoutTail, 5);
    const auto target  = image.Rebase(facts::kRebuildAllWithButtons);
    const auto fontIds = image.Rebase(facts::kButtonFontIds, facts::kButtonFonts);
    g_installed = refresh && tail && target && fontIds && text_blocks::Install(image) && players::weak_object::Installed() &&
                  players::game_globals::UiSlotAddress(0) != 0 &&
                  players::game_globals::Address(players::game_globals::Global::SquareInputOwner) != 0 &&
                  g_relayoutAllTail.InstallTailJump(*tail, *target, reinterpret_cast<std::uintptr_t>(&RelayoutAll));
    if (!g_installed)
    {
        platform::LogWarning(L"button art: the text blocks, the fonts or the game's listener are not what the research read: "
                             L"every view shows the game's own button art");
        return false;
    }
    g_refresh = reinterpret_cast<RefreshFn>(*refresh);
    g_fontIds = *fontIds;
    RegisterCommands();
    platform::Log(L"button art: each player's view shows its own device's button art (keycaps, Xbox, or PlayStation of the "
                  L"pad's own kind); while one view shows, the game's own");
    return true;
}

void Remove() noexcept
{
    if (g_pass.seat != 0)
    {
        LeavePass();
    }
    if (g_pass.seat != 0)
    {
        RestoreFonts();  // off the game thread the states stay (the widgets are the game thread's), the fonts not
        g_pass.seat = 0;
    }
    if (platform::OnGameThread())
    {
        for (LabelBook& book : g_books)
        {
            book.ReleaseAll();
        }
    }
    g_relayoutAllTail.Restore();
    g_installed = false;
}

void EnterPass(int seat) noexcept
{
    if (!g_installed || !g_on || g_refused || g_pass.seat != 0 || seat <= 0 || seat > players::kSeatsBuilt ||
        !platform::OnGameThread())
    {
        return;
    }
    Counts& counts = CountsOf(seat);
    ++counts.passes;
    DeviceArt game{};
    if (!ReadDeviceArt(game))
    {
        return;
    }
    const DeviceArt art = rules::ArtOf(DeviceOf(seat), players::pad_table::DualShock4(seat), game);
    const bool      own = !rules::SameArt(art, game);
    NoteArt(0, game, false);
    NoteArt(seat, art, own);
    if (!own)
    {
        return;
    }
    if (!SaveFonts(g_pass))
    {
        if (!g_loggedNoFonts)
        {
            g_loggedNoFonts = true;
            platform::Log(L"button art: the game's fonts are not loaded yet; the views show the game's art meanwhile");
        }
        return;
    }
    // The game's own listener writes the seat's art into the fonts, as on a device change to the seat's device; its
    // device bytes are the game's again at once, and its relayout of every text is skipped.
    WriteDeviceArt(art);
    g_fontsOnly       = true;
    const bool listed = platform::CallGuarded(g_refresh, reinterpret_cast<void*>(players::game_globals::UiSlotAddress(0)));
    g_fontsOnly       = false;
    WriteDeviceArt(game);
    if (!listed)
    {
        RestoreFonts();
        g_refused = true;
        platform::Log(L"button art: the game's button-art listener raised an exception; every view shows the game's art from now");
        return;
    }
    ++counts.ownArt;
    g_pass.seat = seat;
    g_pass.art  = art;
    // Collected first, then swapped: never a swap or a relayout inside the walk of the game's set.
    g_seenCount = 0;
    text_blocks::ForEachLive(&CollectButtonBlock, &counts);
    std::sort(g_seen.begin(), g_seen.begin() + static_cast<std::ptrdiff_t>(g_seenCount),
              [](const Seen& a, const Seen& b) { return std::less<>{}(a.block, b.block); });
    LabelBook& book = BookOf(seat);
    for (std::size_t i = 0; i < g_seenCount; ++i)
    {
        Seen&               seen       = g_seen[i];
        void* const         controller = text_blocks::ControllerOf(seen.block);
        const std::uint64_t fresh      = book.Counts().created + book.Counts().resets;
        if (!book.SwapIn(seen.block, controller))
        {
            g_refused = true;
            break;
        }
        seen.swapped = book.Holds(controller);
        if (!seen.swapped)
        {
            ++counts.unswapped;
        }
        else if (book.Counts().created + book.Counts().resets != fresh)
        {
            LayOutFresh(seen);
            ++counts.laidOut;
        }
    }
    if (g_refused)
    {
        platform::Log(L"button art: a text state swap raised an exception; every view shows the game's art from now");
        LeavePass();
    }
}

void LeavePass() noexcept
{
    const int seat = g_pass.seat;
    if (seat == 0 || !platform::OnGameThread())
    {
        return;
    }
    RestoreFonts();
    LabelBook& book = BookOf(seat);
    // Found while the seat's states are still in: player 1's states whose text the pass changed (marked pending, so
    // player 1's pass lays them out again), and the button blocks the pass laid out with the seat's fonts but no state of
    // the seat's (laid out again now).
    std::array<void*, kMaxRelaid> stale{};
    std::size_t                   staleCount = 0;
    for (std::size_t i = 0; i < g_seenCount && staleCount < stale.size(); ++i)
    {
        if (g_seen[i].swapped && !SameText(g_seen[i], SeenOf(g_seen[i].block)))
        {
            stale[staleCount++] = g_seen[i].block;  // still readable: the book holds a weak reference
        }
    }
    Leftovers left;
    left.book = &book;
    text_blocks::ForEachLive(&CollectRelaid, &left);
    const std::uint64_t faults = book.Counts().faults;
    book.SwapBack();  // each record stamped first, while the seat's states and the pass's art are in
    g_pass.seat = 0;
    if (book.Counts().faults != faults)
    {
        g_refused = true;  // a block kept the seat's state: no more swaps
        platform::Log(L"button art: putting player 1's text state back raised an exception; every view shows the game's art "
                      L"from now");
    }
    for (std::size_t i = 0; i < left.relaidCount; ++i)
    {
        text_blocks::Relayout(left.relaid[i]);
    }
    const std::uint8_t pending = 1;
    for (std::size_t i = 0; i < staleCount; ++i)
    {
        if (text_blocks::Alive(text_blocks::ControllerOf(stale[i])))
        {
            WriteAt(stale[i], facts::kSlateTextFlags, pending);
        }
    }
    Counts& counts = CountsOf(seat);
    counts.relaidForOne += left.relaidCount;
    counts.staleForOne += staleCount;
    g_seenCount = 0;
}

void Tick() noexcept
{
    if (!g_installed || !platform::OnGameThread())
    {
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        LabelBook& book = BookOf(seat);
        if (g_on && !g_refused)
        {
            book.Sweep();
        }
        else if (book.Count() != 0)
        {
            book.ReleaseAll();
            g_loggedKnown[static_cast<std::size_t>(seat)] = false;
        }
    }
    Census();
}

}  // namespace lss::hud::button_art
