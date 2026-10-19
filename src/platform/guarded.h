#pragma once

// Structured-exception guards for touching engine memory and calling engine code.
//
// Where the mod reads a field the engine might have freed, or calls the engine in a state it never
// produces itself, it guards the access with __try/__except, so a fault is reported and that feature
// stops instead of the game ending. MSVC allows __try only in functions without objects that need
// unwinding, so each guard is a small function of its own. A stack overflow is passed on: the guard
// frame cannot recover it.

#include <cstddef>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // this header is included beside UE4SS's, which use std::numeric_limits<>::max()
#endif
#include <Windows.h>

namespace lss::platform {

inline int GuardFilter(unsigned long code) noexcept
{
    return code == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER;
}

/// memcpy from memory that may not be readable. False if it faulted.
bool CopyGuarded(void* out, const void* from, std::size_t length) noexcept;

/// memcpy to memory that may not be writable. False if it faulted.
bool WriteGuarded(void* to, const void* from, std::size_t length) noexcept;

/// A value read from memory that may not be readable. False if the address is null or the read faulted.
template <class T>
bool Read(const void* address, T& out) noexcept
{
    return address != nullptr && CopyGuarded(&out, address, sizeof(T));
}

/// A pointer field of `object` at `offset`; null if the object is null or the read faulted.
inline void* ReadPointer(const void* object, std::uint32_t offset) noexcept
{
    void* value = nullptr;
    return object != nullptr && Read(static_cast<const std::uint8_t*>(object) + offset, value) ? value : nullptr;
}

/// Calls an engine function, its result into `out`. False if it faulted.
template <class R, class... P, class... A>
bool CallGuarded(R (*fn)(P...), R& out, A... args) noexcept
{
    __try
    {
        out = fn(args...);
        return true;
    }
    __except (GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// Calls an engine function with no result. False if it faulted.
template <class... P, class... A>
bool CallGuarded(void (*fn)(P...), A... args) noexcept
{
    __try
    {
        fn(args...);
        return true;
    }
    __except (GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

}  // namespace lss::platform
