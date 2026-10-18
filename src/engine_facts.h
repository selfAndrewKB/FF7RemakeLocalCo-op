#pragma once

// Every engine address and offset the mod relies on, as read in IDA (image base 0x140000000) for
// ff7remake_.exe SHA-256 25B54C34...6635, with the research document that established it
// (Remake/Documentation/research/). Functions and vtable slots are checked against the running game
// (entry bytes) before use, and redirected calls against their call bytes; plain offsets are not.

#include "platform/verified_code.h"

#include <array>
#include <cstddef>
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

// --- The game's state machine and field step (RE-lifecycle-world.md §1, §2, §3) --------------------
// GS = [GM+0x3D0] (stock AGameModeBase::GameState; the game checks IsA_AEndGameState at every use). ROOT = the
// field root at kEndGameRootGlobal. Offsets from the reflection dump and the research [ASSUMPTION: not yet
// cross-checked against reflection at run time]; values logged on change (players/game_state).
inline constexpr std::size_t    kGameModeGameState    = 0x3D0;  // AGameModeBase::GameState
inline constexpr std::size_t    kGameStateState       = 0x3B8;  // EGameState byte: 4 title, 5 loading, 6 play, 7 pause, 8 menu, 9 leaving, 10 chapter-start pause
inline constexpr std::size_t    kGameStateMenuKind    = 0x4FC;  // the menu's kind while the state is 8
inline constexpr std::size_t    kGameStatePause       = 0x4F0;  // execIsGamePause: != 0 while paused
inline constexpr std::size_t    kFieldRootStep        = 0x70;   // dword: the field step machine (13 = steady field, 14-20 exit steps)
inline constexpr std::size_t    kFieldRootRunning     = 0x74;   // byte: the step machine runs
inline constexpr std::size_t    kFieldRootLocation    = 0x78;   // FName: the current location
inline constexpr std::uint32_t  kGameModeGetWorldSlot = 0x140;

// --- The field exit (RE-lifecycle-world.md §3.1, §8) ----------------------------------------------
inline constexpr std::array<std::uint8_t, 10> kRequestFieldExitEntry{0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x80, 0x79, 0x74, 0x00};
/// EndGameRoot_RequestFieldExit_ClearRunning74(root): clears the running byte, destroys the field camera operator,
/// resets the party's components. Every retry, load, jump and return to the title reaches it; IDA lists two
/// code references (below) and no pointer to it.
inline constexpr platform::FunctionFact kRequestFieldExit{L"EndGameRoot_RequestFieldExit_ClearRunning74", 0x140FF6E00,
                                                          kRequestFieldExitEntry};
inline constexpr std::uintptr_t kFieldExitStateCall     = 0x1416C20B6;  // AEndGameState_OnEnterState(9): call
inline constexpr std::uintptr_t kFieldExitBlueprintJump = 0x141AC7D5A;  // UEndFieldAPI::execExitLoation: tail jmp
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
inline constexpr std::uint32_t  kSplitDisabledFlags   = 0x100A;       // 0x8 TemporalAA, 0x2 Bloom; 0x1000 [ASSUMPTION] MotionBlur
/// What the mod forces without the bloom fix: bit 0x2 (Bloom) leaks player 1's moving content into player 2's half
/// (live A/B, Documentation/plan/topics/render.md) unless render/bloom_origin is installed, which raises the mask to kSplitDisabledFlags.
inline constexpr std::uint32_t  kSplitForcedFlags     = 0x1008;

// --- AO downsample stage-0 origin (RE-rdc-split-frame.md, RE-impl-facts-ao-fix.md) ------------------
// SE_AODownsamplePS0__SetParameters is shared by AO downsample stages 0/1/2; only stage 0's pixel shader reads
// the taps (cb0[19], cb0[20]), at ftoi(tap) + 2*SV_Position, without View.ViewRect.Min.
inline constexpr std::uint32_t kPassContextView = 0x00;  // FRenderingCompositePassContext: const FViewInfo*
inline constexpr std::uint32_t kViewRectMinX    = 0x70;  // FViewInfo::ViewRect.Min.X (int32)
inline constexpr std::uint32_t kViewRectMinY    = 0x74;

