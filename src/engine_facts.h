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
inline constexpr std::uint32_t kActorRootComponent        = 0x160;  // AActor::RootComponent
inline constexpr std::uint32_t kActorTags                 = 0x198;  // AActor::Tags (TArray<FName>)
inline constexpr std::uint32_t kObjectClass               = 0x10;   // UObjectBase::ClassPrivate
inline constexpr std::uint32_t kLocalPlayerController     = 0x30;   // UPlayer::PlayerController
inline constexpr std::uint32_t kControllerPawn            = 0x380;  // AController::Pawn
inline constexpr std::uint32_t kControllerControlRotation = 0x3A8;  // AController::ControlRotation (pitch, yaw, roll)
inline constexpr std::uint32_t kPawnController            = 0x3B0;  // APawn::Controller
inline constexpr std::uint32_t kControllerRotationInput   = 0x598;  // APlayerController::RotationInput (pitch +0x598, yaw +0x59C)
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
inline constexpr std::uint32_t  kOperatorControlYaw   = 0x42C;        // the control yaw the tick read from its controller (+0x428 pitch)
inline constexpr std::uint32_t  kOperatorFinalRotation = 0x4D8;       // the tick's final pitch/yaw (the Lock clamp's output, written back to the controller)
inline constexpr std::uint32_t  kGameModeFrameCounter = 0x770;        // AEndGameMode: the engine frame counter, copied at world-tick phase 100
inline constexpr std::uint32_t  kOperatorObjectFlags  = 0x48;         // the game's spawn flags for the operator
inline constexpr std::uint32_t  kSpawnParamsTemplate  = 0x08;         // FActorSpawnParameters::Template (RE-player-trigger.md §4.2)
inline constexpr std::uint32_t  kSpawnParamsObjectFlags = 0x2C;       // FActorSpawnParameters::ObjectFlags

inline constexpr std::uintptr_t kFieldCameraOperatorVTable = 0x144BB5D78;
inline constexpr std::uintptr_t kSquareInputOwnerGlobal    = 0x1459CA980;  // SquareInput owner*
inline constexpr std::uint32_t  kStickBanks        = 0x6D0;  // TArray<record> per device bank, 16 bytes each
inline constexpr std::uint32_t  kXInputBank        = 1;
inline constexpr std::uint32_t  kStickRecordBytes  = 24;     // LX, LY, RX, RY, LT, RT
inline constexpr std::uint32_t  kStickAnalog       = 0x700;  // LX, LY, RX, RY: what the getters read
inline constexpr std::uint32_t  kStickDigital      = 0x6A0;  // key-driven stick, wins when non-zero

// --- Per-pad buttons (RE-input.md §1, §3.2; RE-input-p2.md §2) ----------------------------------------------
// The owner's two action tables (TSet<Elem40>: an FName, state bytes at +0x18) and the real d-pad bits; the
// pad is known only in the key handlers, from the Slate event's UserIndex.
inline constexpr std::uintptr_t kSquareInputVTable   = 0x144BF24B0;  // IInputProcessor
inline constexpr std::uint32_t  kActionTableRaw      = 0xE8;         // read by every gameplay poller
inline constexpr std::uint32_t  kActionTableUi       = 0x138;        // the UI-filtered twin
inline constexpr std::uint32_t  kDpadBits            = 0x188;        // 8 bytes
inline constexpr std::uint32_t  kKeyEventUserIndex   = 0x0C;         // FKeyEvent: ControllerId = XInput user
inline constexpr std::uint32_t  kKeyEventKey         = 0x20;         // FKeyEvent: the key's FName
inline constexpr std::uint32_t  kStickBankCount      = 0x08;         // a bank's TArray num

inline constexpr std::array<std::uint8_t, 16> kInputTickEntry{
    0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48};
