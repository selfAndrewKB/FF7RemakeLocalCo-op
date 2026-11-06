// Host-side tests for platform::EntryRedirect, run without the game.
//
// A small executable page stands in for the game's code: a function whose first instruction is
// `mov [rsp+8], rbx` (48 89 5C 24 08), as EndUI23_SetCommandCategoryEnabled's is. The
// redirected function is EXECUTED: the replacement must run with the caller's argument and return
// address, call the original through the trampoline (which runs the replaced store), and the
// original must come back after Restore.

#include "platform/entry_redirect.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <intrin.h>

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

using IntFn = int (*)(int);

constexpr std::uint8_t kFirst[] = {0x48, 0x89, 0x5C, 0x24, 0x08};  // mov [rsp+8], rbx

/// page+0x00: caller: sub rsp, 28h ; mov ecx, 5 ; call function (at +0x09) ; add rsp, 28h ; ret
/// page+0x40: function: mov [rsp+8], rbx ; mov rbx, [rsp+8] ; lea eax, [rcx+1] ; ret
///            (returns its argument + 1; reloads rbx from the store the trampoline makes)
/// page+0x7C: the same first instruction, crossing the block at 0x80
class GameLikeCode
{
public:
    static constexpr std::size_t kCallOffset     = 0x09;
    static constexpr std::size_t kReturnOffset   = 0x0E;
    static constexpr std::size_t kFunctionOffset = 0x40;
    static constexpr std::size_t kCrossingOffset = 0x7C;

    GameLikeCode()
    {
        memory_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        const std::uint8_t caller[] = {0x48, 0x83, 0xEC, 0x28,        // sub rsp, 28h
                                       0xB9, 0x05, 0x00, 0x00, 0x00,  // mov ecx, 5
                                       0xE8, 0x00, 0x00, 0x00, 0x00,  // call function (displacement below)
                                       0x48, 0x83, 0xC4, 0x28,        // add rsp, 28h
                                       0xC3};                         // ret
        const std::uint8_t function[] = {0x48, 0x89, 0x5C, 0x24, 0x08,  // mov [rsp+8], rbx
                                         0x48, 0x8B, 0x5C, 0x24, 0x08,  // mov rbx, [rsp+8]
                                         0x8D, 0x41, 0x01,              // lea eax, [rcx+1]
                                         0xC3};                         // ret
        std::memcpy(memory_, caller, sizeof(caller));
        const std::int32_t rel = static_cast<std::int32_t>(kFunctionOffset - (kCallOffset + 5));
        std::memcpy(memory_ + kCallOffset + 1, &rel, sizeof(rel));
        std::memcpy(memory_ + kFunctionOffset, function, sizeof(function));
        std::memcpy(memory_ + kCrossingOffset, function, sizeof(function));
        DWORD previous = 0;
        VirtualProtect(memory_, 0x1000, PAGE_EXECUTE_READ, &previous);
        FlushInstructionCache(GetCurrentProcess(), memory_, 0x1000);
    }
    ~GameLikeCode() { VirtualFree(memory_, 0, MEM_RELEASE); }

    std::uintptr_t At(std::size_t offset) const { return reinterpret_cast<std::uintptr_t>(memory_) + offset; }
    int            RunCaller() const { return reinterpret_cast<int (*)()>(memory_)(); }
    int            Call(std::size_t offset, int value) const { return reinterpret_cast<IntFn>(At(offset))(value); }

private:
    std::uint8_t* memory_ = nullptr;
};

lss::platform::EntryRedirect* g_redirect      = nullptr;
std::uintptr_t                g_returnAddress = 0;
int                           g_argument      = 0;

/// Records its argument and where it was called from, and adds 10 to what the original returns.
__declspec(noinline) int Replacement(int value)
{
    g_returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    g_argument      = value;
    return 10 + reinterpret_cast<IntFn>(g_redirect->Original())(value);
}

void TestRedirectRunsAndRestores()
{
    std::puts("entry redirect: the replacement runs, calls the original through the trampoline, and is removed");
    const GameLikeCode game;
    EXPECT(game.Call(GameLikeCode::kFunctionOffset, 5) == 6);

    lss::platform::EntryRedirect redirect;
    g_redirect = &redirect;
    EXPECT(redirect.Original() == 0);
    EXPECT(redirect.Install(game.At(GameLikeCode::kFunctionOffset), kFirst,
                            reinterpret_cast<std::uintptr_t>(&Replacement)));
    EXPECT(redirect.IsInstalled());
    EXPECT(redirect.Original() != 0);
    EXPECT(game.Call(GameLikeCode::kFunctionOffset, 5) == 16);
    EXPECT(g_argument == 5);
    EXPECT(game.Call(GameLikeCode::kFunctionOffset, 41) == 52);

    EXPECT(redirect.Restore());
    EXPECT(!redirect.IsInstalled());
    EXPECT(game.Call(GameLikeCode::kFunctionOffset, 5) == 6);
    EXPECT(redirect.Original() != 0);  // kept: a call inside the replacement may still use it
}