inline constexpr std::array<std::uint8_t, 16> kAoDownsampleSetParamsEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41, 0x55, 0x41, 0x56};
/// SE_AODownsamplePS0__SetParameters(FShader* ps, FRHICommandList* rhi, const PassContext* ctx).
inline constexpr platform::FunctionFact kAoDownsampleSetParams{L"SE_AODownsamplePS0__SetParameters", 0x1421D0D70,
                                                               kAoDownsampleSetParamsEntry};
inline constexpr std::array<std::uint8_t, 16> kSetShaderValueVec4Entry{
    0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x41, 0x0F, 0xB7};
/// RHICmdList__SetShaderValue_Vec4(FRHICommandList*, FRHIPixelShader*, const FShaderParameter*, const void* value):
/// copies the 16-byte value into the command list at once. Thousands of callers: call sites only.
inline constexpr platform::FunctionFact kSetShaderValueVec4{L"RHICmdList__SetShaderValue_Vec4", 0x1421C8A80,
                                                            kSetShaderValueVec4Entry};
inline constexpr std::uintptr_t kAoStage0SetParamsCall = 0x1421CED47;  // stage-0-only call of the shared SetParameters
inline constexpr std::uintptr_t kAoTapsCallA           = 0x1421D0FC3;  // taps 0,1 -> cb0[19]
inline constexpr std::uintptr_t kAoTapsCallB           = 0x1421D0FD9;  // taps 2,3 -> cb0[20]

// --- Bloom level-0 origin (RE-split-bit2-leak.md, RE-impl-facts-bloom-swap.md) ----------------------------------
// SE_RCPassBloomReduce__Process calls one of two level-0 binders (rcx = pass context, context+0 = FViewInfo*); each
// builds a 0xA8-byte FGraphicsPipelineStateInitializer (PS at +0x10, a raw pointer) and passes it to the stock
// engine pipeline cache, which copies it as its key.
inline constexpr std::uintptr_t kBloomL0BindCall          = 0x142286533;  // E8 -> 0x1422A4C20 SE_BloomReduce__BindFirst
inline constexpr std::uintptr_t kBloomL0BindExactCall     = 0x14228653A;  // E8 -> 0x1422A5050 SE_BloomReduce__BindFirstExact2x
inline constexpr std::array<std::uint8_t, 16> kBloomL0BindEntry{  // the same prologue in both binders
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C};
/// SE_BloomReduce__BindFirst / __BindFirstExact2x(context, FVertexShaderRHIRef* out, const float4* srcRect,
/// const float4* dstRect).
inline constexpr platform::FunctionFact kBloomL0Bind{L"SE_BloomReduce__BindFirst", 0x1422A4C20, kBloomL0BindEntry};
inline constexpr platform::FunctionFact kBloomL0BindExact{L"SE_BloomReduce__BindFirstExact2x", 0x1422A5050,
                                                          kBloomL0BindEntry};
inline constexpr std::uintptr_t kBloomL0PipelineCall      = 0x1422A4ED8;  // in BindFirst
inline constexpr std::uintptr_t kBloomL0ExactPipelineCall = 0x1422A5308;  // in BindFirstExact2x
inline constexpr std::array<std::uint8_t, 16> kGetAndOrCreateGraphicsPsoEntry{
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0xA0, 0x48, 0x81, 0xEC, 0x60};
/// PipelineStateCache::GetAndOrCreateGraphicsPipelineState(FRHICommandList*, const Initializer*, uint32 applyFlags):
/// stock 4.18, 490 callers: call sites only.
inline constexpr platform::FunctionFact kGetAndOrCreateGraphicsPso{
    L"PipelineStateCache::GetAndOrCreateGraphicsPipelineState", 0x1426DE680, kGetAndOrCreateGraphicsPsoEntry};