inline constexpr std::array<std::uint8_t, 16> kInputKeyDownEntry{
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
inline constexpr std::array<std::uint8_t, 16> kInputKeyUpEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
/// IInputProcessor::Tick(this, float dt, FSlateApplication&, TSharedRef<ICursor>): the tables' advance and the
/// stick aggregation, from Slate's FinishedInputThisFrame before the world ticks.
inline constexpr platform::SlotFact kInputTick{L"SquareInput::Tick (slot 1)", kSquareInputVTable + 1 * 8,
                                               {L"SquareInput::Tick", 0x141434550, kInputTickEntry}};
/// HandleKeyDownEvent(this, FSlateApplication&, const FKeyEvent&): maps the key to actions and dispatches each.
inline constexpr platform::SlotFact kInputKeyDown{L"SquareInput::HandleKeyDownEvent (slot 2)", kSquareInputVTable + 2 * 8,
                                                  {L"SquareInput::HandleKeyDownEvent", 0x141436720, kInputKeyDownEntry}};
inline constexpr platform::SlotFact kInputKeyUp{L"SquareInput::HandleKeyUpEvent (slot 3)", kSquareInputVTable + 3 * 8,
                                                {L"SquareInput::HandleKeyUpEvent", 0x141438280, kInputKeyUpEntry}};
/// SquareInput__ActionDown_UpdateTables_Dispatch(owner, unused, ActionEvent*, u8 alreadyHandled): the press into
/// the raw table, then (unless already handled) the UI route, the menu-core delegate and the UI table. 11 callers;
/// its first five instructions are pushes (8 bytes), replaced together: only the game thread runs it, and the
/// redirect is installed from that thread outside it.
inline constexpr std::array<std::uint8_t, 16> kActionDispatchEntry{
    0x40, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00, 0x48};
inline constexpr std::size_t kActionDispatchPushes = 8;
inline constexpr platform::FunctionFact kActionDispatch{L"SquareInput__ActionDown_UpdateTables_Dispatch", 0x141437B70,
                                                        kActionDispatchEntry};
/// SquareInput__RouteActionUpToUIHandlerStack(owner, ActionEvent*): the Up to the capturer and the focus path.
/// Its one call, at the end of SquareInput__ActionUp_UpdateTables.
inline constexpr std::array<std::uint8_t, 16> kRouteActionUpEntry{
    0x4C, 0x8B, 0xDC, 0x55, 0x53, 0x57, 0x49, 0x8D, 0xAB, 0xF8, 0xFA, 0xFF, 0xFF, 0x48, 0x81, 0xEC};
inline constexpr platform::FunctionFact kRouteActionUp{L"SquareInput__RouteActionUpToUIHandlerStack", 0x141443430,
                                                       kRouteActionUpEntry};
inline constexpr std::uintptr_t kRouteActionUpCall = 0x1414396A4;

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

// --- Distortion apply target size (RE-render-views.md 4A.1, the 2026-10-19 capture) -------------------------------
// SE_Distortion_ApplyPass writes the blurred distortion into SceneColor. It sets the viewport to the whole buffer
// (FSceneRenderTargets::BufferSize) but passes DrawRectangle the view's size as TargetSize, so in split screen player
// 1's rectangle covers the whole screen and player 2's lands off screen. The game stores BufferSize as the
// TextureSize argument at [rsp+50h] just before; the patch makes TargetSize load that instead of [rsp+70h] (the view
// size it built from ViewRect). Equal in single view, where the view fills the buffer.
inline constexpr std::array<std::uint8_t, 16> kDistortionApplyEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
/// SE_Distortion_ApplyPass(FRHICommandList*, FSceneRenderTargets*, const FViewInfo*, DistortBlurred).
inline constexpr platform::FunctionFact kDistortionApply{L"SE_Distortion_ApplyPass", 0x14220E5B0, kDistortionApplyEntry};
/// 0x14220EB37 through 0x14220EB74: DrawRectangle's TextureSize (BufferSize, [rsp+50h]) and TargetSize ([rsp+70h])
/// arguments, as the research read them.
inline constexpr std::uintptr_t kDistortionApplyArgsStart = 0x14220EB37;
inline constexpr std::uint8_t   kDistortionApplyArgs[]    = {
    0x48, 0x8B, 0x45, 0x98,                    // mov rax, [rbp-68h]       &FSceneRenderTargets
    0x66, 0x0F, 0x6E, 0xC1,                    // movd xmm0, ecx
    0x89, 0x4C, 0x24, 0x74,                    // mov [rsp+74h], ecx       view height
    0xB9, 0x01, 0x00, 0x00, 0x00,              // mov ecx, 1
    0x89, 0x4C, 0x24, 0x68,                    // mov [rsp+68h], ecx       InstanceCount
    0x48, 0x8B, 0x80, 0x28, 0x02, 0x00, 0x00,  // mov rax, [rax+228h]      BufferSize
    0x89, 0x4C, 0x24, 0x60,                    // mov [rsp+60h], ecx       Flags (the triangle)
    0x49, 0x8B, 0xCF,                          // mov rcx, r15
    0x48, 0x89, 0x74, 0x24, 0x58,              // mov [rsp+58h], rsi       VertexShader
    0x48, 0x89, 0x44, 0x24, 0x50,              // mov [rsp+50h], rax       TextureSize = BufferSize
    0x0F, 0x5B, 0xC0,                          // cvtdq2ps xmm0, xmm0
    0x89, 0x54, 0x24, 0x70,                    // mov [rsp+70h], edx       view width
    0x48, 0x8B, 0x44, 0x24, 0x70,              // mov rax, [rsp+70h]       TargetSize = the view size
    0x48, 0x89, 0x44, 0x24, 0x48,              // mov [rsp+48h], rax
};
inline constexpr std::uintptr_t kDistortionApplyTargetLoad = 0x14220EB6B;  // in the block at 0x14220EB60
inline constexpr std::uint8_t   kDistortionApplyTargetView[]   = {0x48, 0x8B, 0x44, 0x24, 0x70};  // mov rax, [rsp+70h]
inline constexpr std::uint8_t   kDistortionApplyTargetBuffer[] = {0x48, 0x8B, 0x44, 0x24, 0x50};  // mov rax, [rsp+50h]

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

// --- HUD (RE-hud-ui.md, RE-hud-split.md) ---------------------------------------------------------
/// SEndRootOverlay* (static singleton): the one Slate overlay every UEndUserWidget root is added to, rendered
/// into the one UI image each frame by SEndRootOverlay__RenderToInGameUILayer 0x141509D00.
inline constexpr std::uintptr_t kRootOverlayGlobal   = 0x14585A100;
inline constexpr std::uint32_t  kOverlayLayoutMin    = 0x388;  // float x, y: the UI rect's Min
inline constexpr std::uint32_t  kOverlayHostOrigin   = 0x390;  // float x, y: the marker host rect's origin (0, 0)
inline constexpr std::uint32_t  kOverlayUiSize       = 0x398;  // float w, h: the UI rect's size (markers read it)
inline constexpr std::uint32_t  kOverlayHostSize     = 0x3A0;  // float w, h: the host size (the window, px)
inline constexpr std::uint32_t  kOverlayHostScale    = 0x3A8;  // float: the host geometry scale
inline constexpr std::uint32_t  kOverlayRenderStamp  = 0x464;  // i32: the frame counter at the last render
inline constexpr std::uint32_t  kOverlayFade         = 0x450;  // 5 floats: alpha, from, to, elapsed, duration
/// EndUISystem[37], the UI filters (RE-hud-families.md §5.9): a count per type at +0x24 (int[6]), a suppressed byte per
/// type at +0x1E. Type 4 fades the whole overlay (to 0 when its count leaves 0, to 1 when it returns; 0x1414BF920,
/// 0x1414BFE30); type 2 (main menu) also counts 3, 4 and 5.
inline constexpr std::uintptr_t kUiFilterSlotGlobal  = 0x1459CAA98;
inline constexpr std::uint32_t  kUiFilterCounts      = 0x24;
inline constexpr std::uint32_t  kUiFilterSuppressed  = 0x1E;

// The overlay render's calls the HUD layout redirects (RE-hud-overlay-pass.md §3.1; the call bytes are checked by
// platform::CallRedirect against these targets).
inline constexpr std::uintptr_t kInGameUIRectCall      = 0x141509E9D;  // call EndUI__GetInGameUIRect: crosses a block
inline constexpr std::uintptr_t kGetInGameUIRect       = 0x141508810;  // FIntRect* (FIntRect* out)
inline constexpr std::uintptr_t kLayerEntryCall        = 0x14150A006;  // call FindOrAddNamedLayerEntry: one block
inline constexpr std::uintptr_t kFindOrAddLayerEntry   = 0x143A015B0;  // entry* (slot*, FName, const FIntRect*)
// FEndMenuLayerEntry: its rect and the row-major matrix FEndMenuLayerEntry__MakeRectMatrix 0x143A07AE0 fills
// (2/W, -2/H, -1, +1 from the rect's size alone; RE-hud-overlay-pass.md §3.3).
inline constexpr std::uint32_t kEntryRect              = 0x08;  // MinX, MinY, MaxX, MaxY (int32)
inline constexpr std::uint32_t kEntryMatrixScaleX      = 0x40;  // m00
inline constexpr std::uint32_t kEntryMatrixScaleY      = 0x54;  // m11
inline constexpr std::uint32_t kEntryMatrixTranslateX  = 0x70;  // m30
inline constexpr std::uint32_t kEntryMatrixTranslateY  = 0x74;  // m31
// The per-child view-projections SEndCanvas's perspective paint pushes into the entry (64-byte row-major matrices,
// built from the entry rect's size alone); a child with one draws with it instead of the entry matrix
// (RE-hud-band-3d.md).
inline constexpr std::uint32_t kEntryChildMatrices     = 0xD0;  // float* (16 per matrix)
inline constexpr std::uint32_t kEntryChildMatrixCount  = 0xD8;  // int32
inline constexpr std::uintptr_t kLayerSubmitCall       = 0x14150A1EC;  // call SubmitLayerSlot: crosses a block
inline constexpr std::uintptr_t kSubmitLayerSlot       = 0x1439FC750;  // (renderer, pool, slot)

/// EndUI__ContainScaleFor1920x1080: float (FVector2D size, packed in rcx); W/1920 below 16:9, else H/1080, at least
/// 0.01. The game's UI scale of a rect, which every marker conversion applies (RE-hud-ui.md §2.4).
inline constexpr std::array<std::uint8_t, 16> kContainScaleEntry{
    0xF3, 0x0F, 0x10, 0x1D, 0xF8, 0xBA, 0xBE, 0x03, 0xF3, 0x0F, 0x10, 0x25, 0x28, 0x83, 0xBE, 0x03};
inline constexpr platform::FunctionFact kContainScale{L"EndUI__ContainScaleFor1920x1080", 0x1413A94A0, kContainScaleEntry};

// The UI image per view, render thread (RE-hud-two-images.md §2.2, §4, §5). The calls are checked by
// platform::CallRedirect; each holds the view in r8 at the call.
inline constexpr std::uintptr_t kUiAllocBindCall         = 0x1421E82E9;  // Render's UI loop: call AllocBind
inline constexpr std::uintptr_t kUiAllocBindClearCall    = 0x1421E826B;  // the clear-only path: call AllocBind
inline constexpr std::uintptr_t kUiAllocBind             = 0x142541670;  // SceneRenderTargets__AllocBindInGameUITarget
inline constexpr std::uintptr_t kPostProcessCall         = 0x1421E8400;  // Render: call FPostProcessing::Process
inline constexpr std::uintptr_t kPostProcessProcess      = 0x14251C230;  // (this, RHICmdList, View, VelocityRT&)
inline constexpr std::uintptr_t kSceneRenderTargetsGlobal = 0x145923AD0; // FSceneRenderTargets (static object)
inline constexpr std::uint32_t  kSceneTargetsInGameUI    = 0x110;        // TRefCountPtr<IPooledRenderTarget>

// Player 2's HUD image (RE-hud-overlay-pass.md §2, §3, §4; RE-hud-two-images.md §2.3, §3).
inline constexpr std::array<std::uint8_t, 16> kOverlayRenderEntry{
    0x4C, 0x8B, 0xDC, 0x55, 0x41, 0x56, 0x49, 0x8D, 0xAB, 0x68, 0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC};
inline constexpr platform::FunctionFact kOverlayRender{L"SEndRootOverlay__RenderToInGameUILayer", 0x141509D00,
                                                       kOverlayRenderEntry};
inline constexpr std::uintptr_t kOverlayDelegateSlot  = 0x144BF4DB0;  // the render delegate's vtable slot (+0x60)
inline constexpr std::uintptr_t kOverlayDelegateThunk = 0x14151BAF0;  // what it holds: render [this+8] if any
inline constexpr std::uintptr_t kOverlayAcquireCall   = 0x141509FCF;  // call AcquireLayerSlot (pool 2): crosses a block
inline constexpr std::uintptr_t kAppDeltaTime         = 0x1453A49B0;  // FApp::DeltaTime (double): the paint's widget ticks
inline constexpr std::uintptr_t kDrawEntryBatchCall   = 0x1439FCFD8;  // DrawLayerEntry: call DrawEntryBatchToTarget
inline constexpr std::uintptr_t kDrawEntryBatchToTarget = 0x1439FD100;  // 9 arguments, no result
inline constexpr std::uintptr_t kGMallocGlobal        = 0x145825680;  // FMalloc* GMalloc (vt+0x10 Malloc(size, align))
inline constexpr std::uintptr_t kMenuVertexBufferVtable = 0x144F766F8;  // FEndMenuRenderer's pool buffers
inline constexpr std::uintptr_t kMenuIndexBufferVtable  = 0x144F76788;

// Battle markers' placements, recorded and replayed per view (RE-hud-battle-markers.md §3.1, §3.3, §7). Each entry's
// first two instructions are copied (both position-independent), so the redirects are installed from the game thread.
inline constexpr std::uintptr_t kBarPlaceUntargeted = 0x1413DBBC0;  // (mgr, widget, const FVector*)
inline constexpr std::array<std::uint8_t, 7> kBarPlaceUntargetedFirst{0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x40, 0x18};
inline constexpr std::uintptr_t kBarPlaceTargeted = 0x1413DC010;  // (mgr, widget, key*, loc*, int, float, u8)
inline constexpr std::array<std::uint8_t, 5> kBarPlaceTargetedFirst{0x48, 0x89, 0x5C, 0x24, 0x10};
inline constexpr std::uintptr_t kBattleTextPlace = 0x141496AD0;  // (out, pc, canvas*, widget*, loc*, offset, clamp, bool*)
inline constexpr std::array<std::uint8_t, 7> kBattleTextPlaceFirst{0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x20};
/// FLocalPlayerContext::GetPlayerController (ctx = widget+0x1E8): the controller the bar placements project with.
inline constexpr std::uintptr_t kPlayerContextController = 0x143016CF0;
inline constexpr std::array<std::uint8_t, 7> kPlayerContextControllerFirst{0x44, 0x8B, 0x41, 0x0C, 0x48, 0x8B, 0xD1};
inline constexpr std::uintptr_t kSetPositionInViewport = 0x1428F3ED0;  // UUserWidget (this, FVector2D, bool)
// Battle text's replay (RE-hud-battle-markers.md §3.3, §10). The tick passes the canvas and the widget as TSharedPtr copies
// on its stack (NotThreadSafe: a plain `inc [ctrl+8]`), so the record pins its own copies and gives them back with the
// game's ~TSharedPtr. The visibility after the placement is the tick's: Slate `vt+0x1A8` with a TAttribute made from one
// of two EVisibility statics, UMG `vt+0x248` (3 shown, 2 hidden).
inline constexpr std::uintptr_t kSharedPtrRelease = 0x140631EF0;  // ~TSharedPtr<_, NotThreadSafe>(this)
inline constexpr std::array<std::uint8_t, 6> kSharedPtrReleaseFirst{0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
inline constexpr std::uintptr_t kVisibilityAttribute = 0x14136D170;  // TAttribute<EVisibility>(this, const EVisibility*)
inline constexpr std::array<std::uint8_t, 5> kVisibilityAttributeFirst{0x0F, 0xB6, 0x02, 0x88, 0x01};
inline constexpr std::uintptr_t kTextVisibilityShown  = 0x1453BE1CC;  // EVisibility static: placed on screen
inline constexpr std::uintptr_t kTextVisibilityHidden = 0x1453B955A;  // EVisibility static: not projected
inline constexpr std::uintptr_t kTextTickUmgReturn    = 0x141497E25;  // the tick's calls of the core: UMG list
inline constexpr std::uintptr_t kTextTickNumberReturn = 0x1414983D5;  // Slate damage numbers
inline constexpr std::uintptr_t kTextTickTextReturn   = 0x1414987E3;  // Slate texts
inline constexpr std::uintptr_t kTextUmgWrapperReturn = 0x141496FEB;  // EndUI_BattleText_PlaceUMG_SetVisibility (spawn)
inline constexpr std::uint32_t  kSlateSetVisibility   = 0x1A8;  // SWidget::SetVisibility(TAttribute) (by value)
inline constexpr std::uint32_t  kWidgetSetVisibility  = 0x248;  // UWidget::SetVisibility(ESlateVisibility)
inline constexpr std::uint32_t  kObjectWidgetUser     = 0x4E8;  // SObjectWidget::WidgetObject (written at 0x1428D8C50)
// The target cursor (slot 18, also Barret's far-shot cursor, slot 54) and the lock-on markers (slot 43): pure placements
// taking the controller as an argument (RE-hud-battle-markers.md §3.4, §3.7), recorded at their entries and replayed.
inline constexpr std::uintptr_t kCursorPlace = 0x1413E7BE0;  // bool (pc, widget, const FVector*)
inline constexpr std::array<std::uint8_t, 5> kCursorPlaceFirst{0x48, 0x89, 0x74, 0x24, 0x18};
inline constexpr std::uintptr_t kLockonPlace = 0x1414604B0;  // void (pc, widget, const FVector*, u8 show)
inline constexpr std::array<std::uint8_t, 5> kLockonPlaceFirst{0x48, 0x89, 0x74, 0x24, 0x18};
// The ability-name labels (slot 45) and weak-point markers (slot 58): their placement is inline in the tick, so the
// tick runs again for each pass with its controller lookups answering that pass's player (§3.5, §3.6). Both take only
// the manager. Weak points run with their facing gate (manager byte +0) at 0: placement only.
inline constexpr std::uintptr_t kEndUISystemSlots    = 0x1459CA970;  // EndUISystem__Get()'s array of managers
inline constexpr std::uintptr_t kLabelsTick          = 0x1413E5C10;  // EndUISlot45_SkillNameLabels_Tick(mgr)
inline constexpr std::array<std::uint8_t, 4> kLabelsTickFirst{0x40, 0x55, 0x41, 0x55};
inline constexpr std::uintptr_t kLabelsContextCall   = 0x1413E5FDF;  // call PlayerContext: crosses a block
inline constexpr std::uintptr_t kLabelsContextCall2  = 0x1413E60A9;  // the bar-glued y's clamp
inline constexpr std::uintptr_t kWeakPointsTick      = 0x1414C0C10;  // EndUISlot58_WeakPointMarkers_Tick(mgr)
inline constexpr std::array<std::uint8_t, 5> kWeakPointsTickFirst{0x48, 0x89, 0x4C, 0x24, 0x08};
inline constexpr std::uintptr_t kWeakPointsContextCall = 0x1414C0EEA;
inline constexpr std::uint32_t  kLabelsSlot = 45, kWeakPointsSlot = 58;
inline constexpr std::uint32_t  kLabelsNumBits = 0x28, kWeakPointsNumBits = 0x30;  // the TSets' element counts

// Player 2's own navi map manager (EndUISystem[29], RE-hud-minimap-two.md §2-§6, §9). Call sites are checked by
// platform::CallRedirect against their targets.
inline constexpr std::uintptr_t kNaviSlotGlobal        = 0x1459CAA58;  // EndUISystem[29]: the game's manager
inline constexpr std::uintptr_t kBannerSlotGlobal      = 0x1459CAA88;  // EndUISystem[35]: the banner fades' lists +0x10, +0x80
inline constexpr std::uintptr_t kNaviManagerCtor       = 0x1414722C0;  // (memory): in place, 0x268 bytes
inline constexpr std::uintptr_t kGameMalloc            = 0x141C32C90;  // FMemory::Malloc(size, alignment)
inline constexpr std::uintptr_t kMulticastAddRaw       = 0x14099A3B0;  // (list, handle out, object, {method, 0})
inline constexpr std::uintptr_t kNaviBannerStart       = 0x141473960;  // __OnBannerStart_FadeOut
inline constexpr std::uintptr_t kNaviBannerEnd         = 0x141473980;  // __OnBannerEnd_FadeIn
inline constexpr std::uintptr_t kNaviTick              = 0x141477150;  // (manager, float dt)
inline constexpr std::uintptr_t kNaviTickCall          = 0x1413AE3A0;  // in EndUISystem__TickHUDWithDelta
inline constexpr std::uintptr_t kNaviSetupMap          = 0x14147B780;  // (manager, map data)
inline constexpr std::array<std::uintptr_t, 2> kNaviSetupCalls{0x141685898, 0x141685C82};
inline constexpr std::uintptr_t kNaviFieldStart        = 0x14147B0C0;
inline constexpr std::uintptr_t kNaviFieldStartCall    = 0x1413AD412;
inline constexpr std::uintptr_t kNaviFieldEnd          = 0x14147B450;  // releases the map pieces
inline constexpr std::uintptr_t kNaviFieldEndCall      = 0x140FF544A;
inline constexpr std::uintptr_t kNaviReset             = 0x1414728E0;  // removes the widgets
inline constexpr std::uintptr_t kNaviResetCall         = 0x1413ADCE1;
inline constexpr std::uintptr_t kNaviAddIcon           = 0x141479D90;  // (manager, key, a3, a4, a5, a6)
inline constexpr std::array<std::uintptr_t, 4> kNaviAddIconCalls{0x140FF7A40, 0x1410D4CCA, 0x1410D4EB6, 0x1411620B8};
inline constexpr std::uintptr_t kNaviRemoveIcon        = 0x14147A830;  // (manager, key)
// The last crosses a block: installed from the game thread.
inline constexpr std::array<std::uintptr_t, 5> kNaviRemoveIconCalls{0x140FF5115, 0x1410D4071, 0x1410D4AA7, 0x1411623D7,
                                                                    0x140FF7A7F};
inline constexpr std::uintptr_t kNaviSetHidden         = 0x141479BC0;  // +0x180 (manager, value)
inline constexpr std::uintptr_t kNaviToggleVisible     = 0x141473750;  // +0x181 (manager)
inline constexpr std::uintptr_t kNaviSetEnabled        = 0x141473810;  // +0x182 (manager, value)
inline constexpr std::uintptr_t kNaviSetNaviMode       = 0x14147AF40;  // +0x258 (manager, value)
inline constexpr std::uintptr_t kNaviSetCompassMode    = 0x14147B000;  // +0x259 (manager, value)
inline constexpr std::uintptr_t kGetFirstPlayerController = 0x143370520;  // UWorld::GetFirstPlayerController
inline constexpr std::uintptr_t kNaviCompassFirstPcCall = 0x1413EFDE9;  // in the compass's tick
inline constexpr std::uintptr_t kAddToRootOverlay      = 0x1413491B0;  // UEndUserWidget (widget, int ZOrder)
inline constexpr std::uintptr_t kNaviCreatorAddCall    = 0x141473F36;  // in the NaviMap_C creator
inline constexpr std::uintptr_t kNaviFindOctagon       = 0x141472C70;  // (user widget): the Nav_Map octagon
inline constexpr std::uintptr_t kNaviEntryCall         = 0x1414FDDE0;  // the octagon paint's FindOrAdd (pool 0)
inline constexpr std::uintptr_t kNaviSubmitCall        = 0x1414FDFBB;  // the octagon paint's submit (pool 0)
inline constexpr std::uintptr_t kSetColorAndOpacity    = 0x1428F1C20;  // UUserWidget (widget, const FLinearColor&)
inline constexpr std::uintptr_t kCharacterByPlayerType = 0x1413B4C40;  // (u8 type): the party's character of that type
inline constexpr std::uint32_t  kNaviManagerRoot       = 0x08;   // weak NaviMap_C
inline constexpr std::uint32_t  kNaviManagerOctagon    = 0x18;   // weak Nav_Map (UEndNaviMapOctagon)
inline constexpr std::uint32_t  kNaviManagerFlags      = 0x180;  // hidden, visible, enabled, auto-rotate; +0x188 zoom
inline constexpr std::uint32_t  kNaviManagerMapData    = 0x1B4;  // the map data (a pointer at a 4-aligned offset)
inline constexpr std::uint32_t  kNaviManagerModes      = 0x258;  // navi-map mode, compass mode, pending
inline constexpr std::uint32_t  kNaviManagerCompass    = 0x260;  // the compass object (+0: weak Compass_C)
inline constexpr std::uint32_t  kOctagonRenderTexture  = 0x118;  // UEndNaviMapOctagon::RenderTexture
inline constexpr std::uint32_t  kOctagonSlate          = 0x128;  // its Slate object
inline constexpr std::uint32_t  kOctagonSlateTarget    = 0x5C8;  // the Slate object's render target (its entries draw into it)
inline constexpr std::uint32_t  kOctagonPaintInterface = 0x330;  // the Slate object's paint interface (rsi in the paint)
inline constexpr std::uint32_t  kPartyPrimaryController = 0x00;  // AEndPartyManager: the primary controller
inline constexpr std::uint32_t  kPartySlotTypes        = 0x230;  // u8[3]: the deployed members' player types by ordinal
inline constexpr std::uint32_t  kPartyLeaderType       = 0x233;  // the leader's player type byte
inline constexpr std::uint8_t   kPlayerTypeNone        = 9;      // EPlayerType::NONE (an empty row, no member)

// ULocalPlayer's split-screen view as fractions of the viewport (stock 4.18, not reflected: between ViewportClient
// +0x58 and LastViewLocation, before AspectRatioAxisConstraint +0x7C).
inline constexpr std::uint32_t kLocalPlayerOrigin      = 0x60;  // FVector2D
inline constexpr std::uint32_t kLocalPlayerSize        = 0x68;  // FVector2D

// UMG, stock 4.18 (UMG.hpp) and UEndUserWidget's own fields (EndGame.hpp, UEndUserWidget__AddToRootOverlay).
inline constexpr std::uint32_t kWidgetVisibility       = 0xB7;   // UWidget::Visibility (ESlateVisibility)
inline constexpr std::uint32_t kWidgetGcWidget         = 0xD8;   // UWidget::MyGCWidget (TWeakPtr: object, then controller)
inline constexpr std::uint32_t kSlateColorAndOpacity   = 0x410;  // SObjectWidget's ColorAndOpacity attribute (the tint
                                                                 // UUserWidget::SetColorAndOpacity writes; +0x420 is-set)
inline constexpr std::uint32_t kUserWidgetColor        = 0x100;  // UUserWidget::ColorAndOpacity (FLinearColor)
inline constexpr std::uint32_t kUserWidgetAnchors      = 0x1B0;  // viewport anchors (4 floats)
inline constexpr std::uint32_t kUserWidgetOffset       = 0x1C0;  // viewport position (2 floats), size (+0x1C8)
inline constexpr std::uint32_t kUserWidgetAlignment    = 0x1D0;  // viewport alignment (2 floats)
inline constexpr std::uint32_t kUserWidgetPlayerWeak   = 0x1E8;  // PlayerContext: weak ULocalPlayer (index, serial)
inline constexpr std::uint32_t kUserWidgetViewportOffsets = 0x1C0;  // ViewportOffsets.Left/Top (what the placements write)
inline constexpr std::uint32_t kEndWidgetRootCanvas    = 0x230;  // UEndUserWidget: its SEndCanvas while a root
inline constexpr std::uint32_t kEndWidgetZOrder        = 0x248;  // UEndUserWidget: the ZOrder it was added with

// --- The game's single slots a further player gets for one call (players/scopes.h) --------------------------------
// The party manager's leader (RE-party-possession.md §3), the input object's focus context (RE-command-menu-two.md
// §2), the UI filters' latches (§4) and the UI system's command controller slot (RE-battle.md §2.1).
inline constexpr std::uint32_t kPartyLeaderWeak       = 0x238;  // AEndPartyManager: the leader's weak pair {index, serial}
inline constexpr std::uint32_t kFocusPath             = 0x08;   // SquareInput owner: FWeakWidgetPath (32 bytes)
inline constexpr std::uint32_t kFocusPathBytes        = 0x20;
inline constexpr std::uint32_t kFocusLayers           = 0x40;   // TArray of 40-byte layers (16 bytes)
inline constexpr std::uint32_t kFocusLayersBytes      = 0x10;
// A layer is 40 bytes: weak layer, weak saved focus (+0x10), priority (+0x20); the command menu's own is -1000.
inline constexpr std::uint32_t kFocusCause            = 0x6C;   // 1 byte
inline constexpr std::uint32_t kFocusCaptureMap       = 0x98;   // TSet of 48-byte elements (80 bytes)
inline constexpr std::uint32_t kFocusCaptureMapBytes  = 0x50;
inline constexpr std::uint32_t kFocusCaptureFirstFree = 0x30;   // FirstFreeIndex inside the map: -1 when empty
inline constexpr std::uint32_t kFocusMenuDelegate     = 0x190;  // the menu-core delegate (64 bytes; all zero = unbound)
inline constexpr std::uint32_t kFocusMenuDelegateBytes = 0x40;
inline constexpr std::uint32_t kUiFilterLatches       = 0x18;   // EndUISystem[37]: the wait-tint (+0x18) and Dying (+0x19) latch bytes
inline constexpr std::uintptr_t kCommandCtrlSlotGlobal = 0x1459CAA08;  // EndUISystem[19]: the field command controller

// --- A second field command controller (RE-battle.md §2, RE-command-menu-two.md) ----------------------------------
// FieldCommandCtrl: 0x5D0 bytes from GMalloc, the ctor, then SetupMenuWidgets(ctrl, world) builds its own two widgets
// (Command_C and the party panel Status_C), pushes its menu as a focus layer and binds its handlers. Its tick (one call
// site, in the field systems' tick) polls its own widget's key states into requests and drives the leader's attacks,
// guard, dodge and commands. The widget's key states are advanced in the widget's NativeTick, once per overlay paint.
inline constexpr std::uint32_t kCommandCtrlSize          = 0x5D0;
/// The length of the one home-area store that starts the entry-redirected controller functions (their entry arrays
/// below are the 16-byte checks; an entry redirect copies the instruction only).
inline constexpr std::size_t   kHomeStoreBytes           = 5;
inline constexpr std::uint32_t kCommandCtrlMemberCount   = 0x10;   // i32: the controller's member-list count
inline constexpr std::uint32_t kCommandCtrlLeaderOrdinal = 0x28;   // i32: the leader's party ordinal the tick caches; the
                                                                   // poll reads no input while that member is KO'd
inline constexpr std::uint32_t kCommandCtrlCommandLists  = 0x38;   // the command-list sub-object (ctrl+0x38)
inline constexpr std::uint32_t kCommandCtrlPartyList     = 0x1D0;  // the party-list sub-object
inline constexpr std::uint32_t kCommandCtrlFieldMenuWeak = 0x2F8;  // Status_C (the party panel) weak pair
inline constexpr std::uint32_t kCommandCtrlMenuWeak      = 0x300;  // Command_C weak pair
inline constexpr std::uint32_t kCommandCtrlBattleHudMode = 0x348;  // u8: 1 in battle
inline constexpr std::uint32_t kCommandCtrlMenuAllowed   = 0x3D5;  // u8, written directly by events on the game's controller
inline constexpr std::uint32_t kCommandCtrlWaitState     = 0x418;  // u8: the menu's wait (time slow, tint)
inline constexpr std::uint32_t kCommandCtrlInputLive     = 0x420;  // u8, written on the game's controller
inline constexpr std::uint32_t kCommandCtrlSetupPending  = 0x541;  // u8: the tick re-runs Setup
inline constexpr std::uint32_t kCommandCtrlSetUp         = 0x543;  // u8: Setup ran (cleared by ResetAll)
inline constexpr std::uint32_t kCommandCtrlLayerPushed   = 0x544;  // u8: the menu's focus layer is up
inline constexpr std::uintptr_t kCommandMenuVTable       = 0x144BCF730;  // UEndFieldCommandMenu
inline constexpr std::uint32_t  kCommandMenuNativeTickSlot = 90;         // UUserWidget::NativeTick(geometry, dt)

inline constexpr std::array<std::uint8_t, 16> kCommandCtrlCtorEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x4C};
inline constexpr std::array<std::uint8_t, 16> kCommandCtrlSetupEntry{
    0x40, 0x55, 0x56, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00};
inline constexpr std::array<std::uint8_t, 16> kCommandCtrlTickEntry{
    0x40, 0x55, 0x53, 0x41, 0x54, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00};
inline constexpr std::array<std::uint8_t, 16> kCommandCtrlResetAllEntry{
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x50, 0xC6, 0x81, 0x41, 0x05, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF9};
inline constexpr std::array<std::uint8_t, 16> kCommandCtrlSetHudModeEntry{
    0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x0F, 0xB6, 0xF2, 0x48, 0x8B, 0xF9};
inline constexpr std::array<std::uint8_t, 16> kCommandCtrlRefreshEntry{
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x8B, 0x81, 0xFC, 0x02, 0x00, 0x00, 0x48, 0x8B, 0xF9};
inline constexpr std::array<std::uint8_t, 16> kCommandListsRebuildEntry{
    0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x55, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83, 0xEC};
inline constexpr std::array<std::uint8_t, 16> kCommandCtrlSetMenuAllowedEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x8B, 0x91};
inline constexpr std::array<std::uint8_t, 16> kCommandCtrlSetInputLiveEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x88, 0x91, 0x20, 0x04, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0x84};
inline constexpr std::array<std::uint8_t, 16> kCommandMenuNativeTickEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55};
inline constexpr std::array<std::uint8_t, 16> kPartyListRebuildEntry{
    0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x33, 0xFF, 0x48, 0x8D, 0x59, 0x30, 0x89, 0x7B};
