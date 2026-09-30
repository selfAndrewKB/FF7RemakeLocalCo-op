#include "platform/verified_code.h"

#include "platform/log.h"

namespace lss::platform {

std::optional<std::uintptr_t> FindFunction(const GameImage& image, const FunctionFact& fact) noexcept
{
    const auto live = image.Rebase(fact.address);
    if (!live || !image.BytesMatch(fact.address, fact.entryBytes))
    {
        Log(L"check failed: {} (IDA 0x{:X}) does not start with the expected bytes", fact.what, fact.address);
        return std::nullopt;
    }
    return live;
}

bool ReplaceSlot(const GameImage& image, const SlotFact& fact, std::uintptr_t replacement, VTableEntry& entry) noexcept
{
    const auto target = FindFunction(image, fact.target);
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
