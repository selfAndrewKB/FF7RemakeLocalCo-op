#include "platform/verified_code.h"

#include "platform/guarded.h"
#include "platform/log.h"

#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <thread>

namespace lss::platform {

namespace {

/// Whether `fact`'s function may be used behind another hook's jump at its entry: only when the mod calls it or
/// replaces a vtable slot that names it, never when it writes the entry itself.
enum class HookJump
{
    Refuse,
    Accept,
};

/// The live entry of `fact`'s function when another hook (UE4SS, a Lua mod) has put its jump there: `jmp rel32` (E9,
/// 5 bytes) or `jmp [rip+disp32]` (FF 25, 6 bytes) to a place outside the game's image (a hook's code lives in its
/// loader's module or memory; a jump into the image is the game's own thunk). A hook overwrites whole instructions,
/// padding past its jump, so the entry is taken as the game's when its expected bytes resume somewhere after the jump
/// and run to the end of the entry, at least kMinTail of them. Empty otherwise.
std::optional<std::uintptr_t> EntryBehindHookJump(const GameImage& image, const FunctionFact& fact) noexcept
{
    constexpr std::size_t kMinTail = 6;
    const auto            live     = image.Rebase(fact.address);
    std::uint8_t          head[6]{};
    if (!live || !CopyGuarded(head, reinterpret_cast<const void*>(*live), sizeof(head)))
    {
        return std::nullopt;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, head + (head[0] == 0xE9 ? 1 : 2), sizeof(displacement));
    std::size_t    jump        = 0;
    std::uintptr_t destination = 0;
    if (head[0] == 0xE9)
    {
        jump        = 5;
        destination = *live + jump + displacement;
    }
    else if (head[0] == 0xFF && head[1] == 0x25)
    {
        jump = 6;
        if (!CopyGuarded(&destination, reinterpret_cast<const void*>(*live + jump + displacement), sizeof(destination)))
        {
            return std::nullopt;
        }
    }
    if (jump == 0 || destination == 0 || image.Contains(destination))  // 0: a hook's slot not written yet
    {
        return std::nullopt;
    }
    for (std::size_t from = jump; from + kMinTail <= fact.entryBytes.size(); ++from)
    {
        if (image.BytesMatch(fact.address + from, fact.entryBytes.subspan(from)))
        {
            return live;
        }
    }
    return std::nullopt;
}

/// The live address of `fact`'s function, its entry checked up to three times a millisecond apart: a loader may hook
/// a function for a moment while it starts (UE4SS hooks FName's constructor) and take the hook off on its own thread,
/// so a first read can catch the entry hooked or half-written. With `jump` Accept, another hook's jump at the entry
/// passes too, the function then used behind it (`how` says how, in the log). Every outcome but a first exact match is
/// logged; a failure with the entry's live bytes.
std::optional<std::uintptr_t> Locate(const GameImage& image, const FunctionFact& fact, HookJump jump,
                                     const wchar_t* how) noexcept
{
    constexpr int kReads = 3;
    const auto    live   = image.Rebase(fact.address);
    for (int read = 0; live && read < kReads; ++read)
    {
        if (image.BytesMatch(fact.address, fact.entryBytes))
        {
            if (read > 0)
            {
                Log(L"{}: its entry did not match at the first read and matches at read {}", fact.what, read + 1);
            }
            return live;
        }
        if (jump == HookJump::Accept && EntryBehindHookJump(image, fact))
        {
            Log(L"{}: another hook's jump sits at its entry; {}", fact.what, how);
            return live;
        }
        if (read + 1 < kReads)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    // The live bytes go in the line: a jump there means another hook took the function. Read under a guard: the check
    // also fails for a page that cannot be read.
    std::uint8_t entry[16]{};
    const bool   readable = live && CopyGuarded(entry, reinterpret_cast<const void*>(*live), sizeof(entry));
    std::wstring bytes;
    for (std::size_t i = 0; readable && i < sizeof(entry); ++i)
    {
        bytes += std::format(L"{:02X} ", entry[i]);
    }
    Log(L"check failed: {} (IDA 0x{:X}) does not start with the expected bytes{} (live: {})", fact.what, fact.address,
        jump == HookJump::Accept ? L" or another hook's jump before them" : L"",
        !live ? L"outside the image" : readable ? bytes : L"not readable");
    return std::nullopt;
}

}  // namespace

std::optional<std::uintptr_t> FindFunction(const GameImage& image, const FunctionFact& fact) noexcept
{
    return Locate(image, fact, HookJump::Refuse, L"");
}

std::optional<std::uintptr_t> FindCallable(const GameImage& image, const FunctionFact& fact) noexcept
{
    return Locate(image, fact, HookJump::Accept, L"the function is called through it");
}

bool ReplaceSlot(const GameImage& image, const SlotFact& fact, std::uintptr_t replacement, VTableEntry& entry) noexcept
{
    // The slot names the function, and a slot replacement never touches its entry bytes, so another hook's jump there
    // is no bar.
    const auto target = Locate(image, fact.target, HookJump::Accept, L"the slot's function is checked behind it");
    const auto slot   = image.Rebase(fact.slot, sizeof(void*));
    const auto held   = image.ReadPointer(fact.slot);
    if (!target || !slot || !held)
    {
        return false;
    }
    if (*held != *target)
    {
        Log(L"check failed: {} (slot IDA 0x{:X}) does not hold {} (a game update, or another mod replaced it)",
            fact.what, fact.slot, fact.target.what);
        return false;
    }
    if (!entry.Replace(*slot, *target, replacement))
    {
        Log(L"could not replace {} (slot IDA 0x{:X})", fact.what, fact.slot);
        return false;
    }
    return true;
}

const CodeSite* FirstDiffering(const GameImage& image, std::span<const CodeSite> sites) noexcept
{
    for (const CodeSite& site : sites)
    {
        if (!image.BytesMatch(site.address, site.bytes))
        {
            return &site;
        }
    }
    return nullptr;
}

}  // namespace lss::platform
