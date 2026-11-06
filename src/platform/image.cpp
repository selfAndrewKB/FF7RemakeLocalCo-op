#include "platform/image.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstring>

namespace lss::platform {

namespace {

constexpr std::uintptr_t kIdaImageBase = 0x140000000;

std::uintptr_t g_locatedBase = 0;  // set once by Locate at the start (game thread), then only read: the free IdaAddress

/// True if every byte of [address, address + length) is committed and readable. Lying inside the
/// image is not enough: a page can still be inaccessible, and reading it would crash the game
/// instead of letting a startup check fail cleanly.
bool IsReadable(std::uintptr_t address, std::size_t length) noexcept
{
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const std::uintptr_t end = address + length;
    while (address < end)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info) ||
            info.State != MEM_COMMIT || (info.Protect & kReadable) == 0 || (info.Protect & PAGE_GUARD) != 0)
        {
            return false;
        }
        address = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

}  // namespace

std::optional<GameImage> GameImage::Locate() noexcept
{
    const HMODULE module = GetModuleHandleW(kGameExe);
    if (module == nullptr)
    {
        return std::nullopt;
    }

    // The loader has already validated these headers; SizeOfImage bounds every address we use.
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt   = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    g_locatedBase   = base;
    return GameImage(base, nt->OptionalHeader.SizeOfImage);
}

std::uintptr_t IdaAddress(std::uintptr_t live) noexcept
{
    return g_locatedBase == 0 ? 0 : live - g_locatedBase + kIdaImageBase;
}

std::optional<std::uintptr_t> GameImage::Rebase(std::uintptr_t idaAddress, std::size_t length) const noexcept
{
    if (idaAddress < kIdaImageBase)
    {
        return std::nullopt;
    }
    const std::uintptr_t offset = idaAddress - kIdaImageBase;
    if (offset > size_ || length > size_ - offset)
    {
        return std::nullopt;
    }
    return base_ + offset;
}

bool GameImage::BytesMatch(std::uintptr_t idaAddress, std::span<const std::uint8_t> expected) const noexcept
{
    const auto live = Rebase(idaAddress, expected.size());
    return live && IsReadable(*live, expected.size()) &&
           std::memcmp(reinterpret_cast<const void*>(*live), expected.data(), expected.size()) == 0;
}

std::optional<std::uintptr_t> GameImage::ReadPointer(std::uintptr_t idaAddress) const noexcept
{
    const auto live = Rebase(idaAddress, sizeof(std::uintptr_t));
    if (!live || !IsReadable(*live, sizeof(std::uintptr_t)))
    {
        return std::nullopt;
    }
    return *reinterpret_cast<const std::uintptr_t*>(*live);
}

}  // namespace lss::platform