inline constexpr std::array<std::uint8_t, 16> kSetGameSpeedSlotEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x05, 0xFF, 0xAA, 0xE4};

inline constexpr platform::FunctionFact kCommandCtrlCtor{L"FieldCommandCtrl__Ctor", 0x141412310, kCommandCtrlCtorEntry};
/// SetupMenuWidgets(ctrl, UWorld*): creates the widgets once, pushes the menu's focus layer, binds the handlers.
inline constexpr platform::FunctionFact kCommandCtrlSetup{L"FieldCommandCtrl__SetupMenuWidgets", 0x1414130F0,
                                                          kCommandCtrlSetupEntry};
/// Tick(ctrl, float dt): one call site, in EndGame_TickFieldSystems_CallsFieldStepTick; the call crosses a block.
inline constexpr platform::FunctionFact kCommandCtrlTick{L"FieldCommandCtrl__Tick", 0x141418010, kCommandCtrlTickEntry};
inline constexpr std::uintptr_t         kCommandCtrlTickCall = 0x1416BF38C;
/// ResetAll(ctrl): clears the set-up byte, pops its focus layer, collapses the panel, writes speed slot 2 = 1.
inline constexpr platform::FunctionFact kCommandCtrlResetAll{L"FieldCommandCtrl_ResetAll", 0x141414E50,
                                                             kCommandCtrlResetAllEntry};
