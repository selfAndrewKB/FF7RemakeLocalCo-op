#pragma once

// Every engine address and offset the mod relies on, as read in IDA (image base 0x140000000) for
// ff7remake_.exe SHA-256 25B54C34...6635, with the research document that established it
// (Remake/Documentation/research/). Each one is checked against the running game before use.

#include "platform/verified_code.h"

#include <array>
#include <cstdint>

namespace lss::facts {

// --- Reflected offsets (UE4SS CXX header dump, Engine.hpp) -------------------------------------
inline constexpr std::uint32_t kActorOwner                = 0x88;   // AActor::Owner
inline constexpr std::uint32_t kLocalPlayerController     = 0x30;   // UPlayer::PlayerController
inline constexpr std::uint32_t kControllerPawn            = 0x380;  // AController::Pawn
inline constexpr std::uint32_t kControllerCameraManager   = 0x410;  // APlayerController::PlayerCameraManager
inline constexpr std::uint32_t kCameraManagerPcOwner      = 0x378;  // APlayerCameraManager::PCOwner

// --- End camera manager (RE-players-viewport.md §1, RE-camera.md) -------------------------------
inline constexpr std::uint32_t kCameraManagerMainCamera   = 0x1E38; // AEndPlayerCameraManager::MainCameraActor
inline constexpr std::uint32_t kControllerBattleTalkActor = 0x708;  // AEndPlayerController: spawned at BeginPlay, never destroyed

inline constexpr std::uintptr_t kEndCameraManagerVTable   = 0x144D150B8;
inline constexpr std::uintptr_t kEndPlayerControllerVTable = 0x144C16C90;

inline constexpr std::array<std::uint8_t, 16> kCameraManagerBeginPlayEntry{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55};

/// AEndPlayerCameraManager::BeginPlay, vtable slot 94. It spawns the main camera actor and makes it the
/// view target; for a player joining during play it runs inside the spawn, before PCOwner is set.
inline constexpr platform::SlotFact kCameraManagerBeginPlay{
    L"AEndPlayerCameraManager::BeginPlay (slot 94)", kEndCameraManagerVTable + 94 * 8,
    {L"AEndPlayerCameraManager::BeginPlay", 0x141686C00, kCameraManagerBeginPlayEntry}};

/// The ANSI name the camera manager's BeginPlay gives its main camera actor (14 characters). Its only
/// reader is the `lea rcx` at 0x141686EFB inside BeginPlay.
inline constexpr std::uintptr_t kEndCameraActorName = 0x14440A2A0;
inline constexpr std::array<char, 14> kEndCameraActorText{'E', 'n', 'd', 'C', 'a', 'm', 'e', 'r', 'a', 'A', 'c', 't', 'o', 'r'};
/// Player 2's actor gets a name of the same length, so the spawn's duplicate-name replacement of player 1's
/// actor (UE 4.18 StaticAllocateObject reuses an existing object of that name) cannot happen.
inline constexpr std::array<char, 14> kPlayerTwoCameraActorText{'E', 'n', 'd', 'C', 'a', 'm', 'e', 'r', 'a', 'A', 'c', 't', 'r', '2'};

// --- Local players (RE-00-triage.md) -------------------------------------------------------------
inline constexpr std::array<std::uint8_t, 20> kRemoveLocalPlayerEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
    0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20};
/// UGameInstance::RemoveLocalPlayer(ULocalPlayer*): bool.
inline constexpr platform::FunctionFact kRemoveLocalPlayer{L"UGameInstance::RemoveLocalPlayer", 0x142F3D3B0,
                                                           kRemoveLocalPlayerEntry};