inline constexpr std::size_t   kPsoInitializerSize = 0xA8;
inline constexpr std::uint32_t kPsoInitPixelShader = 0x10;

// The RHI. D3D12 unless the command line has -d3d11, -dx11 or -sm5 (PlatformCreateDynamicRHI 0x1426E6AA0).
inline constexpr std::uintptr_t kGDynamicRHI              = 0x14582C478;  // FDynamicRHI*
inline constexpr std::uintptr_t kGRHICommandListImmediate = 0x14593F4D8;  // FRHICommandListImmediate (object)
inline constexpr std::uintptr_t kVtblD3D12DynamicRHI      = 0x144D3B3B0;
inline constexpr std::uintptr_t kVtblD3D11DynamicRHI      = 0x144D39718;
inline constexpr std::uintptr_t kVtblD3D12PixelShader     = 0x144D3B278;  // FD3D12PixelShader (200 bytes)
inline constexpr std::uintptr_t kVtblD3D11PixelShader     = 0x144D39000;  // FD3D11PixelShader (168 bytes)
/// FDynamicRHI slot 144, CreatePixelShader_RenderThread(rhi, FPixelShaderRHIRef* out, FRHICommandListImmediate*,
/// const TArray<uint8>* code): the engine's own render-thread creation. D3D12 uses the base, which stalls the RHI
/// thread around slot 12; D3D11 overrides it and creates at once (its device is free-threaded). `*out` comes back
/// holding one reference.
inline constexpr std::uint32_t  kRhiSlotCreatePixelShaderRT = 144;
inline constexpr std::array<std::uint8_t, 16> kCreatePixelShaderRTEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
inline constexpr platform::FunctionFact kD3D12CreatePixelShaderRT{L"FDynamicRHI::CreatePixelShader_RenderThread",
                                                                  0x1426E47C0, kCreatePixelShaderRTEntry};
inline constexpr std::array<std::uint8_t, 16> kD3D11CreatePixelShaderRTEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x4D, 0x8B, 0xC1, 0x48, 0x8B, 0xDA, 0xE8, 0xFF, 0xFC, 0xFF};
inline constexpr platform::FunctionFact kD3D11CreatePixelShaderRT{
    L"FD3D11DynamicRHI::CreatePixelShader_RenderThread", 0x141F3EAF0, kD3D11CreatePixelShaderRTEntry};
/// FRHIShader::Hash: the shader compiler's OutputHash (SHA-1 of the code and parameter map), written after creation
/// by RHICreatePixelShader_LibraryOrCode 0x141EFE440 on either RHI.
inline constexpr std::uint32_t kRhiShaderHash = 0x18;

/// The original level-0 reduce shader's OutputHash, read from the running game (D3D12, 2026-09-30,
/// tools/bloom_shader/find_live_hash.py). It identifies the shader on both RHIs; D3D11 keeps no copy of the code.
inline constexpr std::array<std::uint8_t, 20> kBloomL0ShaderHash{
    0x8E, 0x8D, 0x85, 0xC6, 0x4A, 0x88, 0x3B, 0x86, 0x8A, 0x69, 0x96, 0xB8, 0xCF, 0xDC, 0xBA, 0xB2, 0x0B, 0x5F, 0x6D, 0x9C};
// The engine's framing of that shader's code blob, around the DXBC ([resource tables][DXBC][optional data][footer]),
// as logged from the D3D12 shader object's code copy (2026-09-30): empty resource tables; optional data 'p' (packed
// resource counts 01 01 01 01 00) and 'u' (size 4, zero); footer 23 = the optional data's size, footer included.
inline constexpr std::array<std::uint8_t, 24> kBloomL0CodePrefix{};
inline constexpr std::array<std::uint8_t, 23> kBloomL0CodeTrailer{
    0x70, 0x05, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x00, 0x75, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x17, 0x00, 0x00, 0x00};

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