inline constexpr std::array<std::uintptr_t, 3> kCommandCtrlResetAllCalls{0x140FF4C86, 0x14113FC67, 0x141B273E8};
inline constexpr std::uintptr_t kCommandCtrlSetupCall     = 0x14113FE01;  // on/off 0/42 off
inline constexpr std::uintptr_t kCommandCtrlSetupTailJump = 0x1413B0552;  // EndUISystem_SetupMenusAndCmdCtrl's tail (on/off 68, state exits, a thunk)
/// SetBattleHudMode348(ctrl, u8 mode): the battle HUD mode (on/off 68); its entry is one home-area store.
inline constexpr platform::FunctionFact kCommandCtrlSetHudMode{L"FieldCommandCtrl_SetBattleHudMode348", 0x141415420,
                                                               kCommandCtrlSetHudModeEntry};
/// RefreshFieldMenu(ctrl): the party panel rebuilt from the party (15 callers); its first step has one call site.
inline constexpr platform::FunctionFact kCommandCtrlRefresh{L"FieldCommandCtrl_RefreshFieldMenu", 0x14141EE80,
                                                            kCommandCtrlRefreshEntry};
inline constexpr platform::FunctionFact kPartyListRebuild{L"PartyList_RebuildStep", 0x1414AC1A0, kPartyListRebuildEntry};
inline constexpr std::uintptr_t         kPartyListRebuildCall = 0x14141EEE3;
/// FieldCmdSub38_RebuildMemberCommandLists(ctrl+0x38, a2): the command lists after equipment, materia and party changes.
inline constexpr platform::FunctionFact kCommandListsRebuild{L"FieldCmdSub38_RebuildMemberCommandLists", 0x1413C4A80,
                                                             kCommandListsRebuildEntry};
