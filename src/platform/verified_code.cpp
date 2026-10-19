#include "platform/verified_code.h"

#include "platform/log.h"

#include <format>
#include <string>

namespace lss::platform {

std::optional<std::uintptr_t> FindFunction(const GameImage& image, const FunctionFact& fact) noexcept
{
    const auto live = image.Rebase(fact.address);
    if (!live || !image.BytesMatch(fact.address, fact.entryBytes))
    {
        // The live bytes go in the line: a jump there means another hook (UE4SS, a Lua mod) took the function first.
        std::wstring bytes;
        for (std::size_t i = 0; live && i < 8; ++i)
        {
            bytes += std::format(L"{:02X} ", *reinterpret_cast<const std::uint8_t*>(*live + i));
        }
        Log(L"check failed: {} (IDA 0x{:X}) does not start with the expected bytes (live: {})", fact.what, fact.address,
            live ? bytes : L"outside the image");
        return std::nullopt;
    }
    return live;
}

bool ReplaceSlot(const GameImage& image, const SlotFact& fact, std::uintptr_t replacement, VTableEntry& entry) noexcept
{
    auto target = FindFunction(image, fact.target);
    if (!target)
    {
        // Another hook (UE4SS, a Lua mod) may have put a jump at the entry; the slot still names the function, and a
        // slot replacement never touches its bytes, so the rest of the entry is checked instead.
        constexpr std::size_t kJump = 5;
        const auto            live  = image.Rebase(fact.target.address);
        if (live && fact.target.entryBytes.size() > kJump && *reinterpret_cast<const std::uint8_t*>(*live) == 0xE9 &&
            image.BytesMatch(fact.target.address + kJump, fact.target.entryBytes.subspan(kJump)))
        {
            Log(L"{}: another hook's jump sits at its entry; the slot is replaced behind it", fact.target.what);
            target = live;
        }
    }
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

}  // namespace lss::platform