// --- Player 2's field camera (RE-camera.md §9, RE-impl-facts-v0.2.md) ----------------------------
inline constexpr std::uintptr_t kPartyManagerGlobal   = 0x1457E9A88;  // AEndPartyManager* (heap); +0 = primary PC
inline constexpr std::uintptr_t kEndGameRootGlobal    = 0x1457E99D0;  // root object*; +0x9B0/+0x9B4 = the operator's weak pair
inline constexpr std::uint32_t  kRootOperatorWeak     = 0x9B0;
inline constexpr std::uintptr_t kGameModeGlobal       = 0x1457E9AB0;  // AEndGameMode*; vtable +0x140 = GetWorld
inline constexpr std::uint32_t  kGameModeGetWorldSlot = 0x140;
inline constexpr std::uintptr_t kOperatorFrameStamp   = 0x1457E99C8;  // u64, written by the operator constructor
inline constexpr std::uintptr_t kOperatorCorrections  = 0x1453A2A51;  // u8, written by the operator constructor
inline constexpr std::uintptr_t kNameNoneIndex        = 0x145991B80;  // u32, NAME_None's comparison index
inline constexpr std::uintptr_t kObjectArrayObjects   = 0x1453BD480;  // FUObjectItem* (flat, 0x18 per item)
inline constexpr std::uintptr_t kObjectArrayCount     = 0x1453BD48C;  // i32
inline constexpr std::uintptr_t kObjectArray          = 0x1453BD470;  // GUObjectArray (the serial allocator's this)
inline constexpr std::uint32_t  kObjectIndex          = 0x0C;         // UObjectBase::InternalIndex
inline constexpr std::uint32_t  kOperatorOwnerChara   = 0x5D0;        // AEndFieldCameraOperator's owner character
inline constexpr std::uint32_t  kOperatorObjectFlags  = 0x48;         // the game's spawn flags for the operator
inline constexpr std::uint8_t   kAerithType           = 3;            // EPlayerType

inline constexpr std::uintptr_t kFieldCameraOperatorVTable = 0x144BB5D78;
inline constexpr std::uintptr_t kSquareInputOwnerGlobal    = 0x1459CA980;  // SquareInput owner*
inline constexpr std::uint32_t  kStickBanks        = 0x6D0;  // TArray<record> per device bank, 16 bytes each
inline constexpr std::uint32_t  kXInputBank        = 1;
inline constexpr std::uint32_t  kStickRecordBytes  = 24;     // LX, LY, RX, RY, LT, RT
inline constexpr std::uint32_t  kStickAnalog       = 0x700;  // LX, LY, RX, RY: what the getters read
inline constexpr std::uint32_t  kStickDigital      = 0x6A0;  // key-driven stick, wins when non-zero

inline constexpr std::array<std::uint8_t, 16> kOperatorBeginPlayEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55};
inline constexpr std::array<std::uint8_t, 16> kOperatorTickEntry{
    0x40, 0x55, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x28, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xD8, 0x02};
inline constexpr std::array<std::uint8_t, 16> kProcessPlayerInputEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0xA9};
inline constexpr std::array<std::uint8_t, 16> kSetOwnerCharaEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55};
inline constexpr std::array<std::uint8_t, 16> kSpawnActorEntry{
    0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48};
inline constexpr std::array<std::uint8_t, 16> kGetPlayerActorEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
inline constexpr std::array<std::uint8_t, 16> kAllocateSerialEntry{
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x3B, 0x15, 0xD0, 0xF9, 0x50, 0x03, 0x7D, 0x14, 0x48, 0x63};
inline constexpr std::array<std::uint8_t, 16> kOperatorStaticClassEntry{
    0x4C, 0x8B, 0xDC, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05, 0x47, 0xB5, 0x9C};

/// AEndFieldCameraOperator::BeginPlay (slot 94): binds the leader delegate on the primary controller.
inline constexpr platform::SlotFact kOperatorBeginPlay{
    L"AEndFieldCameraOperator::BeginPlay (slot 94)", kFieldCameraOperatorVTable + 94 * 8,
    {L"AEndFieldCameraOperator::BeginPlay", 0x140FDA310, kOperatorBeginPlayEntry}};
/// AEndFieldCameraOperator::Tick (slot 129, void(this, float)): writes the primary camera manager's slot 24.
inline constexpr platform::SlotFact kOperatorTick{
    L"AEndFieldCameraOperator::Tick (slot 129)", kFieldCameraOperatorVTable + 129 * 8,
    {L"AEndFieldCameraOperator::Tick", 0x140FDDAD0, kOperatorTickEntry}};