/// SetMenuAllowed3D5(ctrl, u8 on, u8 keep, u8 a4): six callers; about 25 others write the byte directly.
inline constexpr platform::FunctionFact kCommandCtrlSetMenuAllowed{L"FieldCommandCtrl_SetMenuAllowed3D5", 0x14141F4F0,
                                                                   kCommandCtrlSetMenuAllowedEntry};
/// SetInputLive420(ctrl, u8 live, a3): off ends the wait (sound, tint latch, slot 2 = 1); on stores the byte.
inline constexpr platform::FunctionFact kCommandCtrlSetInputLive{L"FieldCommandCtrl_SetInputLive420", 0x141419990,
                                                                 kCommandCtrlSetInputLiveEntry};
/// UEndFieldCommandMenu::NativeTick (slot 90): advances the widget's key states (pressed/held/released) and runs
/// its Blueprint graph; it runs once per overlay paint.
inline constexpr platform::SlotFact kCommandMenuNativeTick{
    L"UEndFieldCommandMenu::NativeTick (slot 90)", kCommandMenuVTable + kCommandMenuNativeTickSlot * 8,
    {L"UEndFieldCommandMenu_AdvanceKeyStateMaps", 0x14121A430, kCommandMenuNativeTickEntry}};
/// GameState_SetGameSpeedSlot(u8 slot, float value): the tick's one write of slot 2 (the menu's time slow).
inline constexpr platform::FunctionFact kSetGameSpeedSlot{L"GameState_SetGameSpeedSlot", 0x14099EFA0, kSetGameSpeedSlotEntry};
inline constexpr std::uintptr_t         kCommandCtrlSlowCall = 0x141418D03;

// --- The battle system and the leader's input buffer (RE-battle.md, RE-battle-member-two.md §2.6) -------------------
inline constexpr std::uintptr_t kBattleSystemGlobal       = 0x1457E9720;  // the battle system (`sys`), null outside battle
inline constexpr std::uint32_t  kGameStatePauseCounter    = 0x4F0;        // AEndGameState: the pause-like counter (dt 0 while set)
inline constexpr std::uint32_t  kCharacterBattleComponent = 0xCB0;        // AEndCharacter: the battle player component (`comp`)
inline constexpr std::uint32_t  kBattleCompInputBufferSlot = 0xE0;        // comp vtable: the leader-only input buffer (comp, dt)

// --- Targeting for player 2 (RE-battle-member-two.md §5, RE-battle.md §2.9) ---------------------------------------
// The battle system's target fields: the lock (+0x388), the displayed and lock-camera targets, the target-list mode,
// the stick-switch latch, the dummy-lock option, latch and timer, all in +0x370..+0x3E8. The targeting tick is called
// twice per battle tick, each after the leader's soft-target pre-tick; the second call crosses a block.
inline constexpr std::uint32_t kBattleSysTargetFields      = 0x370;
inline constexpr std::uint32_t kBattleSysTargetFieldsBytes = 0x78;
inline constexpr std::uint32_t kBattleSysLock              = 0x388;
inline constexpr std::uint32_t kBattleSysDummyLatch        = 0x3E3;  // the lock setter derives it from +0x3E2 and the lock
inline constexpr std::array<std::uint8_t, 16> kBattleTargetPreTickEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B, 0xF9, 0xE8, 0xAE, 0x71};
inline constexpr std::array<std::uint8_t, 16> kBattleTickTargetingEntry{
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x0F, 0x29, 0x74, 0x24, 0x40, 0x48, 0x8B, 0xF9, 0x0F, 0x28};
inline constexpr std::array<std::uint8_t, 16> kBattleSetLockOnEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x0F, 0xB6, 0x81, 0x88, 0x03, 0x00, 0x00, 0x48, 0x8B, 0xD9};
inline constexpr std::array<std::uint8_t, 16> kBattleClearLockAndTargetEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x0F, 0xB6, 0x81, 0x88, 0x03, 0x00, 0x00, 0x48, 0x8B, 0xD9};
/// BattleTarget_PreTick_LeaderSoftTarget(sys): the leader's soft target (its battle component's target) each tick.
inline constexpr platform::FunctionFact kBattleTargetPreTick{L"BattleTarget_PreTick_LeaderSoftTarget", 0x1409C0900,
                                                             kBattleTargetPreTickEntry};
/// BattleSystem_TickTargeting(sys, float dt): the lock toggle, the dummy-lock camera, the target-list mode, the
/// locked follow or the stick switch, the displayed target.
inline constexpr platform::FunctionFact kBattleTickTargeting{L"BattleSystem_TickTargeting", 0x1409C21F0,
                                                             kBattleTickTargetingEntry};
/// BattleSystem_SetLockOn388(sys, u8 on, a3): the lock; on change posts the named parameter Global.LockOn.
inline constexpr platform::FunctionFact kBattleSetLockOn{L"BattleSystem_SetLockOn388", 0x1409BD6E0, kBattleSetLockOnEntry};
/// BattleTarget_ClearLockAndTarget(sys, a2, a3): the lock off (posting the parameter) and the displayed target null.
inline constexpr platform::FunctionFact kBattleClearLockAndTarget{L"BattleTarget_ClearLockAndTarget", 0x1409BD770,
                                                                  kBattleClearLockAndTargetEntry};
inline constexpr std::uintptr_t kTargetingCallPaused = 0x1409B7009;  // inside a block
inline constexpr std::uintptr_t kTargetingCallNormal = 0x1409B793F;  // crosses a block

// --- The player's rates for player 2's member (RE-battle-member-two.md §2) -----------------------------------------
// The eight character battle components share one vtable layout: +0x318 the passive ATB tick (its rate getter +0x310
// and the guard coefficient test the leader inside it), +0x748 the "ability by the player" flag setter. The enemy's
// stagger-gauge hit takes the attacker as an argument and has one call site.
inline constexpr std::array<std::uintptr_t, 8> kBattleCompVTables{
    0x144B65C48, 0x144B665C8, 0x144B66F48, 0x144B678C8, 0x144B68248, 0x144B68BC8, 0x144B69548, 0x144B69EC8};
inline constexpr std::uint32_t kBattleCompPassiveAtbSlot = 0x318 / 8;
inline constexpr std::uint32_t kBattleCompUcpcFlagSlot   = 0x748 / 8;
inline constexpr std::array<std::uint8_t, 16> kBattleCompTickPassiveAtbEntry{
    0x41, 0x56, 0x48, 0x83, 0xEC, 0x70, 0x80, 0xB9, 0x11, 0x06, 0x00, 0x00, 0x0D, 0x4C, 0x8B, 0xF1};
inline constexpr std::array<std::uint8_t, 16> kBattleCompSetUcpcFlagEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0xF3, 0x0F, 0x10, 0x42, 0x2C, 0x48};
inline constexpr std::array<std::uint8_t, 16> kBattleStaggerRewardsEntry{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56};
/// BattlePlayerComp_TickPassiveATB(comp, float dt): the passive ATB fill, at the AI-ally coefficient for a non-leader.
inline constexpr platform::FunctionFact kBattleCompTickPassiveAtb{L"BattlePlayerComp_TickPassiveATB", 0x140986CF0,
                                                                  kBattleCompTickPassiveAtbEntry};
/// BattlePlayerComp_Vt748_SetUCPCAbilityFlag(comp, a2, a3): bit 29 of comp+0x168 for the leader's abilities.
inline constexpr platform::FunctionFact kBattleCompSetUcpcFlag{L"BattlePlayerComp_Vt748_SetUCPCAbilityFlag", 0x1408A3950,
                                                               kBattleCompSetUcpcFlagEntry};
/// BattleBurst_OnHit_AddBP_StaggerRewards(burst, attackerChar, hit): the stagger gauge (half for a non-leader's
/// normal attacks) and the stagger rewards (the leader's or an ability's).
inline constexpr platform::FunctionFact kBattleStaggerRewards{L"BattleBurst_OnHit_AddBP_StaggerRewards", 0x140966EE0,
                                                              kBattleStaggerRewardsEntry};
inline constexpr std::uintptr_t kBattleStaggerRewardsCall = 0x140892DB8;  // its one call site

// --- The lock camera (RE-lock-camera-two.md §1.4) --------------------------------------------------------------
// The operator tick's Lock-state call of BattleCamRequests_FindActiveRotation(reqMgr, &rot, owner, view, params, 0):
// the battle camera requests, keyed on the party leader, that re-aim the lock camera.
inline constexpr std::array<std::uint8_t, 16> kBattleCamFindActiveRotationEntry{
    0x48, 0x89, 0x54, 0x24, 0x10, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x48};
inline constexpr platform::FunctionFact kBattleCamFindActiveRotation{L"BattleCamRequests_FindActiveRotation", 0x140A15A80,
                                                                      kBattleCamFindActiveRotationEntry};
inline constexpr std::uintptr_t kBattleCamFindActiveRotationLockCall = 0x140FDE18C;  // E8 EF 78 A3 FF; crosses a block