void TestRedirectKeepsTheCallersReturnAddress()
{
    std::puts("entry redirect: a game caller's call arrives with its own return address");
    const GameLikeCode           game;
    lss::platform::EntryRedirect redirect;
    g_redirect = &redirect;
    EXPECT(redirect.Install(game.At(GameLikeCode::kFunctionOffset), kFirst,
                            reinterpret_cast<std::uintptr_t>(&Replacement)));
    g_returnAddress = 0;
    EXPECT(game.RunCaller() == 16);
    EXPECT(g_returnAddress == game.At(GameLikeCode::kReturnOffset));
    EXPECT(g_argument == 5);
    EXPECT(redirect.Restore());
    EXPECT(game.RunCaller() == 6);
}

void TestRedirectRefusesWhatItDoesNotRecognise()
{
    std::puts("entry redirect: refuses other bytes, a short or crossing instruction, no replacement, or a second install");
    const GameLikeCode game;
    const auto         replacement = reinterpret_cast<std::uintptr_t>(&Replacement);
    const auto         function    = game.At(GameLikeCode::kFunctionOffset);
    lss::platform::EntryRedirect redirect;
    g_redirect = &redirect;
    constexpr std::uint8_t kOther[] = {0x48, 0x89, 0x6C, 0x24, 0x08};  // mov [rsp+8], rbp

    EXPECT(!redirect.Install(function, kOther, replacement));                                       // other bytes
    EXPECT(!redirect.Install(function, std::span<const std::uint8_t>(kFirst, 4), replacement));      // shorter than a jump
    EXPECT(!redirect.Install(game.At(GameLikeCode::kCrossingOffset), kFirst, replacement));          // 0x7C..0x80 crosses
    EXPECT(!redirect.Install(function, kFirst, 0));
    EXPECT(!redirect.IsInstalled());
    EXPECT(game.Call(GameLikeCode::kFunctionOffset, 5) == 6);  // untouched

    EXPECT(redirect.Install(function, kFirst, replacement));
    EXPECT(!redirect.Install(function, kFirst, replacement));
    EXPECT(redirect.Restore());
    EXPECT(game.Call(GameLikeCode::kFunctionOffset, 5) == 6);
}

constexpr std::uint8_t kSecond[] = {0x48, 0x8B, 0x5C, 0x24, 0x08};  // mov rbx, [rsp+8]

/// The copy at +0x7C: its first instruction crosses the block at 0x80, its second (0x81..0x85) does not,
/// and reads back the slot the first one stores, so the trampoline must replay both, in order (the shape
/// of 1.005's FieldActionMgr_AddPoint).
void TestSecondInstructionRedirect()
{
    std::puts("entry redirect: at the second instruction, for a first instruction that crosses a block");
    const GameLikeCode           game;
    const auto                   replacement = reinterpret_cast<std::uintptr_t>(&Replacement);
    const auto                   function    = game.At(GameLikeCode::kCrossingOffset);
    lss::platform::EntryRedirect redirect;
    g_redirect = &redirect;
    constexpr std::uint8_t kOther[] = {0x48, 0x8B, 0x6C, 0x24, 0x08};  // mov rbp, [rsp+8]

    EXPECT(!redirect.InstallAtSecondInstruction(function, kFirst, kOther, replacement));  // other second bytes
    EXPECT(!redirect.InstallAtSecondInstruction(function, kOther, kSecond, replacement));  // other first bytes
    EXPECT(!redirect.InstallAtSecondInstruction(function, kFirst, std::span<const std::uint8_t>(kSecond, 4),
                                                replacement));  // shorter than a jump
    EXPECT(!redirect.IsInstalled());
    EXPECT(game.Call(GameLikeCode::kCrossingOffset, 5) == 6);

    EXPECT(redirect.InstallAtSecondInstruction(function, kFirst, kSecond, replacement));
    EXPECT(game.Call(GameLikeCode::kCrossingOffset, 5) == 16);
    EXPECT(g_argument == 5);
    EXPECT(game.Call(GameLikeCode::kCrossingOffset, 41) == 52);
    EXPECT(!redirect.InstallAtSecondInstruction(function, kFirst, kSecond, replacement));  // already holds one
    EXPECT(redirect.Restore());
    EXPECT(game.Call(GameLikeCode::kCrossingOffset, 5) == 6);
}

}  // namespace

int main()
{
    TestRedirectRunsAndRestores();
    TestRedirectKeepsTheCallersReturnAddress();
    TestRedirectRefusesWhatItDoesNotRecognise();
    TestSecondInstructionRedirect();

    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