/// AEndPlayerController::ProcessPlayerInput (slot 376, void(this, float, bool)): the right stick into the operator.
inline constexpr platform::SlotFact kProcessPlayerInput{
    L"AEndPlayerController::ProcessPlayerInput (slot 376)", kEndPlayerControllerVTable + 376 * 8,
    {L"AEndPlayerController::ProcessPlayerInput", 0x1415F50E0, kProcessPlayerInputEntry}};

inline constexpr std::array<std::uint8_t, 16> kFieldMoveEntry{
    0x40, 0x55, 0x53, 0x48, 0x8D, 0x6C, 0x24, 0xD8, 0x48, 0x81, 0xEC, 0x28, 0x01, 0x00, 0x00, 0x44};
/// AEndPlayerController::Tick_FieldMove (slot 129, void(this, float)): the left stick moves its own leader.
inline constexpr platform::SlotFact kFieldMove{
    L"AEndPlayerController::Tick_FieldMove (slot 129)", kEndPlayerControllerVTable + 129 * 8,
    {L"AEndPlayerController::Tick_FieldMove", 0x1415F5990, kFieldMoveEntry}};

// --- Split-screen show flags (RE-render-views.md) ------------------------------------------------
// Stock EngineShowFlagOverride (0x14322C1D0) clears these show-flag bits whenever the viewport is split; its last
// step is `flags = Force1 | (flags & ~Force0)` with two engine masks that only the static initialiser writes (zero).
inline constexpr std::uintptr_t kShowFlagForce1Mask   = 0x1453E39D4;  // u32: bits forced on for every view
inline constexpr std::uint32_t  kSplitDisabledFlags   = 0x100A;       // 0x8 TemporalAA; 0x2, 0x1000 [ASSUMPTION] Bloom, MotionBlur

// --- The game's own input tick (RE-input-p2.md §1) ------------------------------------------------
// AEndPlayerController's constructor turns off the stock per-controller input tick in PlayerTick for gameplay;
// the game ticks only PartyManager+0's input, from its world-tick phase hook (phase 1, start), through a global
// function pointer set at runtime.
inline constexpr std::uintptr_t kWorldTickPhaseHookGlobal = 0x145994E28;  // void (*)(UWorld*, void*, int32 phase, bool start)
inline constexpr std::int32_t   kInputTickPhase           = 1;
inline constexpr std::uint32_t  kControllerPlayerInput    = 0x498;        // APlayerController::PlayerInput
inline constexpr std::array<std::uint8_t, 16> kWorldTickPhaseHookEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48};
inline constexpr std::array<std::uint8_t, 16> kTickPlayerInputEntry{
    0x48, 0x8B, 0xC4, 0x56, 0x41, 0x56, 0x48, 0x81, 0xEC, 0xD8, 0x00, 0x00, 0x00, 0x0F, 0x29, 0x70};
inline constexpr platform::FunctionFact kWorldTickPhaseHook{L"EndGame_WorldTickPhaseHook", 0x1416BF9D0,
                                                            kWorldTickPhaseHookEntry};
/// Stock APlayerController::TickPlayerInput(float DeltaSeconds, bool bGamePaused): PlayerInput tick, then
/// ProcessPlayerInput (slot 376). Does not check PlayerInput for null in shipping.
inline constexpr platform::FunctionFact kTickPlayerInput{L"APlayerController::TickPlayerInput", 0x1431BC2B0,
                                                         kTickPlayerInputEntry};

inline constexpr platform::FunctionFact kSetOwnerChara{L"AEndFieldCameraOperator::SetOwnerChara", 0x140FDFB50,
                                                       kSetOwnerCharaEntry};
inline constexpr platform::FunctionFact kSpawnActor{L"UWorld::SpawnActor", 0x143007550, kSpawnActorEntry};
inline constexpr platform::FunctionFact kGetPlayerActor{L"PartyManager_GetPlayerActor", 0x1415F0650, kGetPlayerActorEntry};
inline constexpr platform::FunctionFact kAllocateSerialNumber{L"FUObjectArray::AllocateSerialNumber", 0x141EADAB0,
                                                              kAllocateSerialEntry};
inline constexpr platform::FunctionFact kOperatorStaticClass{L"AEndFieldCameraOperator::StaticClass", 0x140FD9850,
                                                             kOperatorStaticClassEntry};

}  // namespace lss::facts