// --- The lock camera's edges outside the operator's tick (RE-lock-camera-ledger.md) -------------------------
// A character's camera-override delegate (a TFunction at `chara+0x20F0`; the handler pointer at +0). The game's one
// handler resolves the root operator's NotifyOverride flavor (row X3).
inline constexpr std::uint32_t kCharaCameraOverrideDelegate = 0x20F0;
inline constexpr std::array<std::uint8_t, 16> kCharaCameraOverrideHandlerEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40, 0xB1, 0x15, 0x4D, 0x8B, 0xD1, 0x4D};
inline constexpr platform::FunctionFact kCharaCameraOverrideHandler{
    L"Chara_CameraOverrideDelegate_ToGlobalFieldCamNotifyOverride", 0x141011C30, kCharaCameraOverrideHandlerEntry};

/// AEndPlayerCameraManager::UpdateCamera (slot 198, void(this, float)): the leader occlusion fade and the slot
/// blends inside read the party leader and the primary controller (row X7).
inline constexpr std::array<std::uint8_t, 16> kCameraManagerUpdateCameraEntry{
    0x4C, 0x8B, 0xDC, 0x55, 0x41, 0x57, 0x49, 0x8D, 0xAB, 0x78, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC};
inline constexpr platform::SlotFact kCameraManagerUpdateCamera{
    L"AEndPlayerCameraManager::UpdateCamera (slot 198)", kEndCameraManagerVTable + 198 * 8,
    {L"AEndPlayerCameraManager::UpdateCamera", 0x141687090, kCameraManagerUpdateCameraEntry}};

// Lock re-targets from outside both players' contexts (row X9): an enemy's transform (the AI controller's
// TransformCharacter exec) moves the displayed target to the new form; a script locks the leader on the nearest
// enemy (the battle API's SetDisableTarget exec). Both call sites sit inside one 16-byte block.
inline constexpr std::array<std::uint8_t, 16> kTransformRetargetEntry{
    0x40, 0x55, 0x53, 0x56, 0x48, 0x8D, 0xAC, 0x24, 0x40, 0xEC, 0xFF, 0xFF, 0xB8, 0xC0, 0x14, 0x00};
inline constexpr platform::FunctionFact kTransformRetarget{L"BattleAI_TransformCharacter_RetargetDisplayed390", 0x140940950,
                                                           kTransformRetargetEntry};
inline constexpr std::uintptr_t kTransformRetargetCall = 0x1417A2AF7;  // E8 54 DE 19 FF

// The field camera operator's battle mode (RE-operator-settings-two.md §2.3): one function enters it on the root
// operator (the Battle setting pushed, SpaceOffset off, TargetLength and TargetOffset restarted, the battle scales and
// pitch limits selected), called from the encounter manager's state change, the field on/off table's battle-on branch
// and a field step; the battle-off branch of the on/off table leaves it inline, starting with the Battle request's
// pop. Each call site sits inside one 16-byte block.
inline constexpr std::array<std::uint8_t, 16> kFieldCameraEnterBattleModeEntry{
    0x48, 0x83, 0xEC, 0x68, 0x48, 0x8B, 0x05, 0x25, 0xBA, 0x81, 0x04, 0x8B, 0x90, 0xB4, 0x09, 0x00};
inline constexpr platform::FunctionFact kFieldCameraEnterBattleMode{L"FieldCamera_EnterBattleMode", 0x140FCDFA0,
                                                                    kFieldCameraEnterBattleModeEntry};
inline constexpr std::array<std::uintptr_t, 3> kFieldCameraEnterBattleModeCalls{
    0x140FB0101,   // EndEncountManager_OnStateChange (state 2)
    0x141140947,   // EndFieldOnOffTable_ApplyChange, battle on
    0x140F98DE7};  // the field step (sub_140F98B60)
inline constexpr std::array<std::uint8_t, 16> kCameraSettingPopRequestEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x8B, 0x52, 0x28, 0x48, 0x8B, 0xDA, 0x41, 0x83, 0xEA};
inline constexpr platform::FunctionFact kCameraSettingPopRequest{L"FEndCameraSetting::PopRequest", 0x140FE0A30,
                                                                 kCameraSettingPopRequestEntry};
inline constexpr std::uintptr_t kCameraSettingPopRequestBattleEndCall = 0x141140A85;  // E8 A6 FF E9 FF, the battle-off block's pop
inline constexpr std::array<std::uint8_t, 16> kOperatorSetStateFreeEntry{
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x80, 0xB9, 0x59, 0x06, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF9};
inline constexpr platform::FunctionFact kOperatorSetStateFree{L"AEndFieldCameraOperator::SetStateFree", 0x140FE0AE0,
                                                              kOperatorSetStateFreeEntry};
inline constexpr std::array<std::uint8_t, 16> kOperatorResetBattleMoveFollowEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x1D, 0x1F, 0xAF, 0x86};
inline constexpr platform::FunctionFact kOperatorResetBattleMoveFollow{L"AEndFieldCameraOperator::ResetBattleMoveFollowElapsed",
                                                                       0x140FE16A0, kOperatorResetBattleMoveFollowEntry};
inline constexpr std::uint32_t kOperatorBattleMode          = 0x5D8;  // i32: 0 field, 1 battle
inline constexpr std::uint32_t kOperatorSettingBattle       = 0x600;  // FEndCameraSetting* "Battle" (the five start at +0x5E0)
inline constexpr std::uint32_t kOperatorSettingRequests     = 0x28;   // a setting's request count (+0x30 its active byte)
inline constexpr std::uint32_t kOperatorFlavors             = 0x6C0;  // 29 flavor objects, 8 bytes each, by type
inline constexpr std::uint32_t kFlavorActive                = 0x38;   // a flavor object's active byte
inline constexpr std::uint32_t kFlavorMoveFollow            = 2;
inline constexpr std::uint32_t kFlavorSpaceOffset           = 3;
inline constexpr std::uint32_t kFlavorTargetLength          = 4;
inline constexpr std::uint32_t kFlavorTargetOffset          = 5;
inline constexpr std::uint32_t kOperatorSpaceOffsetByRow    = 0x7A8;  // u8: the SpaceOffset flavor follows the active setting row
inline constexpr std::uint32_t kOperatorDistanceOptionPtr   = 0x8C0;  // u8*: the active camera-distance option byte
inline constexpr std::uint32_t kOperatorFieldDistanceOption = 0x8BA;  // u8 (+0x8BB the battle one)
inline constexpr std::uint32_t kOperatorActiveScales        = 0x8F8;  // 3 float*: the active FOV/length/height scales
inline constexpr std::uint32_t kOperatorFieldScales         = 0x8C8;  // 3 float*: CameraLengthFieldScale* (+0x8E0 the battle set)
inline constexpr std::uint32_t kOperatorActivePitchLimits   = 0x930;  // 2 float*: the active pitch limits
inline constexpr std::uint32_t kOperatorFieldPitchLimits    = 0x910;  // 2 float*: CameraDefaultPitchLimitMin/Max (+0x920 the battle pair)
inline constexpr std::uint32_t kOptionSpaceOffsetInField    = 0xB69BF;  // u8 in the data singleton: SpaceOffset follows the row in field mode

// The controller's nav-area component (RE-operator-settings-two.md §2.5): its tick diffs its controller's own
// leader's nav areas and pushes or pops the InDoor/Town camera settings on the root operator, writing the leader
// actor's and the primary controller's bytes too. The controller's component sits at +0x6F0, its controller at +0xF8.
inline constexpr std::uintptr_t kNavAreaComponentVTable = 0x144C15DD8;  // EndPlayerExistNavAreaComponent
inline constexpr std::array<std::uint8_t, 16> kNavAreaComponentTickEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x0F, 0x29, 0x74, 0x24, 0x20, 0x48, 0x8B, 0xD9, 0x0F, 0x28};
inline constexpr platform::SlotFact kNavAreaComponentTick{
    L"EndPlayerExistNavAreaComponent::TickComponent (slot 102)", kNavAreaComponentVTable + 102 * 8,
    {L"EndPlayerExistNavAreaComponent::TickComponent", 0x1415F9F80, kNavAreaComponentTickEntry}};
inline constexpr std::uint32_t kNavAreaComponentController = 0xF8;  // the owning End player controller, set at BeginPlay
inline constexpr std::array<std::uint8_t, 16> kScriptLockNearestEntry{
    0x48, 0x85, 0xC9, 0x0F, 0x84, 0xF9, 0x03, 0x00, 0x00, 0x57, 0x41, 0x56, 0x48, 0x81, 0xEC, 0xC8};
inline constexpr platform::FunctionFact kScriptLockNearest{L"BattleTarget_ScriptLockNearest_FromExec", 0x14095C8B0,
                                                           kScriptLockNearestEntry};
inline constexpr std::uintptr_t kScriptLockNearestCall = 0x141873C31;  // E8 7A 8C 0E FF

// The command menu's target-list end restores the lock and asks the Command camera flavor (row X5): one function in
// two command-menu state vtables, a second variant in a third slot. Which context runs them for player 2 is read first.
inline constexpr std::array<std::uint8_t, 16> kCmdMenuRestoreLockEntryA{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8, 0x42, 0xE8, 0xF8, 0xFF, 0x84, 0xC0};
inline constexpr std::array<std::uint8_t, 16> kCmdMenuRestoreLockEntryB{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8, 0x52, 0xE6, 0xF8, 0xFF, 0x84, 0xC0};
inline constexpr platform::FunctionFact kCmdMenuRestoreLockA{L"CmdMenuState_EndTargetList_RestoreLock_Cam13", 0x1414266A0,
                                                             kCmdMenuRestoreLockEntryA};
inline constexpr platform::FunctionFact kCmdMenuRestoreLockB{L"CmdMenuState_EndTargetList_RestoreLock_Cam13_B", 0x141426890,
                                                             kCmdMenuRestoreLockEntryB};
inline constexpr std::array<platform::SlotFact, 3> kCmdMenuRestoreLockSlots{{
    {L"command-menu state vtable 0x144BF1D28: restore lock", 0x144BF1D28, kCmdMenuRestoreLockA},
    {L"command-menu state vtable 0x144BF1E78: restore lock", 0x144BF1E78, kCmdMenuRestoreLockA},
    {L"command-menu state vtable 0x144BF1E80: restore lock (B)", 0x144BF1E80, kCmdMenuRestoreLockB}}};

// --- Enemies and a downed leader (RE-enemy-leader-reads.md §2, §5.1) ---------------------------------------
inline constexpr std::uint32_t kBattleCompActionState            = 0x611;   // the battle component's action state byte
inline constexpr std::uint8_t  kBattleCompStateKnockedOut        = 13;      // the KO (dead) state
inline constexpr std::uint32_t kCharacterBattleAI                = 0xCA8;   // AEndCharacter: its battle AI controller
inline constexpr std::uint32_t kBattleAITargetSelectType         = 0x15D0;  // BattleAICharaSpec.TargetSelectType on the controller
inline constexpr std::uint8_t  kBattleAITargetSelectFollowsLeader = 10;     // re-targeted to the new leader on a forced switch
inline constexpr std::uint32_t kEndGameRootCharaOwner            = 0x170;   // the field root's object holding the registry
inline constexpr std::uint32_t kCharaRegistry                    = 0xCE98;  // the character registry inside it

inline constexpr std::array<std::uint8_t, 16> kGetPartyLeaderEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x0F, 0xB6, 0xCA, 0x84, 0xD2, 0x74, 0x65};
inline constexpr platform::FunctionFact kGetPartyLeader{L"PartyManager_GetPartyLeader", 0x1415F1CE0, kGetPartyLeaderEntry};
/// AEndBattleAIController_TickBattleAI's leader read (E8 06 4E CD 00, inside its block): the remembered leader
/// against the party leader, then the Blueprint OnChangeLeader.
inline constexpr std::uintptr_t kBattleAITickLeaderCall = 0x14091CED5;
/// The Blueprint natives enemy scripts pick "the player" with, each inside its block (RE-enemy-leader-reads.md
/// R4-R6): SetTargetByType(UCPC) and the target-type collector (GetActionCharacter and kin) call
/// PartyManager_GetPlayerActor(PM, 0, PM+0x233, 0) with no alive test; GetLeader calls PartyManager_GetPartyLeader.
inline constexpr std::array<std::uintptr_t, 2> kBattleScriptPlayerActorCalls{0x140922452, 0x140938923};  // E8 F9 E1 CC 00, E8 28 7D CB 00
inline constexpr std::uintptr_t kBattleScriptLeaderCall = 0x1417B32DA;  // E8 01 EA E3 FF
inline constexpr std::array<std::uint8_t, 16> kCharaRegistryIterBeginEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};
inline constexpr platform::FunctionFact kCharaRegistryIterBegin{L"EndCharaRegistry_IterBegin_ByCategoryBit", 0x140F99920,
                                                                kCharaRegistryIterBeginEntry};
inline constexpr std::array<std::uint8_t, 16> kCharaRegistryIterNextEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x8B, 0x41, 0x10, 0x48, 0x8B, 0xD9, 0x44, 0x39, 0x81};
inline constexpr platform::FunctionFact kCharaRegistryIterNext{L"EndCharaRegistry_IterNext", 0x140F99A60, kCharaRegistryIterNextEntry};
inline constexpr std::array<std::uint8_t, 16> kBattleCompSetTargetCharEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x83, 0xB9, 0x10, 0x03, 0x00, 0x00};
inline constexpr platform::FunctionFact kBattleCompSetTargetChar{L"BattleComp_SetTargetChar", 0x14096BFA0, kBattleCompSetTargetCharEntry};
inline constexpr std::array<std::uint8_t, 16> kGameFreeEntry{
    0x48, 0x85, 0xC9, 0x74, 0x2E, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x0D};
inline constexpr platform::FunctionFact kGameFree{L"FMemory::Free", 0x1416C0370, kGameFreeEntry};

// --- The battle sequence cameras (RE-seq-camera-two.md) --------------------------------------------------------
// The request lists at `[sys+0x78]`: two singly linked lists of 48-byte entries (+0x08 weak sequence actor, +0x19
// active byte, +0x20 next). A sequence actor's camera actor at +0x468; the per-view state the game keeps once:
// the camera actor's history block (its seven latch bytes first), the sequence's blend pose (the primary camera
// manager's cached POV block), its cut one-shot, its play state and its blend-out delta latch and delta.
inline constexpr std::uint32_t kBattleSysCamRequests     = 0x78;
inline constexpr std::uint32_t kSeqListOneHead           = 0x08;
inline constexpr std::uint32_t kSeqListTwoHead           = 0x28;
inline constexpr std::uint32_t kSeqEntrySequenceWeak     = 0x08;
inline constexpr std::uint32_t kSeqEntryActive           = 0x19;
inline constexpr std::uint32_t kSeqEntryNext             = 0x20;
inline constexpr std::uint32_t kSeqCameraActor           = 0x468;
inline constexpr std::uint32_t kSeqBlendPose             = 0x550;
inline constexpr std::uint32_t kSeqBlendPoseBytes        = 0x30;
inline constexpr std::uint32_t kSeqCutOneShot            = 0x580;
inline constexpr std::uint32_t kSeqPlayState             = 0x584;
inline constexpr std::uint8_t  kSeqPlayStateStopped      = 2;
inline constexpr std::uint32_t kSeqBlendOutDelta         = 0x598;  // the latch (4) and the delta (+0x59C..+0x5B3)
inline constexpr std::uint32_t kSeqBlendOutDeltaBytes    = 0x1C;
inline constexpr std::uint32_t kSeqCameraHistory         = 0xBF0;
inline constexpr std::uint32_t kSeqCameraHistoryBytes    = 0xB8;
inline constexpr std::uint32_t kSeqCameraLatchBytes      = 7;
inline constexpr std::uint32_t kSeqOutPoseActiveWeak     = 0x38;   // the operator's out pose (op+0x3A8): the active camera weak (index, serial), valid byte at +0x40
inline constexpr std::uint32_t kCameraManagerCachePointer = 0x22F8;  // AEndPlayerCameraManager: the cached view, when set
inline constexpr std::uint32_t kCameraManagerInlineCache  = 0x1E48;  // else its inline cache
inline constexpr std::uint32_t kCameraCachePov            = 0x480;   // the POV block inside the cache (0x30 bytes)

inline constexpr std::array<std::uint8_t, 16> kSeqApplyEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
inline constexpr platform::FunctionFact kSeqApply{L"BattleCamRequests_ApplySequenceOverride", 0x140A15D30, kSeqApplyEntry};
inline constexpr std::uintptr_t kSeqApplyCall = 0x140FDEFE8;  // E8 43 6D A3 FF in the operator tick, inside its block
inline constexpr std::array<std::uint8_t, 16> kSeqEvalEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
inline constexpr platform::FunctionFact kSeqEval{L"BattleCamRequest_EvalSequenceOverride", 0x140A16470, kSeqEvalEntry};
inline constexpr std::uintptr_t kSeqEvalCall = 0x140A15DE4;  // E8 87 06 00 00 in ApplySequenceOverride, inside its block
inline constexpr std::array<std::uint8_t, 16> kSeqCutChangeEntry{
    0x48, 0x8B, 0xC4, 0x53, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00, 0x80, 0xB9, 0xB4, 0x05, 0x00};
inline constexpr platform::FunctionFact kSeqCutChange{L"AEndBattleLevelSequenceActor::ApplyCutChange", 0x140A12FE0, kSeqCutChangeEntry};
inline constexpr std::uintptr_t kSeqCutChangeCall = 0x140A16EC5;  // E8 16 C1 FF FF in OnCutChange_PrimaryPCMPose, inside its block
inline constexpr std::array<std::uint8_t, 16> kSeqJumpToCutEntry{
    0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x48, 0x20, 0x4C, 0x89, 0x40, 0x18, 0x48, 0x89, 0x48, 0x08, 0x55};
inline constexpr platform::FunctionFact kSeqJumpToCut{L"AEndBattleLevelSequenceActor::JumpToCut_CapturePrimaryPose", 0x140A11900, kSeqJumpToCutEntry};
inline constexpr std::uintptr_t kSeqJumpToCutCall = 0x140A14EAA;  // E8 51 CA FF FF in BattleCamRequests_ForOwner_JumpToCut, inside its block
inline constexpr std::array<std::uint8_t, 16> kSeqCutFirstPlayerEntry{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x0D, 0x35, 0x5D, 0x81, 0x04, 0x48, 0x85, 0xC9, 0x75, 0x04};
inline constexpr platform::FunctionFact kSeqCutFirstPlayer{L"Camera_SetCutBit_FirstPCPCM", 0x140FD3D70, kSeqCutFirstPlayerEntry};
inline constexpr std::uintptr_t kSeqCutRaiseCall = 0x140A15E2D;  // E8 3E DF 5B 00 in ApplySequenceOverride; crosses a block
inline constexpr std::array<std::uint8_t, 16> kSeqCutPrimaryEntry{
    0x48, 0x83, 0xEC, 0x28, 0xE8, 0x37, 0x4B, 0x8E, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x0A, 0x81, 0x88};
inline constexpr platform::FunctionFact kSeqCutPrimary{L"PrimaryPCM_SetCameraCutBit400", 0x140DB29E0, kSeqCutPrimaryEntry};

// --- The battle switch (RE-battle-switch.md §4) -----------------------------------------------------------------
// BattleSystem_CycleLeader walks the members from the leader; BattleSystem_CanSwitchLeader counts them. Each fetches
// a candidate with PartyManager_GetPlayerActor(PM, 0, type, 0); a null answer is the game's own skip branch. The
// cycle's call sits inside a block; CanSwitch's crosses one (placed from the game thread, outside the function).
inline constexpr std::uintptr_t kCycleLeaderCandidateCall = 0x1409BAC99;
inline constexpr std::uintptr_t kCanSwitchCandidateCall   = 0x1409BA98C;

// --- The party order, a member's AI state and the game's leader funnels (RE-slot-member.md) --------------------
/// The data singleton (allocated lazily): the field leader's type (DefaultPartyLeaderType) and the eight player
/// status bytes (EPlayerStatus: 1 PARTY, 5 PARTY_Battle_Support are in the formation; 5 is skipped by the cycle).
inline constexpr std::uintptr_t kDataSingletonGlobal   = 0x1457E9768;
inline constexpr std::uint32_t  kDataFieldLeaderType   = 0x5089B;
inline constexpr std::uint32_t  kDataPlayerStatus      = 0x5089C;  // u8[8] by player type
inline constexpr std::uint8_t   kStatusParty           = 1;
inline constexpr std::uint8_t   kStatusBattleSupport   = 5;
/// AEndCharacter: the PLAYER controller slot (the reconcile lets it win), the field AI, the actor-mode object
/// (reached as char800+0xA00 by the game); on that object the actor type (EActorType, 0 PLAYER, 1 PARTY) and the
/// actor mode (EActorMode: 1 FIELD, 3 FIELD_CINEMA, 4 CAUTION, 5 BATTLE, 7 CINEMA).
inline constexpr std::uint32_t  kCharacterPlayerSlot   = 0xCE8;
inline constexpr std::uint32_t  kCharacterFieldAi      = 0xC88;
inline constexpr std::uint32_t  kCharacter800          = 0x800;   // the EndCharacter800 interface the game's calls take
inline constexpr std::uint32_t  kCharacterActorMode    = 0x1200;  // the actor-mode object
inline constexpr std::uint32_t  kActorModeType         = 0xB8;
inline constexpr std::uint32_t  kActorModeMode         = 0xBA;
inline constexpr std::uint8_t   kActorTypePlayer       = 0;
inline constexpr std::uint8_t   kActorModeField        = 1;
inline constexpr std::uint8_t   kActorModeFieldCinema  = 3;
inline constexpr std::uint8_t   kActorModeCaution      = 4;
inline constexpr std::uint8_t   kActorModeBattle       = 5;
inline constexpr std::uint8_t   kActorModeCinema       = 7;
/// AEndFieldAIController: a stack of 15 request layers; the active layer (0 = the party's default layer, 1 = the
/// level's AI setting, 16 = none) and its kind: 1 Stable (the stream's default) or 11, the level's default AI setting
/// (a follow with a formation position and gait: Aerith following Cloud in the field reads layer 1 kind 11
/// [CONFIRMED live 2026-10-19]). Another kind is a script's hold; the layer is not read (a cutscene's preparation
/// copies the active record into the cinema layer 13 for the members outside the scene).
inline constexpr std::uint32_t  kFieldAiActiveLayer    = 0xDF8;
inline constexpr std::uint32_t  kFieldAiActiveKind     = 0x55B8;
inline constexpr std::uint8_t   kFieldAiKindStable     = 1;
inline constexpr std::uint8_t   kFieldAiKindDefaultSetting = 11;
inline constexpr std::uint32_t  kFieldAiLayerRecords   = 0xE00;   // 15 records of 0x4C8 bytes; byte +0 = the kind (0 empty)
inline constexpr std::uint32_t  kFieldAiLayerBytes     = 0x4C8;
inline constexpr std::uint32_t  kFieldAiLayers         = 15;
/// The field control lock mask on the state manager ([root+0x138]): bit 0x100000 while an event holds the field.
inline constexpr std::uint32_t  kRootStateManager      = 0x138;
inline constexpr std::uint32_t  kStateManagerControlLock = 0x11062B0;
inline constexpr std::uint32_t  kControlLockEvent      = 0x100000;
/// The leader funnels: every path that seats player 1 on a character passes PartyManager_SetPartyLeader(PM, actor,
/// input, force) or its toad twin (PM, actor) before any party-manager write; both open with pushes.
inline constexpr std::array<std::uint8_t, 16> kSetPartyLeaderEntry{
    0x40, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x4C, 0x8B};
inline constexpr std::size_t kSetPartyLeaderPushes = 8;
inline constexpr platform::FunctionFact kSetPartyLeader{L"PartyManager_SetPartyLeader", 0x1415F18B0, kSetPartyLeaderEntry};
inline constexpr std::array<std::uint8_t, 16> kSetPartyLeaderToadEntry{
    0x40, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x4C, 0x8B, 0xF1, 0x48, 0x8B, 0xFA};
inline constexpr std::size_t kSetPartyLeaderToadPushes = 6;
inline constexpr platform::FunctionFact kSetPartyLeaderToad{L"PartyManager_SetPartyLeader_NoInputNoForce", 0x1415F1AE0,
                                                           kSetPartyLeaderToadEntry};
/// The formation's one writer, at the end of every player stream, reached by the Blueprint exec's tail jump.
inline constexpr std::array<std::uint8_t, 16> kFinishPlayerStreamEntry{
    0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41};
inline constexpr platform::FunctionFact kFinishPlayerStream{L"PartyManager_FinishPlayerStream_RebuildFormation", 0x1415F08F0,
                                                            kFinishPlayerStreamEntry};
inline constexpr std::uintptr_t kFinishPlayerStreamJump = 0x141B90183;  // E9 68 07 A6 FF, inside its block
/// The game's hand of its leader to its AI (the PLAYER slot cleared, the controller reconciled) and the return
/// (the slot set to the primary controller): the event's start and end, and the field-action tick.
inline constexpr std::array<std::uint8_t, 16> kHandLeaderToAiEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xE8, 0xE5, 0x13, 0x66, 0xFF, 0x48, 0x8B, 0xC8, 0x33, 0xD2};
inline constexpr platform::FunctionFact kHandLeaderToAi{L"Party_HandLeaderToAI_ClearPlayerSlot", 0x140FF66D0, kHandLeaderToAiEntry};
inline constexpr std::array<std::uintptr_t, 2> kHandLeaderCalls{0x1411215B6, 0x140FF62FB};  // E8 15 51 ED FF, E8 D0 03 00 00
inline constexpr std::array<std::uint8_t, 16> kReturnLeaderToPcEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x81, 0x38, 0x01, 0x00};
inline constexpr platform::FunctionFact kReturnLeaderToPc{L"Party_ReturnLeaderToPrimaryPC", 0x140FF6720, kReturnLeaderToPcEntry};
inline constexpr std::uintptr_t kReturnLeaderCall = 0x141121933;  // E8 E8 4D ED FF, inside its block
/// The reconcile (char800 vt+0x128: the PLAYER slot wins, else the AI for the mode) and the actor-type setter
/// (char800 vt+0x110), as the game's own hand-backs and the formation rebuild call them.
inline constexpr std::array<std::uint8_t, 16> kReconcileControllerEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48};
inline constexpr platform::FunctionFact kReconcileController{L"EndCharacter800_ReconcileActiveController", 0x1416450F0,
                                                             kReconcileControllerEntry};
inline constexpr std::array<std::uint8_t, 16> kSetActorTypeEntry{
    0x48, 0x8B, 0x81, 0x00, 0x0A, 0x00, 0x00, 0x88, 0x90, 0xB8, 0x00, 0x00, 0x00, 0x80, 0xFA, 0x02};
inline constexpr platform::FunctionFact kSetActorType{L"EndCharacter800_SetActorType", 0x14164DE50, kSetActorTypeEntry};

// --- The battle target cursor (RE-target-cursor-two.md) --------------------------------------------------------
/// The one cursor widget's pointer on the battle system (outside the target fields), and the HUD keys the cursor
/// driver writes each frame: the enemy bar manager's (EndUISystem[27]) displayed target (a weak pointer pair) and
/// the off-screen icon manager's (EndUISystem[38]) target name (an FString at +0x88, its FName at +0x98).
inline constexpr std::uint32_t  kBattleSysCursorWidget        = 0x10;
inline constexpr std::uintptr_t kEnemyBarSlotGlobal           = 0x1459CAA48;  // EndUISystem[27]
inline constexpr std::uint32_t  kEnemyBarDisplayedTarget      = 0xD8;
inline constexpr std::uint32_t  kEnemyBarDisplayedTargetBytes = 0x10;
inline constexpr std::uintptr_t kOutsideIconSlotGlobal        = 0x1459CAAA0;  // EndUISystem[38]
inline constexpr std::uint32_t  kOutsideIconTargetName        = 0x88;
inline constexpr std::uint32_t  kOutsideIconTargetNameBytes   = 0x18;
/// The cursor driver (creates, moves, styles, traces and releases the cursor from the leader's target; called by the
/// battle tick after each targeting call), the line-of-sight step (once per frame from the field systems' tick;
/// its call crosses a block) and the battle suspension that releases the cursor (entry: three instructions, 5 bytes).
inline constexpr std::array<std::uint8_t, 16> kCursorDriverEntry{
    0x40, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x38, 0xFE, 0xFF, 0xFF};
inline constexpr platform::FunctionFact kCursorDriver{L"BattleSystem_TickTargetCursorWidget_Slot18", 0x1409B9170, kCursorDriverEntry};
inline constexpr std::array<std::uint8_t, 16> kCursorLineOfSightEntry{
    0x4C, 0x8B, 0xDC, 0x53, 0x48, 0x81, 0xEC, 0x10, 0x02, 0x00, 0x00, 0x48, 0x8B, 0x05, 0xE6, 0x03};
inline constexpr platform::FunctionFact kCursorLineOfSight{L"BattleSystem_TickTargetCursorLineOfSight_Slot18", 0x1409C06D0,
                                                           kCursorLineOfSightEntry};
inline constexpr std::uintptr_t kCursorLineOfSightCall = 0x1416BF36E;  // E8 5D 13 30 FF, crosses a block
inline constexpr std::array<std::uint8_t, 16> kBattleSuspendEntry{
    0x4C, 0x8B, 0xDC, 0x55, 0x53, 0x57, 0x41, 0x54, 0x41, 0x55, 0x49, 0x8D, 0xAB, 0x68, 0xFE, 0xFF};
inline constexpr std::size_t kBattleSuspendFirstBytes = 5;  // mov r11, rsp; push rbp; push rbx
inline constexpr platform::FunctionFact kBattleSuspend{L"BattleSystem_Suspend_ReleaseCursorAndIcons", 0x1409B5630, kBattleSuspendEntry};

}  // namespace lss::facts
