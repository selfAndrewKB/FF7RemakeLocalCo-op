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
inline constexpr std::uint32_t kObjectName                = 0x18;   // UObjectBase::NamePrivate (an FName)
inline constexpr std::uint32_t kLocalPlayerController     = 0x30;   // UPlayer::PlayerController
inline constexpr std::uint32_t kControllerPawn            = 0x380;  // AController::Pawn
inline constexpr std::uint32_t kControllerControlRotation = 0x3A8;  // AController::ControlRotation (pitch, yaw, roll)
// AEndPlayerController: the right stick's turn rate (float, the GameParams entry CameraLeapTimeScale; the turn is this
// times the Responsiveness level).
inline constexpr std::uint32_t kControllerCameraRate = 0x9E8;
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
/// A further local player's actor gets a name of the same length ending in its player number (controller id + 1:
/// "EndCameraActr2" for id 1, "EndCameraActr3" for id 2), so the spawn's duplicate-name replacement (UE 4.18
/// StaticAllocateObject reuses an existing object of that name) can hit neither player 1's actor nor another
/// further player's: with one name for every further player, player 3's join took over player 2's camera actor
/// (seen live).
constexpr std::array<char, 14> FurtherCameraActorText(int controllerId) noexcept
{
    std::array<char, 14> text{'E', 'n', 'd', 'C', 'a', 'm', 'e', 'r', 'a', 'A', 'c', 't', 'r', '?'};
    text[13] = controllerId >= 1 && controllerId <= 8 ? static_cast<char>('1' + controllerId) : '9';
    return text;
}

// --- Local players (RE-00-triage.md) -------------------------------------------------------------
inline constexpr std::array<std::uint8_t, 20> kRemoveLocalPlayerEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
    0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20};
/// UGameInstance::RemoveLocalPlayer(ULocalPlayer*): bool.
inline constexpr platform::FunctionFact kRemoveLocalPlayer{L"UGameInstance::RemoveLocalPlayer", 0x142F3D3B0,
                                                           kRemoveLocalPlayerEntry};

// --- A further player's field camera (RE-camera.md §9, RE-impl-facts-v0.2.md) ---------------------
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
inline constexpr std::int32_t   kFieldStepSteady      = 13;     // the steady field: the state manager drains its trigger queue from it
inline constexpr std::size_t    kFieldRootRunning     = 0x74;   // byte: the step machine runs
inline constexpr std::size_t    kFieldRootLocation    = 0x78;   // FName: the current location
/// UEndMenuAPI::execIsPauseMenuClosed(ctx, FFrame&, bool* result) (RE-pause-menu-any-pad.md §3.4): the Blueprint
/// thunk with the test inlined (the pause, main and map menus' weak slots, Libra, game over, the named widgets).
/// It only reads and bumps the frame's code pointer (+0x20), which stays 0 in a zeroed frame.
inline constexpr std::array<std::uint8_t, 16> kIsPauseMenuClosedEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x42, 0x20, 0x33, 0xC9, 0x48, 0x85, 0xC0, 0x49};
inline constexpr platform::FunctionFact kIsPauseMenuClosed{L"UEndMenuAPI::execIsPauseMenuClosed", 0x141B25ED0,
                                                           kIsPauseMenuClosedEntry};
inline constexpr std::size_t kScriptFrameBytes = 0x80;  // a zeroed FFrame for the thunk
/// The FKey globals of the three menu-opening actions (their first 8 bytes are the FName), filled by the game's
/// static initialisers (RE-pause-menu-any-pad.md §1.1, §2.2).
inline constexpr std::uintptr_t kPauseMenuKeyGlobal  = 0x145903B00;
inline constexpr std::uintptr_t kMapMenuKeyGlobal    = 0x145903BD8;
inline constexpr std::uintptr_t kEnemyIntelKeyGlobal = 0x145903BF0;

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
inline constexpr std::uint32_t  kOperatorCameraLocation = 0x410;      // FVector: the tick's camera location (RE-lock-camera-ledger.md §12, §13, read live)
inline constexpr std::uint32_t  kGameModeFrameCounter = 0x770;        // AEndGameMode: the engine frame counter, copied at world-tick phase 100
inline constexpr std::uint32_t  kOperatorObjectFlags  = 0x48;         // the game's spawn flags for the operator
inline constexpr std::uint32_t  kSpawnParamsTemplate  = 0x08;         // FActorSpawnParameters::Template (RE-player-trigger.md §4.2)
inline constexpr std::uint32_t  kSpawnParamsObjectFlags = 0x2C;       // FActorSpawnParameters::ObjectFlags

inline constexpr std::uintptr_t kFieldCameraOperatorVTable = 0x144BB5D78;
inline constexpr std::uintptr_t kSquareInputOwnerGlobal    = 0x1459CA980;  // SquareInput owner*
inline constexpr std::uint32_t  kStickBanks        = 0x6D0;  // TArray<record> per device bank, 16 bytes each
inline constexpr std::uint32_t  kPlayStationBank   = 0;      // the PlayStation reader's (its events carry no device scope)
inline constexpr std::uint32_t  kXInputBank        = 1;
inline constexpr std::uint32_t  kStickRecordBytes  = 24;     // LX, LY, RX, RY, LT, RT
inline constexpr std::uint32_t  kStickAnalog       = 0x700;  // LX, LY, RX, RY: what the getters read
inline constexpr std::uint32_t  kStickDigital      = 0x6A0;  // key-driven stick, wins when non-zero
/// The keyboard's and mouse's direction bits the input tick builds the key-driven stick from (the left stick's 8 bytes,
/// then the right's: bit 1 left, 2 right, 4 up, 8 down) and its two trigger bytes (the key-driven triggers' source).
inline constexpr std::uint32_t  kStickDigitalBits        = 0x6B0;
inline constexpr std::uint32_t  kStickDigitalBitsBytes   = 16;
inline constexpr std::uint32_t  kStickDigitalBitsRight   = 8;
inline constexpr std::uint32_t  kTriggerDigitalBits      = 0x6C8;
inline constexpr std::uint32_t  kTriggerDigitalBitsBytes = 2;

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

// --- The pad slots (RE-pad-slots.md) ---------------------------------------------------------------------------
// Unreal's stock XInputInterface::SendControllerEvents is the game's only XInput caller, through these two import
// slots of xinput1_3 (read at every call: `call [rip+slot]`; .rdata, read-only). It polls a slot only while the slot
// is marked connected, or every slot while the interface's re-check byte is set (cleared after the poll; the window
// procedure's WM_DEVICECHANGE sets it).
inline constexpr std::uintptr_t kXInputGetStateImport   = 0x1440941F8;
inline constexpr std::uintptr_t kXInputSetStateImport   = 0x1440941F0;
inline constexpr std::uintptr_t kWindowsApplicationGlobal = 0x145827708;  // FWindowsApplication* (GWindowsApplication)
inline constexpr std::uint32_t  kAppXInputInterface     = 0x1D8;          // XInputInterface*
inline constexpr std::uint32_t  kAppInputDevices        = 0x1E8;          // TArray<IInputDevice*> (data, num at +8)
inline constexpr std::uint32_t  kXInputNeedsStateUpdate = 0x08;           // bNeedsControllerStateUpdate (bool)
inline constexpr std::uint32_t  kXInputControllerStates = 0x28;           // FControllerState[4], one per slot
inline constexpr std::uint32_t  kXInputControllerStateBytes = 0x100;      // the records' stride
inline constexpr std::uint32_t  kXInputStateConnected   = 0xE8;           // a record's bIsConnected (bool)
inline constexpr std::uintptr_t kSonyControllerDeviceVTable = 0x144F47608;  // the PlayStation pad device (scePad): not XInput
/// The engine's device list holds 16-byte shared references, the object pointer first.
inline constexpr std::uint32_t  kAppInputDeviceBytes    = 0x10;

// --- The PlayStation reader (RE-playstation-pads.md §5, §9) ----------------------------------------------------------
// Sony's scePad library, linked into the exe: SonyControllerDevice__SendEventsForSlot reads each record's pad through
// scePadReadState(handle, data), its only caller, on the game thread's device poll; the rumble goes through
// scePadSetVibration(handle, u8 motors[2]) (large, small) from two callers. Both functions open with a home-area store.
inline constexpr std::uintptr_t kSonyReadState    = 0x143F7EC50;
inline constexpr std::uintptr_t kSonySetVibration = 0x143F7EF70;
inline constexpr std::array<std::uint8_t, 5> kSonyLibraryFirst{0x48, 0x89, 0x5C, 0x24, 0x08};  // mov [rsp+8], rbx
/// The library's system-button switch (u8, zero at start, no writer in the game): while it is zero, the read clears
/// Create, PS and Mute from every pad's buttons.
inline constexpr std::uintptr_t kSonySystemButtons = 0x1457C5F71;
/// A PlayStation reader record (record N reads slot N): the pad data scePadReadState fills, the record's controller id
/// (i32), its scePad handle (i32; handle & 0xFF = the scePad user 1..4, the pad's identity), the connected byte of its
/// last poll (a rise runs the game's connect path: controller information, the connection broadcast), and the last motor
/// values sent (floats; rumble is sent when they change).
inline constexpr std::uint32_t kSonyRecordPadData   = 0x78;
inline constexpr std::uint32_t kSonyRecordId        = 0x210;
inline constexpr std::uint32_t kSonyRecordHandle    = 0x218;
inline constexpr std::uint32_t kSonyRecordConnected = 0x228;
inline constexpr std::uint32_t kSonyRecordLastLarge = 0x268;
inline constexpr std::uint32_t kSonyRecordLastSmall = 0x26C;
/// ScePadData: buttons (u32; bit 31 set makes the record skip the frame), the sticks LX LY RX RY (u8, centre 0x80),
/// the triggers L2 R2 (u8), the touch count (u8), connected (u8).
inline constexpr std::uint32_t kPadDataButtons    = 0x00;
inline constexpr std::uint32_t kPadDataSticks     = 0x04;
inline constexpr std::uint32_t kPadDataTriggers   = 0x08;
inline constexpr std::uint32_t kPadDataTouchCount = 0x34;
inline constexpr std::uint32_t kPadDataConnected  = 0x4C;

// --- Text in a player's own view (RE-view-text.md) --------------------------------------------------------------
// UGameViewportClient::Draw gives each local player's HUD a canvas offset to that player's view and calls its
// PostRender through the AHUD vtable (every HUD here is the plain engine class: one table).
inline constexpr std::array<std::uint8_t, 16> kHudPostRenderEntry{
    0x40, 0x55, 0x53, 0x48, 0x8D, 0x6C, 0x24, 0xB1, 0x48, 0x81, 0xEC, 0xD8, 0x00, 0x00, 0x00, 0x48};
inline constexpr platform::FunctionFact kHudPostRender{L"AHUD::PostRender", 0x142F98580, kHudPostRenderEntry};
inline constexpr platform::SlotFact     kHudPostRenderSlot{L"vtbl_AHUD +0x660 (PostRender)", 0x144E6A408 + 0x660, kHudPostRender};
inline constexpr std::array<std::uint8_t, 16> kDrawShadowedStringEntry{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x55, 0x57, 0x41, 0x56, 0x48};
// int32 (FCanvas*, float X, float Y, const wchar_t*, const UFont*, const FLinearColor*, float TextScale): view-relative
inline constexpr platform::FunctionFact kDrawShadowedString{L"FCanvas::DrawShadowedString", 0x14333D3B0, kDrawShadowedStringEntry};
inline constexpr std::uint32_t kHudOwner         = 0x378;  // AHUD::PlayerOwner (APlayerController*)
inline constexpr std::uint32_t kHudCanvas        = 0x3C0;  // AHUD::Canvas (UCanvas*), set only inside the HUD pass
inline constexpr std::uint32_t kCanvasClipX      = 0x30;   // UCanvas::ClipX (float): the view's width in pixels
inline constexpr std::uint32_t kCanvasClipY      = 0x34;   // UCanvas::ClipY
inline constexpr std::uint32_t kCanvasFCanvas    = 0x260;  // UCanvas::Canvas (FCanvas*)
inline constexpr std::uint32_t kEngineMediumFont = 0x70;   // UEngine::MediumFont (UFont*, Roboto)

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

// --- The keyboard and mouse for any player (RE-kbm-routing.md §1.3, §2) --------------------------------------------
// The scene viewport hands every key, mouse button, wheel turn and mouse move to the game viewport client's InputKey
// and InputAxis with the Slate user as the controller id (0 for the keyboard and mouse; the mouse axes always 0); each
// gives the input to the local player holding that controller id. A FKey is an FName first; by value it is passed as
// a pointer to the caller's copy, which the callee destroys.
inline constexpr std::uintptr_t kGameViewportClientVTable = 0x144E5BB68;  // its FViewportClient part
inline constexpr std::array<std::uint8_t, 16> kViewportInputKeyEntry{
    0x44, 0x89, 0x44, 0x24, 0x18, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x48, 0x8D, 0x6C};
inline constexpr std::array<std::uint8_t, 16> kViewportInputAxisEntry{
    0x44, 0x89, 0x44, 0x24, 0x18, 0x53, 0x55, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xD0};
/// bool InputKey(this, FViewport*, int32 ControllerId, FKey, EInputEvent, float AmountDepressed, bool bGamepad).
inline constexpr platform::SlotFact kViewportInputKey{L"UGameViewportClient::InputKey (FViewportClient slot 8)",
                                                      kGameViewportClientVTable + 8 * 8,
                                                      {L"UGameViewportClient::InputKey", 0x142F634C0, kViewportInputKeyEntry}};
/// bool InputAxis(this, FViewport*, int32 ControllerId, FKey, float Delta, float DeltaTime, int32 NumSamples, bool bGamepad).
inline constexpr platform::SlotFact kViewportInputAxis{L"UGameViewportClient::InputAxis (FViewportClient slot 9)",
                                                       kGameViewportClientVTable + 9 * 8,
                                                       {L"UGameViewportClient::InputAxis", 0x142F63E70, kViewportInputAxisEntry}};
inline constexpr std::int32_t kInputEventPressed  = 0;  // EInputEvent IE_Pressed
inline constexpr std::int32_t kInputEventReleased = 1;  // IE_Released
/// FKeyEvent (0x40 bytes): the system repeat flag (u8), the key's shared details reference controller (the FKey's
/// TSharedPtr: object, then controller), the Windows key code (u32; a pad button's event carries none).
inline constexpr std::uint32_t kKeyEventBytes        = 0x40;
inline constexpr std::uint32_t kKeyEventRepeat       = 0x10;
inline constexpr std::uint32_t kKeyEventKeyReference = 0x30;
inline constexpr std::uint32_t kKeyEventKeyCode      = 0x3C;
inline constexpr std::uint32_t kSharedReferenceCount = 0x08;  // a reference controller's shared count (i32, not atomic)
inline constexpr std::uint32_t kKeyEventPath         = 0x18;  // the event's widget path pointer (a fresh event's is null)
inline constexpr std::array<std::uint8_t, 16> kKeyEventDestroyEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x59, 0x30, 0x48, 0x8B};
/// FKeyEvent's destructor (this; it never frees the event): lets go of the key's details reference only.
inline constexpr platform::FunctionFact kKeyEventDestroy{L"FKeyEvent::~FKeyEvent", 0x14132A330, kKeyEventDestroyEntry};
/// FPointerEvent: the effecting button's FKey (its FName first).
inline constexpr std::uint32_t kPointerEventButton = 0x40;
/// The input object's "keyboard or mouse used" byte (its key, mouse-button and wheel handlers set it; the input tick
/// folds it into the device class and clears it) and the device class (0 Sony, 1 keyboard and mouse, 2 XInput, 3
/// DirectInput), which picks the button art and the rumble's device.
inline constexpr std::uint32_t kInputKeyboardUsed    = 0x54;
inline constexpr std::uint32_t kInputDeviceClass     = 0x75C;
inline constexpr std::uint8_t  kDeviceClassKeyboard  = 1;
/// The input object's mouse-button and wheel handlers (this, FSlateApplication&, const FPointerEvent&), the pointer
/// event's user 0 only.
inline constexpr std::array<std::uint8_t, 16> kInputMouseDownEntry{
    0x4C, 0x8B, 0xDC, 0x55, 0x56, 0x57, 0x49, 0x8D, 0xAB, 0x28, 0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC};
inline constexpr std::array<std::uint8_t, 16> kInputMouseUpEntry{
    0x40, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x78, 0xC6, 0x41, 0x52, 0x01, 0x49, 0x8B, 0xF8, 0x41, 0x83};
inline constexpr std::array<std::uint8_t, 16> kInputWheelEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05};
inline constexpr platform::SlotFact kInputMouseDown{L"SquareInput::HandleMouseButtonDownEvent (slot 6)", kSquareInputVTable + 6 * 8,
                                                    {L"SquareInput::HandleMouseButtonDownEvent", 0x14143AA50, kInputMouseDownEntry}};
inline constexpr platform::SlotFact kInputMouseUp{L"SquareInput::HandleMouseButtonUpEvent (slot 7)", kSquareInputVTable + 7 * 8,
                                                  {L"SquareInput::HandleMouseButtonUpEvent", 0x14143AFF0, kInputMouseUpEntry}};
inline constexpr platform::SlotFact kInputWheel{L"SquareInput::HandleMouseWheelOrGestureEvent (slot 10)", kSquareInputVTable + 10 * 8,
                                                {L"SquareInput::HandleMouseWheelOrGestureEvent", 0x14143B400, kInputWheelEntry}};
/// The input tick's repeat of a held mouse button: a direct call of the action dispatcher.
inline constexpr std::uintptr_t kMouseRepeatCall = 0x141434AF9;

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
/// What the mod forces without the bloom fix: bit 0x2 (Bloom) leaks player 1's moving content into the further views
/// (live A/B, Documentation/plan/topics/render.md) unless render/bloom_origin is installed, which raises the mask to kSplitDisabledFlags.
inline constexpr std::uint32_t  kSplitForcedFlags     = 0x1008;

// --- AO downsample stage-0 origin (RE-rdc-split-frame.md, RE-impl-facts-ao-fix.md) ------------------
// SE_AODownsamplePS0__SetParameters is shared by AO downsample stages 0/1/2; only stage 0's pixel shader reads
// the taps (cb0[19], cb0[20]), at ftoi(tap) + 2*SV_Position, without View.ViewRect.Min.
inline constexpr std::uint32_t kPassContextView = 0x00;  // FRenderingCompositePassContext: const FViewInfo*
inline constexpr std::uint32_t kViewRectMinX    = 0x70;  // FViewInfo::ViewRect.Min.X (int32)
inline constexpr std::uint32_t kViewRectMinY    = 0x74;
inline constexpr std::uint32_t kViewRectMaxX    = 0x78;  // ViewRect.Max.X: FIntRect is {Min, Max}, stock 4.18

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

// --- Distortion apply target size (RE-render-views.md 4A.1, from a frame capture) ---------------------------------
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

/// The original level-0 reduce shader's OutputHash, read from the running game (D3D12,
/// tools/bloom_shader/find_live_hash.py). It identifies the shader on both RHIs; D3D11 keeps no copy of the code.
inline constexpr std::array<std::uint8_t, 20> kBloomL0ShaderHash{
    0x8E, 0x8D, 0x85, 0xC6, 0x4A, 0x88, 0x3B, 0x86, 0x8A, 0x69, 0x96, 0xB8, 0xCF, 0xDC, 0xBA, 0xB2, 0x0B, 0x5F, 0x6D, 0x9C};
// The engine's framing of that shader's code blob, around the DXBC ([resource tables][DXBC][optional data][footer]),
// as logged from the D3D12 shader object's code copy: empty resource tables; optional data 'p' (packed
// resource counts 01 01 01 01 00) and 'u' (size 4, zero); footer 23 = the optional data's size, footer included.
inline constexpr std::array<std::uint8_t, 24> kBloomL0CodePrefix{};
inline constexpr std::array<std::uint8_t, 23> kBloomL0CodeTrailer{
    0x70, 0x05, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x00, 0x75, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x17, 0x00, 0x00, 0x00};

// --- Square Enix's screen-space reflections (RE-third-view-ghost.md) ----------------------------------------------
// SE_RCPassSSR__Process 0x142549750 draws every view over the whole buffer; its pixel shaders add the view's corner to
// the already absolute pixel (render/dxbc_fix.h). The pipeline-state call in each of its two binders, SE_SSR__BindPS0
// 0x14255B900 and SE_SSR__BindPS1 0x14255BC90 (called by that pass only); each call inside one 16-byte block.
inline constexpr std::uintptr_t kSsrPs0PipelineCall = 0x14255BBB9;
inline constexpr std::uintptr_t kSsrPs1PipelineCall = 0x14255BF49;
// FD3D12PixelShader keeps its own copy of the code it was made from (RE-impl-facts-bloom-swap.md §1, [CONFIRMED]):
// the TArray<uint8> in the engine's framing, and the DXBC's place and length inside that copy. D3D11 keeps none.
inline constexpr std::uint32_t kD3D12PsBytecode       = 0x48;  // const uint8* (inside the code copy)
inline constexpr std::uint32_t kD3D12PsBytecodeLength = 0x50;  // SIZE_T
inline constexpr std::uint32_t kD3D12PsCode           = 0x58;  // TArray<uint8> {Data, Num, Max}

// --- Square Enix's ribbon trails' vertex fill (RE-ribbon-fill-cost.md §3) ------------------------------------------
/// FillVertexData(emitter data, FAsyncBufferFillData*): writes every ribbon point's two 80-byte vertices into the locked
/// dynamic vertex buffer, and reads each position back from it (write-combined memory on the GPU's upload heap).
/// Square Enix's ribbon emitter data only; its DoBufferFill 0x140D832F0 calls it through this slot on the render thread.
inline constexpr std::array<std::uint8_t, 16> kRibbonFillVertexEntry{
    0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x57, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0x08, 0xFE};
inline constexpr platform::SlotFact kRibbonFillVertexSlot{
    L"EndDynamicRibbonEmitterData::FillVertexData (slot 21)", 0x144B8E9F8 + 21 * 8,
    {L"EndDynamicRibbonEmitterData::FillVertexData", 0x140D87120, kRibbonFillVertexEntry}};
/// FAsyncBufferFillData (stock 4.18), as ParticleEmitter_AllocDynamicVertexIndexBuffers 0x14312D5A0 fills it.
inline constexpr std::uint32_t kBufferFillVertexCount = 0x88;  // int32
inline constexpr std::uint32_t kBufferFillVertexSize  = 0x8C;  // int32, bytes
inline constexpr std::uint32_t kBufferFillVertexData  = 0x90;  // void*, inside the locked buffer

// --- The sun's cascades per view, and the translucency lighting volume (RE-haze-per-view.md §2, §3, §9) -------------
/// FProjectedShadowInfo::DependentView: the view a cascade was made for (SetupWholeSceneProjection 0x1425EC850 writes
/// it; AddViewDependentWholeSceneShadowsForView passes &Views[i]); null for a shadow of no one view.
inline constexpr std::uint32_t kShadowDependentView = 0x38;
inline constexpr std::array<std::uint8_t, 16> kIsWholeSceneDirectionalShadowEntry{
    0x8B, 0x81, 0x68, 0x05, 0x00, 0x00, 0xA8, 0x40, 0x74, 0x10, 0x83, 0xB9, 0x24, 0x05, 0x00, 0x00};
inline constexpr platform::FunctionFact kIsWholeSceneDirectionalShadow{
    L"FProjectedShadowInfo::IsWholeSceneDirectionalShadow", 0x142214700, kIsWholeSceneDirectionalShadowEntry};
/// Its one call, in ComputeLightGrid's cascade loop (0x14221EA50; shadow in rcx). The view whose forward light data is
/// filled sits in the caller's frame at [rsp+78h] (written once at 0x14221EC9B; rsp fixed after the prologue).
inline constexpr std::uintptr_t kLightGridCascadeTestCall = 0x14221F483;
inline constexpr std::uint32_t  kLightGridFilledView      = 0x78;  // caller's rsp offset at the call
/// The caller's frame as the research read it, checked before the redirect: its stack allocation (after six pushes)
/// and the one store of the filled view (`mov [rsp+78h], r13`, r13 = Views + 0x28B0 * i).
inline constexpr std::uintptr_t kLightGridFrameAlloc = 0x14221EA61;
inline constexpr std::uint8_t   kLightGridFrameAllocBytes[] = {0x48, 0x81, 0xEC, 0x08, 0x09, 0x00, 0x00};
inline constexpr std::uintptr_t kLightGridViewStore = 0x14221EC9B;
inline constexpr std::uint8_t   kLightGridViewStoreBytes[] = {0x4C, 0x89, 0x6C, 0x24, 0x78};
inline constexpr std::array<std::uint8_t, 16> kInjectVolumeLightingEntry{
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
/// InjectTranslucentVolumeLightingArray(RHICmdList, const FViewInfo* view, const TArray<injection>* lights): reads the
/// list only ({Data, Num, Max}, 32-byte entries, the shadow at +8: render/sun_owner.h). Neither the list nor its data
/// outlives the call: the list pointer stays in a local, each entry's fields are copied to locals, and the deferred
/// commands it records hold render resources only (checked 0x1426285C1..E4) [CONFIRMED].
inline constexpr platform::FunctionFact kInjectVolumeLighting{L"InjectTranslucentVolumeLightingArray", 0x142627520,
                                                              kInjectVolumeLightingEntry};
/// Its four calls: RenderLights' unshadowed and shadowed lights, and the two batch helpers; each passes Views[0].
inline constexpr std::array<std::uintptr_t, 4> kInjectVolumeLightingCalls{0x1422354A8, 0x1422359F8, 0x142628964,
                                                                          0x142628B02};
inline constexpr std::array<std::uint8_t, 16> kCalcVolumeBoundsEntry{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x18, 0x57, 0x48, 0x81, 0xEC, 0x70};
/// FViewInfo::CalcTranslucencyLightingVolumeBounds(const FViewInfo* view, FBox* out): the volume's box from that view's
/// camera. Called once, from FViewInfo::SetupUniformBufferParameters (0x1425C4240), for each view each frame; the
/// result goes to the view's uniforms (+0x8F0..+0x92F) and fields (+0x2264..+0x2298). Return value unused.
inline constexpr platform::FunctionFact kCalcVolumeBounds{L"FViewInfo::CalcTranslucencyLightingVolumeBounds",
                                                          0x142623FC0, kCalcVolumeBoundsEntry};
inline constexpr std::uintptr_t kCalcVolumeBoundsCall = 0x1425C4F78;
/// FSceneView::Family (+0); FSceneViewFamily::Views {const FSceneView** Data, int32 Num} (+0, +8). The game's stereo
/// rule just before the call reads the same fields (0x1425C4F10).
inline constexpr std::uint32_t kViewFamily      = 0x00;
inline constexpr std::uint32_t kFamilyViews     = 0x00;
inline constexpr std::uint32_t kFamilyViewCount = 0x08;
/// FSceneView::State (FSceneViewStateInterface*); the renderer's FViewInfo copies it in place.
inline constexpr std::uint32_t kViewState = 0x08;
/// The area controllers' time blend (research/RE-indoor-light-two.md): FSceneViewFamily::Scene (+0x30), whose vtable
/// +0x2D0 answers the world; the world's rendering settings controllers (TArray at +0x8E8, count +0x8F0), each a
/// URenderingSettingsControllerComponent: BlendType +0x2C1 (3 TimeInBox, 4 TimeInSphere fade over BlendTime +0x2C8),
/// then unreflected: the pending time its tick adds (+0x2D4), the timer (+0x2D8), the weight (+0x2DC), the snap byte
/// (+0x2E4). FSceneView__StartFinalPostprocessSettings_SE_RSCBlend (0x14320FBD0, the call shadows_per_view redirects)
/// moves the timer by the pending time and zeroes it and the snap, per view.
inline constexpr std::uint32_t kFamilyScene         = 0x30;
inline constexpr std::uint32_t kSceneGetWorldSlot   = 0x2D0;
inline constexpr std::uint32_t kWorldAreaControllers     = 0x8E8;
inline constexpr std::uint32_t kWorldAreaControllerCount = 0x8F0;
inline constexpr std::uint32_t kRscBlendType = 0x2C1;
inline constexpr std::uint32_t kRscBlendTime = 0x2C8;
inline constexpr std::uint32_t kRscPending   = 0x2D4;  // pending, timer and weight are written for every blend type
inline constexpr std::uint32_t kRscTimer     = 0x2D8;
inline constexpr std::uint32_t kRscWeight    = 0x2DC;
inline constexpr std::uint32_t kRscSnap      = 0x2E4;
inline constexpr std::uint8_t  kRscTimeInBox    = 3;
inline constexpr std::uint8_t  kRscTimeInSphere = 4;
/// FSceneViewFamily +0x70 (Square Enix): nonzero skips the scene's frame-number step in BeginRenderingViewFamily (the
/// gate `0x1425C9A88`). +0x74 is the FrameNumber the submit writes; this build has no
/// bAdditionalViewFamily (RE-haze-occlusion.md Pass 9 H.3.2).
inline constexpr std::uint32_t kFamilySkipFrameStep = 0x70;
/// APlayerCameraManager::SetGameCameraCutThisFrame (stock 4.18, `or [rcx+1E18h], 400h; ret`): the next draw's views of
/// that player drop their history (RE-haze-history.md §4). The engine clears the bit on every manager after each draw
/// (FViewport::Draw, 0x14330DA82).
inline constexpr std::array<std::uint8_t, 16> kSetGameCameraCutEntry{
    0x81, 0x89, 0x18, 0x1E, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
inline constexpr platform::FunctionFact kSetGameCameraCut{L"APlayerCameraManager::SetGameCameraCutThisFrame",
                                                          0x143719650, kSetGameCameraCutEntry};
/// FDeferredShadingSceneRenderer::ComputeVolumetricFog(renderer, cmdList) (stock 4.18): each view's volumetric fog,
/// after visibility and relevance, so each view's fog-volume list is final when it is called. Called from Render on
/// the early-depth path (0x1421E6C12) and the other (0x1421E6FD0), each `E8 rel32` inside one 16-byte block
/// (RE-haze-occlusion.md, Pass 4).
inline constexpr std::array<std::uint8_t, 16> kComputeVolumetricFogEntry{
    0x40, 0x55, 0x41, 0x55, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x60, 0xFC, 0xFF, 0xFF, 0x48, 0x81};
inline constexpr platform::FunctionFact kComputeVolumetricFog{L"FDeferredShadingSceneRenderer::ComputeVolumetricFog",
                                                              0x14264D410, kComputeVolumetricFogEntry};
inline constexpr std::array<std::uintptr_t, 2> kComputeVolumetricFogCalls{0x1421E6C12, 0x1421E6FD0};
/// FSceneRenderer::Views {FViewInfo* Data, int32 Num} (+0xD0, +0xD8), one FViewInfo every 0x28B0 bytes.
inline constexpr std::uint32_t kRendererViews     = 0xD0;
inline constexpr std::uint32_t kRendererViewCount = 0xD8;
inline constexpr std::uint32_t kViewInfoStride    = 0x28B0;
/// FViewInfo::VolumetricPrimSet {FPrimitiveSceneProxy** Data, int32 Num} (+0x17A8, +0x17B0): the view's fog-volume
/// primitives, filled by relevance from the visibility map after occlusion culling (0x1425CDD51, 0x1425CECAB) and
/// voxelized into that view's fog by VoxelizeFogVolumePrimitives (0x142653660), which skips an entry whose bit in
/// the view's PrimitiveVisibilityMap (TBitArray +0x1430: inline words, heap pointer at +0x1440) is clear.
inline constexpr std::uint32_t kViewFogVolumes        = 0x17A8;
inline constexpr std::uint32_t kViewFogVolumeCount    = 0x17B0;
inline constexpr std::uint32_t kViewVisibilityInline  = 0x1430;
inline constexpr std::uint32_t kViewVisibilityHeap    = 0x1440;
/// FViewInfo flag word (+0x229C). Bit 0x80 = bUsesLightingChannels: set by relevance only when a VISIBLE primitive
/// (after occlusion culling) uses a lighting channel other than 0 (0x1425CE62E); without it, ShouldRenderLight
/// (0x142237760, test at 0x1422377D8) drops every light outside channel 0 from that view's forward light data and
/// volumetric fog. Bit 0x08 = the view ignores old occlusion answers (camera cut, `r.AllowOcclusionQueries 0`)
/// (RE-haze-occlusion.md, Pass 5).
inline constexpr std::uint32_t kViewFlags               = 0x229C;
inline constexpr std::uint32_t kViewUsesLightingChannels = 0x80;
inline constexpr std::uint32_t kViewIgnoresOldAnswers    = 0x08;
/// FPrimitiveSceneProxy: PrimitiveSceneInfo (+0x170, whose packed index is at +0x118), OwnerName and ResourceName
/// (FName, +0x178, +0x180) (RE-haze-occlusion.md, S-RMK-HAZESET).
inline constexpr std::uint32_t kProxySceneInfo       = 0x170;
inline constexpr std::uint32_t kSceneInfoPackedIndex = 0x118;
inline constexpr std::uint32_t kProxyOwnerName       = 0x178;
inline constexpr std::uint32_t kProxyResourceName    = 0x180;
/// FetchVisibilityForPrimitives_Range(params) (stock 4.18): one view's occlusion answers for its primitives. Params
/// +0 = the FScene, +8 = the FViewInfo. It reads each primitive's occlusion flags from FScene::PrimitiveOcclusionFlags
/// (TArray<uint8> +0x1530: data, num +0x1538) at 0x1425D8DBB; bit 0 (CanBeOccluded) clear = never tested, counted
/// visible (0x1425D8FBE). Called once, from FetchVisibilityForPrimitives (0x1425CCF89), which OcclusionCull runs for
/// each view in turn on the render thread; the parallel variant (0x1425D7DB0) is not used (RE-haze-occlusion.md,
/// Pass 4, S-RMK-HAZEOCC).
inline constexpr std::array<std::uint8_t, 16> kFetchVisibilityRangeEntry{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55};
inline constexpr platform::FunctionFact kFetchVisibilityRange{L"FetchVisibilityForPrimitives_Range", 0x1425D8B60,
                                                              kFetchVisibilityRangeEntry};
inline constexpr std::uintptr_t kFetchVisibilityRangeCall  = 0x1425CCF89;
inline constexpr std::uint32_t  kSceneOcclusionFlags       = 0x1530;
inline constexpr std::uint32_t  kSceneOcclusionFlagCount   = 0x1538;
inline constexpr std::uint8_t   kOcclusionCanBeOccluded    = 0x01;
/// FRendererModule::BeginRenderingViewFamily(this, FCanvas*, FSceneViewFamily*) (stock 4.18): renders one family,
/// called once per frame from UGameViewportClient::Draw (0x142F66819, `call [r9+40h]`) with every player's view in it,
/// and by Square Enix's own one-view submit (0x1428D5F90). The renderer copies the family and its views when it starts
/// (0x1425C6A20), so the views may be reordered around the call and put back after (RE-haze-occlusion.md, Pass 9).
inline constexpr std::uintptr_t kRendererModuleVTable = 0x144D655E0;
inline constexpr std::array<std::uint8_t, 16> kBeginRenderingViewFamilyEntry{
    0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x57, 0x41, 0x54, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24};
/// UGameViewportClient::Draw's call of that slot, `call qword ptr [r9+40h]`: the address after it is the return address
/// the replacement sees for the game viewport's family. The menu's own viewport (FUMGViewportClient::Draw 0x1428D5F90)
/// is the slot's only other caller (RE-haze-occlusion.md Pass 10 I.4).
inline constexpr std::uintptr_t                 kDrawSubmitCall = 0x142F66819;
inline constexpr std::array<std::uint8_t, 4>    kDrawSubmitCallBytes{0x41, 0xFF, 0x51, 0x40};
inline constexpr std::uintptr_t                 kDrawSubmitReturn = kDrawSubmitCall + kDrawSubmitCallBytes.size();
inline constexpr platform::SlotFact kBeginRenderingViewFamily{
    L"FRendererModule::BeginRenderingViewFamily (slot 8)", kRendererModuleVTable + 8 * 8,
    {L"FRendererModule::BeginRenderingViewFamily", 0x1425C9770, kBeginRenderingViewFamilyEntry}};
/// FScene::Primitives {FPrimitiveSceneInfo** Data, int32 Num} (+0x14E0); FPrimitiveSceneInfo::Proxy (+8).
inline constexpr std::uint32_t kScenePrimitives      = 0x14E0;
inline constexpr std::uint32_t kSceneInfoProxy       = 0x08;

// --- The game's own input tick (RE-input-p2.md §1) ------------------------------------------------
// AEndPlayerController's constructor turns off the stock per-controller input tick in PlayerTick for gameplay;
// the game ticks only PartyManager+0's input, from its world-tick phase hook (phase 1, start), through a global
// function pointer set at runtime.
/// The game's one input step during play: EndGame_TickFieldSystems (0x1416BF150) calls the primary controller's
/// TickPlayerInput here, at the start of the field step, before that step changes the game-speed slots (the hit
/// stop's end and start, the command menu's slow-down). The right stick's turn is scaled by DeltaTime over the
/// game speed of the moment, so an input step run after those changes reads a mismatched pair
/// (RE-camera-jump-combo.md).
inline constexpr std::uintptr_t kPrimaryInputTickCall  = 0x1416BF1A4;  // E8 -> APlayerController::TickPlayerInput
inline constexpr std::uint32_t  kControllerPlayerInput = 0x498;        // APlayerController::PlayerInput
inline constexpr std::array<std::uint8_t, 16> kTickPlayerInputEntry{
    0x48, 0x8B, 0xC4, 0x56, 0x41, 0x56, 0x48, 0x81, 0xEC, 0xD8, 0x00, 0x00, 0x00, 0x0F, 0x29, 0x70};
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
inline constexpr std::uint32_t  kUiFilterSlot        = 37;  // EndUISystem[37]
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
// The clip states the paint pushes into the entry (64-byte records): the zone's corners in the paint's absolute
// pixels (top-left, then bottom-right at +0x20), a flags byte (bit 0: axis aligned), a byte saying the state has a
// rect of its own, and a stack of 36-byte zones whose last one is used instead when the byte is clear. The renderer's
// scissor is the entry rect's Min plus that rect (FEndMenuRenderer__DrawEntryBatchToTarget 0x1439FD100).
inline constexpr std::uint32_t kEntryClipStates        = 0xB0;  // clip state* (64 bytes each)
inline constexpr std::uint32_t kEntryClipStateCount    = 0xB8;  // int32
inline constexpr std::uint32_t kClipStateSize          = 0x40;
inline constexpr std::uint32_t kClipStateTopLeft       = 0x08;  // float x, y
inline constexpr std::uint32_t kClipStateBottomRight   = 0x20;  // float x, y
inline constexpr std::uint32_t kClipStateFlags         = 0x28;  // uint8: bit 0 axis aligned
inline constexpr std::uint32_t kClipStateOwnRect       = 0x2C;  // uint8: the corners above apply (else the stack's last zone)
inline constexpr std::uint32_t kClipStateStack         = 0x30;  // zone* (kClipZoneSize bytes each)
inline constexpr std::uint32_t kClipStateStackCount    = 0x38;  // int32
inline constexpr std::uint32_t kClipZoneSize           = 36;
inline constexpr std::uint32_t kClipZoneTopLeft        = 0x00;  // float x, y
inline constexpr std::uint32_t kClipZoneBottomRight    = 0x18;  // float x, y
inline constexpr std::uint32_t kClipZoneFlags          = 0x20;  // uint8
// The entry's layers: a sparse array (the stock 4.18 shape: the records, an inline bit array of four words with a secondary
// pointer, the bit count) of 40-byte records holding the layer at +0x10; a layer's draw elements are 304-byte records
// (FEndMenuLayerEntry__AllocElement 0x143A080E0, FEndMenuElement__InitCommon 0x143A06EB0): the texture mode (1 binds the
// entry's offscreen target as the second texture), the draw flags (bit 2 renders into that target), a pixel-shader
// vector, the type (4: custom vertices) and the indices of the clip state and the per-child matrix the element draws with.
inline constexpr std::uint32_t kEntryCurrentLayer      = 0x90;   // layer*: the layer the paint adds to (the root overlay's one)
inline constexpr std::uint32_t kEntryLayers            = 0xE0;   // record* (40 bytes each): further layers, empty for the overlay
inline constexpr std::uint32_t kEntryLayerBitsInline   = 0xF0;   // uint32[4]
inline constexpr std::uint32_t kEntryLayerBitsSecondary = 0x100; // uint32* (used when not null)
inline constexpr std::uint32_t kEntryLayerBitCount     = 0x108;  // int32
inline constexpr std::uint32_t kLayerRecordSize        = 40;
inline constexpr std::uint32_t kLayerRecordLayer       = 0x10;   // layer*
inline constexpr std::uint32_t kLayerElements          = 0x1078; // element* (304 bytes each)
inline constexpr std::uint32_t kLayerElementCount      = 0x1080; // int32
inline constexpr std::uint32_t kElementSize            = 0x130;
inline constexpr std::uint32_t kElementTextureMode     = 0x30;   // int32
inline constexpr std::uint32_t kElementDrawFlags       = 0x34;   // int32: bit 2 = render into the entry's offscreen target
inline constexpr std::uint32_t kElementShaderVector    = 0x40;   // float x, y, z, w
inline constexpr std::uint32_t kElementType            = 0x50;   // int32: 4 = custom vertices
inline constexpr std::uint32_t kElementMatrixIndex     = 0x58;   // int32 into the entry's per-child matrices, -1 for the entry's
// The area map (SEndNaviMap__PaintChildren_PushOrthoVP 0x1414FB580) draws each 256-unit tile twice: the region's shape into
// the widget's mask target under a per-child matrix of its own (flags bit 2), then the map texture quad on screen with the
// mask bound (texture mode 1), its shader vector x and y the tile's origin in the paint's absolute pixels and z one over
// the tile's pixel size (w the entry's own value), locating the mask under the pixel's position on the target
// (research/RE-hud-band-clip.md).
inline constexpr std::int32_t  kElementTypeCustomVerts = 4;
inline constexpr std::int32_t  kElementTextureModeMask = 1;
inline constexpr std::int32_t  kElementDrawToTarget    = 4;
inline constexpr std::uintptr_t kLayerSubmitCall       = 0x14150A1EC;  // call SubmitLayerSlot: crosses a block
inline constexpr std::array<std::uint8_t, 16> kSubmitLayerSlotEntry{
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x50};
/// (renderer, pool, slot)
inline constexpr platform::FunctionFact kSubmitLayerSlot{L"SubmitLayerSlot", 0x1439FC750, kSubmitLayerSlotEntry};

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
/// The UI particles (the summon gauge's glow and every other UI effect) drawn per view into the view's UI image, right
/// after the HUD delegate in Render's UI loop (RE-summon-gauge.md §2.4, §10): SceneRenderer__DrawInGameUIPrimitivesForView
/// (renderer, view, RHICmdList), its call inside one block, under whatever viewport is current (AllocBind's whole image,
/// which the HUD's own layer draws rely on, so that one is left alone and the particles get a viewport of their own).
inline constexpr std::uintptr_t kUiPrimitivesDrawCall = 0x1421E8330;  // E8 0B 4E 44 00
inline constexpr std::uintptr_t kUiPrimitivesDraw     = 0x14262D140;
/// The UI image's size as AllocBind reads it for its viewport every time (FSceneRenderTargets +0x240/+0x244).
inline constexpr std::uint32_t kSceneTargetsUiWidth  = 0x240;
inline constexpr std::uint32_t kSceneTargetsUiHeight = 0x244;
/// FRHICommandListBase as this build lays it out and FRHICommandSetViewport as AllocBind appends it (RE-summon-gauge.md
/// §10.1): the list's link to the last command's Next (+0x08), the command count (+0x14), the memstack's Top and End
/// (+0x30/+0x38; a new chunk from FMemStackBase__AllocateNewChunk(stack, size) when a command does not fit); a command is
/// Next (+0x00) then its execute function (+0x08), the viewport's MinX, MinY (u32), MinZ (float), MaxX, MaxY (u32), MaxZ
/// (float) at +0x10..+0x24, 0x28 bytes, 8-aligned. A viewport's MinX/MinY are unsigned at the RHI: never negative.
inline constexpr std::uint32_t  kRhiListCommandLink   = 0x08;
inline constexpr std::uint32_t  kRhiListCommandCount  = 0x14;
inline constexpr std::uint32_t  kRhiListStackTop      = 0x30;
inline constexpr std::uint32_t  kRhiListStackEnd      = 0x38;
inline constexpr std::uint32_t  kRhiCommandExecute    = 0x08;
inline constexpr std::uint32_t  kRhiViewportFields    = 0x10;
inline constexpr std::uint32_t  kRhiCommandSize       = 0x28;
inline constexpr std::uint32_t  kRhiChunkRequest      = 0x30;  // what AllocBind asks the memstack for a viewport command
inline constexpr std::uintptr_t kRhiSetViewportExecute = 0x141F09590;  // FRHICommandSetViewport's execute (context vt+0x168)
inline constexpr std::uintptr_t kMemStackAllocateChunk = 0x141CB8DA0;  // FMemStackBase__AllocateNewChunk(stack, minSize)
/// A viewport whose corner is above or left of the image (RE-ui-particle-negative-viewport.md): no command type takes
/// one, but the command context's RHISetMultipleViewports (vt+0xB8: (ctx, u32 count, six floats per viewport, the
/// D3D12_VIEWPORT layout) hands floats unclamped to the state cache, and RHISetScissorRect (vt+0x178: (ctx, bool,
/// MinX, MinY, MaxX, MaxY)) sets the scissor on its own. The context is the list's +0x20 when a command executes. The
/// known contexts: D3D12's, D3D12's multi-GPU redirector, D3D11's (-d3d11 only), each slot's function as read.
inline constexpr std::uint32_t kRhiListContext          = 0x20;
inline constexpr std::uint32_t kRhiContextSetViewports  = 0xB8;
inline constexpr std::uint32_t kRhiContextSetScissor    = 0x178;
inline constexpr std::uint32_t kRhiFloatViewportSize    = 0x38;  // Next, execute, six floats, the scissor's four u32
inline constexpr std::uint32_t kRhiFloatViewportRequest = 0x40;  // the memstack request, as the game asks size + 8
struct RhiContextFact
{
    std::uintptr_t vtable, setViewports, setScissor;
};
inline constexpr std::array<RhiContextFact, 3> kRhiContexts{{
    {0x144D3AA68, 0x141F87000, 0x141F64210},  // FD3D12CommandContext
    {0x144D3A690, 0x141F5B4E0, 0x141F5B6D0},  // FD3D12CommandContextRedirector
    {0x144D39408, 0x141F3EE20, 0x141F31390},  // FD3D11DynamicRHI
}};
/// The UI effects' layout-point conversion (UIEffect_LayoutPointToEffectSpace 0x1413FD2A0) reads the overlay's UI size
/// twice through EndUI__GetOverlayUISize(FVector2D* out) (returns out; the overlay's +0x398 pair, else the in-game UI
/// rect's size): both calls inside one block. Answered with the 1920x1080 frame while the split's band applies, so
/// every effect is converted in one layout (an effect set once, in whichever pass, included).
inline constexpr std::uintptr_t kEffectUiSizeCallOne = 0x1413FD2C0;  // E8 FB B4 F4 FF
inline constexpr std::uintptr_t kEffectUiSizeCallTwo = 0x1413FD2F6;  // E8 C5 B4 F4 FF
inline constexpr std::array<std::uint8_t, 16> kOverlayUiSizeGetterEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x1D, 0x37, 0x19, 0x51};
inline constexpr platform::FunctionFact kOverlayUiSizeGetter{L"EndUI__GetOverlayUISize", 0x1413487C0, kOverlayUiSizeGetterEntry};
inline constexpr std::uintptr_t kSceneRenderTargetsGlobal = 0x145923AD0; // FSceneRenderTargets (static object)
inline constexpr std::uint32_t  kSceneTargetsInGameUI    = 0x110;        // TRefCountPtr<IPooledRenderTarget>
/// The movie frame for every view (research/RE-movie-view.md §2.5, reviewed in IDA): FPostProcessing::Process
/// ends with an unconditional release of the composite source FSceneRenderTargets+0x120 (the media texture's frame,
/// set once per frame; its only code caller), so the second view's final pass binds the black fallback. The call is
/// redirected (rcx = &FSceneRenderTargets) and deferred to the family's last view, the game's own guard for its other
/// per-family target at 0x14251DCCE.
inline constexpr std::uintptr_t kCompositeReleaseCall    = 0x14251E168;  // E8 43 A5 FF FF, inside block 0x14251E160
inline constexpr std::array<std::uint8_t, 16> kCompositeReleaseEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x99, 0x20, 0x01, 0x00};
inline constexpr platform::FunctionFact kCompositeRelease{L"SceneRenderTargets__ReleaseCompositeHDR_120", 0x1425186B0,
                                                          kCompositeReleaseEntry};
inline constexpr std::uint32_t  kSceneTargetsCompositeHdr = 0x120;       // TRefCountPtr<FRHITexture>: the movie frame

// The further players' HUD images (RE-hud-overlay-pass.md §2, §3, §4; RE-hud-two-images.md §2.3, §3).
inline constexpr std::array<std::uint8_t, 16> kOverlayRenderEntry{
    0x4C, 0x8B, 0xDC, 0x55, 0x41, 0x56, 0x49, 0x8D, 0xAB, 0x68, 0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC};
inline constexpr platform::FunctionFact kOverlayRender{L"SEndRootOverlay__RenderToInGameUILayer", 0x141509D00,
                                                       kOverlayRenderEntry};
inline constexpr std::uintptr_t kOverlayDelegateSlot  = 0x144BF4DB0;  // the render delegate's vtable slot (+0x60)
inline constexpr std::uintptr_t kOverlayDelegateThunk = 0x14151BAF0;  // what it holds: render [this+8] if any
inline constexpr std::uintptr_t kOverlayAcquireCall   = 0x141509FCF;  // call AcquireLayerSlot (pool 2): crosses a block
inline constexpr std::uintptr_t kAppDeltaTime         = 0x1453A49B0;  // FApp::DeltaTime (double): the paint's widget ticks
/// GFrameNumber (u32; RE-damage-numbers-per-view.md; the name and the one increment a frame by its use [ASSUMPTION]),
/// read by the HUD widgets
/// that animate themselves (damage numbers, battle sub-texts, UEndUserWidget): a paint stamps it, and the post-paint
/// catch-up advances by the frame's step every widget whose stamp is not this frame's (one the view did not draw). The
/// engine itself takes it back by one around an extra view family it renders (0x1428D6122).
inline constexpr std::uintptr_t kFrameNumber          = 0x1453A4A00;
inline constexpr std::uintptr_t kDrawEntryBatchCall   = 0x1439FCFD8;  // DrawLayerEntry: call DrawEntryBatchToTarget
inline constexpr std::uintptr_t kDrawEntryBatchToTarget = 0x1439FD100;  // 9 arguments, no result
/// One render buffer as FEndMenuRenderer's constructor makes pool 2's: 64 bytes, its vtable at +0, a u32 the constructor
/// sets to 4 at +8 [UNKNOWN: its meaning], the capacity at +0x38; InitResource is the vtable's +0x28. A layer entry's
/// own batches hang at +0x80, and the draw call is given that pointer + 0x50 (RE-hud-two-images.md).
inline constexpr std::uint32_t kRenderBufferBytes    = 64;
inline constexpr std::uint32_t kRenderBufferWord8    = 0x08;
inline constexpr std::uint32_t kRenderBufferCapacity = 0x38;
inline constexpr std::uint32_t kRenderBufferInitSlot = 0x28;
inline constexpr std::uint32_t kLayerEntryBatches    = 0x80;
inline constexpr std::uint32_t kBatchesDrawn         = 0x50;
inline constexpr std::uintptr_t kGMallocGlobal        = 0x145825680;  // FMalloc* GMalloc (vt+0x10 Malloc(size, align))
inline constexpr std::uint32_t  kMallocSlotMalloc     = 0x10;         // FMalloc's vtable: Malloc(size, alignment)
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
inline constexpr std::array<std::uint8_t, 7> kPlayerContextControllerFirst{0x44, 0x8B, 0x41, 0x0C, 0x48, 0x8B, 0xD1};
inline constexpr platform::FunctionFact kPlayerContextController{L"FLocalPlayerContext::GetPlayerController", 0x143016CF0,
                                                                 kPlayerContextControllerFirst};
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
// The enemy bar's class per column (RE-enemy-bar-two-content.md, RE-enemy-bar-two-classification.md; hud/bar_class.h).
// The slot-27 tick classifies each bar once with player 1's displayed target; a further column's pass classifies again
// with its own and sets what follows the class through the game's own setters: the bar's and three sub-widgets'
// visibility (stock UWidget::SetVisibility, repeatable), and the status icons' refresh for a bar only that column
// targets (the game's own once-per-frame call, which the tick made only for player 1's targets).
inline constexpr std::array<std::uint8_t, 16> kWidgetSetVisibilityEntry{
    0x40, 0x53, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x0F, 0xB6, 0xDA, 0x88, 0x91, 0xB7, 0x00};
inline constexpr platform::FunctionFact kWidgetSetVisibilityFn{L"UWidget__SetVisibility", 0x1428D7FC0, kWidgetSetVisibilityEntry};
inline constexpr std::array<std::uint8_t, 16> kWidgetGetVisibilityEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8D, 0x54, 0x24, 0x20, 0x48};
inline constexpr platform::FunctionFact kWidgetGetVisibilityFn{L"UWidget__GetVisibility", 0x1428D7F00, kWidgetGetVisibilityEntry};
inline constexpr std::array<std::uint8_t, 16> kWidgetUpdateRenderTransformEntry{
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0xA9, 0x48, 0x81, 0xEC, 0xF0, 0x00};
inline constexpr platform::FunctionFact kWidgetUpdateRenderTransform{L"UWidget__UpdateRenderTransform", 0x1428D7840,
                                                                     kWidgetUpdateRenderTransformEntry};
inline constexpr std::array<std::uint8_t, 16> kBattleUnitIsTargetableEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20};
inline constexpr platform::FunctionFact kBattleUnitIsTargetable{L"BattleComp_Vt008_IsTargetable", 0x140994980,
                                                                kBattleUnitIsTargetableEntry};  // (unit, 1, 0) -> bool
inline constexpr std::array<std::uint8_t, 16> kBarStatusIconsUpdateEntry{
    0x48, 0x85, 0xD2, 0x0F, 0x84, 0x9C, 0x08, 0x00, 0x00, 0x53, 0x41, 0x56, 0x48, 0x81, 0xEC, 0xC8};
inline constexpr platform::FunctionFact kBarStatusIconsUpdate{L"EndUISlot27_EnemyBar_StatusIcons_Update", 0x1414A7B30,
                                                              kBarStatusIconsUpdateEntry};  // (p+0x40 block, character)
inline constexpr std::array<std::uint8_t, 16> kMainTargetOfLockOnEntry{
    0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0x3A, 0x00, 0x48, 0x8B};
inline constexpr platform::FunctionFact kMainTargetOfLockOn{L"EndUI_GetMainTargetOfLockOnTarget", 0x1413DA690,
                                                            kMainTargetOfLockOnEntry};  // (weak pair* out, const weak pair* key)
inline constexpr std::array<std::uint8_t, 16> kGameStateDtAlphaEntry{
    0x48, 0x83, 0xEC, 0x38, 0x48, 0x8B, 0x0D, 0x35, 0xD4, 0x40, 0x04, 0x0F, 0x29, 0x74, 0x24, 0x20};
inline constexpr platform::FunctionFact kGameStateDtAlpha{L"EndUI_GameStateDtAlpha", 0x1413DC670, kGameStateDtAlphaEntry};  // (float k) -> min(k*dt, 1)
inline constexpr std::array<std::uint8_t, 16> kWeakPtrReleaseEntry{
    0x48, 0x8B, 0x49, 0x08, 0x48, 0x85, 0xC9, 0x74, 0x20, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0x0F};
inline constexpr platform::FunctionFact kWeakPtrRelease{L"TWeakPtrTS_Release", 0x140657A90, kWeakPtrReleaseEntry};  // (weak pair*): one weak reference off
// The bar manager's entries (0x150 bytes: +0 source, +8 its weak controller, p = +0x10) and the bar's objects.
inline constexpr std::uint32_t kBarEntrySize        = 0x150;
inline constexpr std::uint32_t kBarManagerEntries   = 0x08;   // the manager's set: its elements' data
inline constexpr std::uint32_t kBarManagerEntryBits = 0x30;   //   and NumBits
inline constexpr std::uint32_t kBarEntryP           = 0x10;
inline constexpr std::uint32_t kBarEntryNameText    = 0xE8;   // p: weak Txt_EnemyName
inline constexpr std::uint32_t kBarEntryStatusPanel = 0xF0;   // p: weak Pnl_Icon_Status
inline constexpr std::uint32_t kBarEntryLinkage     = 0xD0;   // p: weak CommandLinkage (the stagger bonus icon)
inline constexpr std::uint32_t kBarEntryStatusIcons = 0x40;   // p: the status icons' block (TArray of 0x20 slots)
inline constexpr std::uint32_t kBarEntryShowFlag    = 0x12D;  // p: u8, the bar shown whoever targets (a burst began)
inline constexpr std::uint32_t kBarEntryPoint       = 0xB4;   // p: the cached screen point (x, y)
inline constexpr std::uint32_t kBarEntryEase        = 0x110;  // p: the displayed target's y-ease toward -60
inline constexpr std::uint32_t kBarStatusSlotSize   = 0x20;   // a status icon slot: +0 weak widget
inline constexpr std::uint32_t kWidgetRenderTranslation  = 0x90;  // UWidget::RenderTransform.Translation (x, y)
inline constexpr std::uint32_t kWidgetRenderTranslationY = 0x94;  // UWidget::RenderTransform.Translation.Y
inline constexpr std::uint32_t kWidgetRenderScale        = 0x98;  // UWidget::RenderTransform.Scale (x, y)
inline constexpr std::uint32_t kSourceCharacterSlot = 0x20;   // source->vt: the character (AEndCharacter) of a bar's source
inline constexpr std::uint32_t kSourceIsPartSlot    = 0x68;   // source->vt: a part source (no status panel, no bonus icon)
inline constexpr std::uint32_t kCharacterUnit       = 0xCB0;  // AEndCharacter: its battle unit
inline constexpr std::uint32_t kCharacterHalfSizeObject = 0x800;  // AEndCharacter: the object whose vt+0x118 gives the bar clamp's half size
inline constexpr std::uint32_t kHalfSizeSlot        = 0x118;
inline constexpr std::uint32_t kCharacterIsDownedSlot = 0x8B0;  // AEndCharacter vt
inline constexpr std::uint32_t kUnitBonusActiveSlot = 0x798;  // battle unit vt: the stagger bonus is showing
inline constexpr std::uint32_t kUnitKeepBarFlag     = 0x543;  // battle unit: u8, a script keeps the bar of an untargetable unit
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

// The field objective and item icons (slot 51, RE-field-icon-placement.md §2.2-§2.3, §5.1): one stateless placement call
// per icon, projecting with the controller it is given; its first three instructions (5 bytes, position-independent, no
// branch lands inside: IDA) are copied, so the redirect is installed from the game thread, outside the function.
inline constexpr std::uintptr_t kFieldIconPlace = 0x1414223E0;  // (entry, canvas, float dt, pc, const FVector* leader, int, const FVector* icon)
inline constexpr std::array<std::uint8_t, 5> kFieldIconPlaceFirst{0x48, 0x8B, 0xC4, 0x55, 0x53};
inline constexpr std::uintptr_t kOverlayProject = 0x141348940;  // bool EndUI__ProjectWorldToOverlay(pc, const FVector*, FVector2D* out)
inline constexpr std::array<std::uint8_t, 6> kOverlayProjectFirst{0x40, 0x57, 0x48, 0x83, 0xEC, 0x70};
inline constexpr std::uint32_t kFieldIconSlot = 51;  // EndUISystem[51]: +0 the one SEndFieldQuestIconCanvas, +0x10 the icon map
// The icon map, TMap<FName, TUniquePtr<Entry>>: its sparse array (platform/sparse_array.h) at +0x10; 24 bytes per
// element, the entry pointer at +8.
inline constexpr std::uint32_t kFieldIconMapData = 0x10, kFieldIconMapElement = 24, kFieldIconMapElementEntry = 8;
inline constexpr std::uint32_t kFieldIconMapElementKey = 0;  // the navi icon's FName, slot 29's key (RE-field-icon-placement.md §2.2)
inline constexpr std::uint32_t kFieldIconEntryHasSlot = 28, kFieldIconEntryAlpha = 32;  // the entry's has-slot byte; its alpha, the only state the placement carries
/// The diamond's image set (FieldQuestIcon_C's `Ims_QuestIcon`, a UEndImageSet; research/RE-lead-marker-size.md): its
/// UMG Size (+0x134 x, +0x138 y; 24 x 24 at the widget's creation, read every frame by the placement and written by nothing
/// per frame), its Slate widget (+0x150), and the Slate size setter the creation calls (slate, FVector2D by value), found
/// in the widget's tree by the game's own name global. [CONFIRMED code]
inline constexpr std::uint32_t  kImageSetSize        = 0x134;
inline constexpr std::uint32_t  kImageSetSlate       = 0x150;
inline constexpr float          kQuestIconSize       = 24.0f;
inline constexpr std::uintptr_t kNameImsQuestIcon    = 0x145856F00;  // FName "Ims_QuestIcon"
inline constexpr std::array<std::uint8_t, 16> kImageSetSetSizeEntry{
    0x48, 0x83, 0xEC, 0x28, 0x66, 0x48, 0x0F, 0x6E, 0xCA, 0x0F, 0x2E, 0x89, 0x08, 0x04, 0x00, 0x00};
inline constexpr platform::FunctionFact kImageSetSetSize{L"SEndImageSet__SetImageSize", 0x1414F0060, kImageSetSetSizeEntry};
inline constexpr std::uint32_t kCanvasChildren   = 0x160;  // SEndCanvas vtable: its children (+0 vtable: count; +8 slot pointers)
inline constexpr std::uint32_t kCanvasSlotWidget = 0x08, kCanvasSlotOffset = 0x20;  // a slot's SWidget, its Offset {x, y, w, h}

// Each further player's own navi map manager (EndUISystem[29], RE-hud-minimap-two.md §2-§6, §9). Call sites are checked by
// platform::CallRedirect against their targets.
inline constexpr std::uint32_t  kNaviSlot              = 29;           // EndUISystem[29]: the game's manager
inline constexpr std::uint32_t  kBannerSlot            = 35;           // EndUISystem[35]: the banner fades' lists +0x10, +0x80
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
inline constexpr std::array<std::uint8_t, 16> kSetColorAndOpacityEntry{
    0x40, 0x53, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05, 0x98, 0xEE, 0xAA, 0x02};
/// UUserWidget (widget, const FLinearColor&)
inline constexpr platform::FunctionFact kSetColorAndOpacity{L"UUserWidget::SetColorAndOpacity", 0x1428F1C20,
                                                            kSetColorAndOpacityEntry};
inline constexpr std::uintptr_t kCharacterByPlayerType = 0x1413B4C40;  // (u8 type): the party's character of that type; an RE fact kept, with no user here (the scopes carry the member type)
/// The manager's icon store (a TSparseArray of 152-byte elements, research/RE-lead-marker.md): the key at +0, the
/// minimap pin (a raw Slate SEndMapIcon) at +0x10. The game type-checks a pin by its Slate type name (+0x1D0) against a
/// name global, then sets its per-icon scale (icon, FVector2D by value), which the navi tick writes again every frame
/// (research/RE-lead-marker-size.md). [CONFIRMED code]
inline constexpr std::uint32_t  kNaviIconStore         = 0xD8;
inline constexpr std::uint32_t  kNaviIconBytes         = 0x98;
inline constexpr std::uint32_t  kNaviIconPin           = 0x10;
inline constexpr std::uint32_t  kSlateTypeName         = 0x1D0;
inline constexpr std::uintptr_t kNameSEndMapIcon       = 0x145857D20;  // FName "SEndMapIcon"
inline constexpr std::array<std::uint8_t, 16> kMapIconSetScaleEntry{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x70, 0x20, 0x57, 0x48, 0x81, 0xEC, 0xA0};
inline constexpr platform::FunctionFact kMapIconSetScale{L"SEndMapIcon_SetIconScale", 0x1414F5D70, kMapIconSetScaleEntry};
inline constexpr std::uint32_t  kNaviManagerRoot       = 0x08;   // weak NaviMap_C
inline constexpr std::uint32_t  kNaviManagerOctagon    = 0x18;   // weak Nav_Map (UEndNaviMapOctagon)
inline constexpr std::uint32_t  kNaviManagerFlags      = 0x180;  // hidden, visible, enabled, auto-rotate; +0x188 zoom
inline constexpr std::uint32_t  kNaviManagerMapData    = 0x1B4;  // the map data (a pointer at a 4-aligned offset)
inline constexpr std::uint32_t  kNaviManagerModes      = 0x258;  // navi-map mode, compass mode, pending
inline constexpr std::uint32_t  kNaviManagerCompass    = 0x260;  // the compass object (+0: weak Compass_C)
/// The compass object (research/RE-lead-marker-size.md): its icon map at +0x08 (a TMap whose sparse array holds 32-byte
/// elements, the navi store's key at +0 and the entry at +8; the entry's weak Icon_NaviMap_C at +0), and its shown
/// byte. The compass tick, the manager tick's last call, writes the icon image set's (`All_Nut_Icon_NaviMap`, found by
/// the game's name global) render scale every frame. [CONFIRMED code]
inline constexpr std::uint32_t  kCompassIcons          = 0x08;
inline constexpr std::uint32_t  kCompassIconBytes      = 0x20;
inline constexpr std::uint32_t  kCompassIconEntry      = 0x08;
inline constexpr std::uint32_t  kCompassShown          = 0x58;
inline constexpr std::uintptr_t kNameAllNutIconNaviMap = 0x145856560;  // FName "All_Nut_Icon_NaviMap"
// Read only for the `hud.minimap` description:
inline constexpr std::uint32_t  kNaviManagerPlayerIcon = 0x20;   // weak Icon_Player
inline constexpr std::uint32_t  kNaviManagerAnimation  = 0x18C;  // int
inline constexpr std::uint32_t  kNaviManagerFade       = 0x198;  // int
inline constexpr std::uint32_t  kNaviManagerAlpha      = 0x1A0;  // float
inline constexpr std::uint32_t  kNaviManagerRotation   = 0x1A4;  // float x2
inline constexpr std::uint32_t  kWidgetRenderAngle     = 0xA8;   // UWidget::RenderTransform.Angle (Icon_Player: the facing)
inline constexpr std::uint32_t  kOctagonRenderTexture  = 0x118;  // UEndNaviMapOctagon::RenderTexture
inline constexpr std::uint32_t  kOctagonSlate          = 0x128;  // its Slate object
inline constexpr std::uint32_t  kOctagonSlateTarget    = 0x5C8;  // the Slate object's render target (its entries draw into it)
inline constexpr std::uint32_t  kOctagonPaintInterface = 0x330;  // the Slate object's paint interface (rsi in the paint)
inline constexpr std::uint32_t  kPartyPrimaryController = 0x00;  // AEndPartyManager: the primary controller
inline constexpr std::uint32_t  kPartySlotTypes        = 0x230;  // u8[3]: the deployed members' player types by ordinal
inline constexpr std::uint32_t  kPartyLeaderType       = 0x233;  // the leader's player type byte
inline constexpr std::uint8_t   kPlayerTypeNone        = 9;      // EPlayerType::NONE (an empty row, no member)
inline constexpr std::uint8_t   kPlayerTypeCloud = 0, kPlayerTypeBarret = 1, kPlayerTypeTifa = 2;  // the three with the switch-board animation pack

// ULocalPlayer's split-screen view as fractions of the viewport (stock 4.18, not reflected: between ViewportClient
// +0x58 and LastViewLocation, before AspectRatioAxisConstraint +0x7C).
inline constexpr std::uint32_t kLocalPlayerOrigin      = 0x60;  // FVector2D
inline constexpr std::uint32_t kLocalPlayerSize        = 0x68;  // FVector2D
inline constexpr std::uint32_t kLocalPlayerViewportClient = 0x58;  // ULocalPlayer::ViewportClient (UGameViewportClient*)
inline constexpr std::uint32_t kLocalPlayerAxisRule       = 0x7C;  // ULocalPlayer::AspectRatioAxisConstraint (u8)
inline constexpr std::uint8_t  kAxisRuleMaintainXFov      = 1;     // EAspectRatioAxisConstraint::AspectRatio_MaintainXFOV

// The view point (stock 4.18 FMinimalViewInfo; GetProjectionData passes LP+0x7C, the viewport through LP+0x58 -> +0xA0,
// and FMinimalViewInfo::CalculateProjectionMatrixGivenView 0x142DCDB10 reads these; research/RE-view-size-fov.md §4).
inline constexpr std::uint32_t kViewInfoFov            = 0x18;  // float, degrees (horizontal for the game's cameras)
inline constexpr std::uint32_t kViewInfoAspectRatio    = 0x28;  // float, the camera's aspect (16:9 for the game's)
inline constexpr std::uint32_t kViewInfoFlags          = 0x2C;  // bit 0 bConstrainAspectRatio
inline constexpr std::uint8_t  kViewInfoConstrainBit   = 0x01;
/// ULocalPlayer's vtable (vtbl_ULocalPlayer); slot 72 GetViewPoint(this, FMinimalViewInfo& out, EStereoscopicPass),
/// called by GetProjectionData (`call [rax+240h]` 0x14301946A) and CalcSceneView.
inline constexpr std::uintptr_t kLocalPlayerVTable     = 0x144E6D668;
inline constexpr std::array<std::uint8_t, 16> kGetViewPointEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xF2, 0x48};
inline constexpr platform::SlotFact kGetViewPoint{L"ULocalPlayer::GetViewPoint (slot 72)", kLocalPlayerVTable + 72 * 8,
                                                  {L"ULocalPlayer::GetViewPoint", 0x1430185F0, kGetViewPointEntry}};

// The camera in a narrow view (research/RE-narrow-view-camera.md). The operator's Tick turns the pose's aim offset into a
// world point at one call, inside one aligned block; the battle's TargetOffset flavor projects with window pixels at a call
// that crosses into the next block.
inline constexpr std::array<std::uint8_t, 16> kRotateVectorEntry{
    0x48, 0x8B, 0xC4, 0x48, 0x81, 0xEC, 0x98, 0x00, 0x00, 0x00, 0xF3, 0x41, 0x0F, 0x10, 0x08, 0x4C};
inline constexpr platform::FunctionFact kRotateVector{L"FRotator::RotateVector", 0x141C8E110, kRotateVectorEntry};
inline constexpr std::uintptr_t kOperatorAimRotateCall = 0x140FDEA58;
inline constexpr std::uint32_t  kOperatorAimOffset     = 0x4E4;  // AEndFieldCameraOperator: this frame's pose +0x44, lagged; sideways +4
inline constexpr std::array<std::uint8_t, 16> kProjectWorldToScreenEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
inline constexpr platform::FunctionFact kProjectWorldToScreen{L"UGameplayStatics::ProjectWorldToScreen", 0x142F5A3F0,
                                                              kProjectWorldToScreenEntry};
inline constexpr std::uintptr_t kTargetLeanProjectCall = 0x1410C0ABC;
inline constexpr std::uint32_t  kControllerPlayer      = 0x3E0;  // APlayerController::Player (Engine.hpp)

// UMG, stock 4.18 (UMG.hpp) and UEndUserWidget's own fields (EndGame.hpp, UEndUserWidget__AddToRootOverlay).
inline constexpr std::uint32_t kWidgetVisibility       = 0xB7;   // UWidget::Visibility (ESlateVisibility)
inline constexpr std::uint32_t kWidgetRenderPivot      = 0xAC;   // UWidget::RenderTransformPivot (x, y)
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
inline constexpr std::uint32_t kCommandCtrlSlot       = 19;     // EndUISystem[19]: the field command controller

// --- Summons for players 2 and 3 (research/RE-summon-seats.md) ----------------------------------------------------
// The party's one summon gauge lives in the summon manager; a command controller asks whether it may offer Summon (and,
// with a summon on the field, its commands) through two readers of the controller's own summon gauge widget (weak at
// ctrl+0x520, bytes +0x36C shown and +0x36D on the field). The manager makes and sets that widget on player 1's
// controller only, so a seat's controller always reads 0. Both readers begin with the same 7-byte position-independent
// instruction, `mov r8d, [rcx+524h]`, inside one aligned block: their entry bytes match, and the address tells them apart.
inline constexpr std::array<std::uint8_t, 16> kSummonGaugeReaderEntry{
    0x44, 0x8B, 0x81, 0x24, 0x05, 0x00, 0x00, 0x45, 0x85, 0xC0, 0x74, 0x79, 0x48, 0x63, 0x91, 0x20};
inline constexpr std::size_t kSummonGaugeReaderFirstBytes = 7;
inline constexpr platform::FunctionFact kSummonGaugeShown{L"FieldCommandCtrl_IsSummonGaugeShown_W520_36C", 0x14141F690,
                                                           kSummonGaugeReaderEntry};
inline constexpr platform::FunctionFact kSummonOnField{L"FieldCommandCtrl_IsSummonOnField_W520_36D", 0x14141F7C0,
                                                        kSummonGaugeReaderEntry};
// The summon manager (a plain game object, the global holds a pointer to it, or 0 outside a battle with summons): its
// state (u8; views/whole_screen_rules.h names the values) and the event lock it takes for a summon's call
// (SummonMgr_EventLock_Take_3AC 0x1409E5410, the only writer of a nonzero value; released by 0x1409E54B0).
inline constexpr std::uintptr_t kSummonManager       = 0x1457E9748;
inline constexpr std::uint32_t  kSummonManagerState  = 0x2B0;
inline constexpr std::uint32_t  kSummonManagerEvent  = 0x3AC;

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
inline constexpr std::uint32_t kCommandCtrlWaitState     = 0x418;  // i32 (low byte read): the menu's wait (slow, tint)
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
/// The wait state's values (FieldCommandCtrl__WaitModeFilter_LatchRef 0x141419C20: 0 -> 1 -> 5 -> 4 -> 0).
inline constexpr std::uint8_t kCommandWaitOpening = 1;
inline constexpr std::uint8_t kCommandWaitOpen    = 5;

// --- The open battle menu's grey: UI filter 0, the wait tint (research/RE-menu-grey.md) -----------------------------
// The UI filter manager EndUISystem[37] (`EUI+0x128`, kUiFilterSlot; its counts and suppressed bytes above): effect handle per type at +0x00 (i32, -1 = none), the latch
// at +0x18 (u8 per type), suppressed at +0x1E (u8 per type), the shared count at +0x24 (i32 per type). Type 0 = the
// wait tint (FX_R_WaitFilter_01). EndUIFilter__ActivateType(fm, u8 type, u8 instant), EndUIFilter__DeactivateType(fm,
// u8 type) (returns at once when the type's handle is -1). The wait's spawn call (count 0 -> 1, not suppressed), and
// the four calls that bring a counted filter back after a menu closes over it (the top/pause menu, menu widgets A and
// B, EndUIFilter__Unsuppress); every one an E8 inside one 16-byte block.
inline constexpr std::uint32_t  kUiFilterHandles    = 0x00;
inline constexpr std::uint8_t   kUiFilterWait       = 0;
inline constexpr std::array<std::uint8_t, 16> kUiFilterActivateEntry{
    0x4C, 0x8B, 0xDC, 0x55, 0x53, 0x56, 0x57, 0x41, 0x57, 0x49, 0x8D, 0xAB, 0x08, 0xFF, 0xFF, 0xFF};
inline constexpr platform::FunctionFact kUiFilterActivate{L"EndUIFilter__ActivateType", 0x1414BF920, kUiFilterActivateEntry};
inline constexpr std::array<std::uint8_t, 16> kUiFilterDeactivateEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48};
inline constexpr platform::FunctionFact kUiFilterDeactivate{L"EndUIFilter__DeactivateType", 0x1414BFE30,
                                                            kUiFilterDeactivateEntry};
inline constexpr std::uintptr_t                kUiFilterWaitOpenCall = 0x14141A074;
inline constexpr std::array<std::uintptr_t, 4> kUiFilterRestoreCalls{0x1411A6F17, 0x1412C0643, 0x1412AA469, 0x1414C0395};

// --- The low-health red edge: UI filter 1, "Dying" (research/RE-low-health-effect.md) ---------------------------------
// FieldCommandCtrl__DyingFilter_PartyHp (once per command-controller tick) sets the controller's own flag when any
// formation member's HP is at or below its danger threshold, gated by the controller's allowed byte (on/off 43 writes it
// on player 1's controller only); the flags share the filter's count, and the effect is the screen-filter slot's
// post-process component. Its HP read is Party_GetHpByType(u8 type) -> u32, one call inside the function; a member read
// as kDyingNeverLow is never at or below a threshold.
inline constexpr std::uint32_t kCommandCtrlDying        = 0x424;  // u8: the controller's own danger flag
inline constexpr std::uint32_t kCommandCtrlDyingAllowed = 0x547;  // u8: the effect allowed (1), hidden by an event (0)
inline constexpr std::uint32_t kDyingNeverLow           = 0x7FFFFFFF;
inline constexpr std::array<std::uint8_t, 16> kPartyHpByTypeEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x05, 0x1B, 0x86, 0xCE, 0x04, 0x0F, 0xB6, 0xD9};
inline constexpr platform::FunctionFact kPartyHpByType{L"Party_GetHpByType", 0x140B01140, kPartyHpByTypeEntry};
inline constexpr std::uintptr_t         kDyingHpCall = 0x14141A162;  // in FieldCommandCtrl__DyingFilter_PartyHp
// The screen-filter slot EndUISystem[20]: a holder whose +0x08 is the post-process component and whose Dying fade is
// linear (current, start, target, elapsed, duration), run every HUD tick by EndUISlot20_ScreenFilterParams_Tick(holder,
// float dt). With +0x08 null the tick moves the holder's fades and writes nothing else, so a zeroed holder of
// kScreenFilterHolderBytes runs the same curve. The fade's target and length are the game's constants (data); the game
// fades in and out over the same length.
inline constexpr std::uint32_t  kScreenFilterSlot        = 20;
inline constexpr std::uint32_t  kScreenFilterComponent   = 0x08;
inline constexpr std::uint32_t  kScreenFilterHolderBytes = 0x70;
inline constexpr std::uint32_t  kDyingFadeNow            = 0x54;  // float
inline constexpr std::uint32_t  kDyingFadeStart          = 0x58;
inline constexpr std::uint32_t  kDyingFadeTarget         = 0x5C;
inline constexpr std::uint32_t  kDyingFadeElapsed        = 0x60;
inline constexpr std::uint32_t  kDyingFadeLength         = 0x64;
inline constexpr std::uintptr_t kDyingIntensityOn        = 0x144F91DF4;  // float 0.8
inline constexpr std::uintptr_t kDyingFadeSeconds        = 0x144F91D1C;  // float 0.5
inline constexpr std::array<std::uint8_t, 16> kScreenFilterTickEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9, 0x0F, 0x29, 0x74, 0x24, 0x20, 0x48, 0x8B};
inline constexpr platform::FunctionFact kScreenFilterTick{L"EndUISlot20_ScreenFilterParams_Tick", 0x1414996C0,
                                                          kScreenFilterTickEntry};
inline constexpr std::uintptr_t         kScreenFilterTickCall = 0x1413AE3D6;  // in EndUISystem__TickHUDWithDelta
// The component's FPostProcessSettings copy, made by its own tick and read by each view's blend: the override flags
// (bit 21 the narrow intensity) and the narrow intensity the view takes.
inline constexpr std::uint32_t kImageEffectsSettingsFlags = 0x31C;
inline constexpr std::uint32_t kImageEffectsNarrowBit     = 0x200000;
inline constexpr std::uint32_t kImageEffectsNarrowNow     = 0x77C;  // float

// --- The open battle menu's sound muffle: the "WaitMode" sound state (research/RE-menu-sound-muffle.md) ---------------
// The native of UEndSoundAPI::CallEvent. 12 WaitMode_ON (world sound quieter and lower, footsteps off, a wet reverb), 13
// WaitMode_OFF: absolute values for the one mix, no count. The wait's own two calls; the summon, a minigame's wait and an
// event's end have sites of their own. The open call shares its 16-byte block with kUiFilterWaitOpenCall, so it goes in
// after that one and comes out before it.
inline constexpr std::array<std::uint8_t, 16> kSoundEventEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x41};
inline constexpr platform::FunctionFact kSoundEvent{L"EndAudio_ApplyUIModeById", 0x1416AF370, kSoundEventEntry};
inline constexpr std::uintptr_t kWaitSoundOnCall  = 0x14141A07B;
inline constexpr std::uintptr_t kWaitSoundOffCall = 0x141419E36;
inline constexpr std::uint8_t   kSoundEventWaitOn = 12, kSoundEventWaitOff = 13;
// The mix's state, for the reading: the categories (pointers; id at +0x60; volume faders at +0x38, 48 bytes each, the game's
// layer third; the pitch fader at +0x40; a fader's target at +0x0C) and the mixer's current effect preset. se3d is world
// sound, masts the footsteps; preset 101 is WaitMode. Written on SEAD's thread: a read is a sample.
inline constexpr std::uintptr_t kSeadCategoryCount = 0x1457E8260, kSeadCategories = 0x1457E8268, kSeadMixer = 0x1457E8730;
inline constexpr std::uint32_t  kSeadCategoryId = 0x60, kSeadCategoryVolumes = 0x38, kSeadCategoryPitch = 0x40;
inline constexpr std::uint32_t  kSeadFaderBytes = 48, kSeadVolumeLayerGame = 2, kSeadFaderTarget = 0x0C, kSeadMixerPreset = 0x14;
inline constexpr std::int32_t   kSeadCategorySe3d = 28, kSeadCategoryMasts = 27, kSeadPresetWaitMode = 101;

// --- The battle system and the leader's input buffer (RE-battle.md, RE-battle-member-two.md §2.6) -------------------
inline constexpr std::uintptr_t kBattleSystemGlobal       = 0x1457E9720;  // the battle system (`sys`), null outside battle
inline constexpr std::uint32_t  kGameStatePauseCounter    = 0x4F0;        // AEndGameState: the pause-like counter (dt 0 while set)
inline constexpr std::uint32_t  kCharacterBattleComponent = 0xCB0;        // AEndCharacter: the battle player component (`comp`)
inline constexpr std::uint32_t  kBattleCompInputBufferSlot = 0xE0;        // comp vtable: the leader-only input buffer (comp, dt)

// --- Targeting for a further player (RE-battle-member-two.md §5, RE-battle.md §2.9) -------------------------------
// The battle system's target fields: the lock (+0x388), the displayed and lock-camera targets, the target-list mode,
// the stick-switch latch, the dummy-lock option, latch and timer, all in +0x370..+0x3E8. The targeting tick is called
// twice per battle tick, each after the leader's soft-target pre-tick; the second call crosses a block.
inline constexpr std::uint32_t kBattleSysTargetFields      = 0x370;
inline constexpr std::uint32_t kBattleSysTargetFieldsBytes = 0x78;
inline constexpr std::uint32_t kBattleSysLock              = 0x388;
inline constexpr std::uint32_t kBattleSysDisplayedTarget   = 0x390;  // the displayed (lock-camera) target's object (+0x398 its refcount block)
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

// --- The player's rates for a further player's member (RE-battle-member-two.md §2) ---------------------------------
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
/// BattlePlayerComp_SteerToMoveStick_InputDirRotate(comp, float dt): while an animation's turn window is open
/// (comp+0x758, set by UEndAnimNotifyStateBattleInputDirectionRotate) and the action is a dodge (comp+0x611 == 6), has no
/// live target or forces it (comp+0x75C), turns the character toward the move stick rotated by PartyManager+0's camera
/// (RE-roll-direction-two.md). Its one call site is in BattlePlayerComp_Tick, outside every scope.
inline constexpr std::array<std::uint8_t, 16> kBattleCompSteerToStickEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x48, 0x8B, 0xEC, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00};
inline constexpr platform::FunctionFact kBattleCompSteerToStick{L"BattlePlayerComp_SteerToMoveStick_InputDirRotate", 0x140979020,
                                                                kBattleCompSteerToStickEntry};
inline constexpr std::uintptr_t kBattleCompSteerToStickCall = 0x14096B730;  // E8 in BattlePlayerComp_Tick (rcx comp, xmm1 dt)
inline constexpr std::uint8_t   kBattleCompStateDodge       = 6;          // kBattleCompActionState (below): a dodge
inline constexpr std::uint32_t  kBattleCompTurnRate        = 0x758;      // float: the turn window's rate (0 closed)
inline constexpr std::uint32_t  kBattleCompTurnForce        = 0x75C;      // u8: the window turns even with a live target

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
// TransformCharacter exec) moves the displayed target to the new form; a script makes a character targetable or not
// (the battle API's SetDisableTarget exec; "lock nearest" in the names is the IDA name, research/RE-scripted-lock-seat.md).
// Both call sites sit inside one 16-byte block.
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
inline constexpr std::uint32_t kOperatorSettingTown    = 0x5E8;  // FEndCameraSetting: the Town setting (nav-area volumes, type 0)
inline constexpr std::uint32_t kOperatorSettingInDoor  = 0x5F0;  // FEndCameraSetting: the InDoor setting (nav-area volumes, types 1, 2)
inline constexpr std::uint32_t kOperatorSettingRequests     = 0x28;   // a setting's request count (+0x30 its active byte)
inline constexpr std::uint32_t kOperatorFlavors             = 0x6C0;  // kOperatorFlavorTypes flavor objects, 8 bytes each, by type
inline constexpr std::uint32_t kOperatorFlavorTypes         = 29;
inline constexpr std::uint32_t kFlavorActive                = 0x38;   // a flavor object's active byte
inline constexpr std::uint32_t kFlavorMoveFollow            = 2;
inline constexpr std::uint32_t kFlavorSpaceOffset           = 3;
inline constexpr std::uint32_t kFlavorTargetLength          = 4;
inline constexpr std::uint32_t kFlavorTargetOffset          = 5;
inline constexpr std::uint32_t kOperatorSpaceOffsetByRow    = 0x7A8;  // u8: the SpaceOffset flavor follows the active setting row
inline constexpr std::uint32_t kOperatorDistanceOptionPtr   = 0x8C0;  // u8*: the active camera-distance option byte
inline constexpr std::uint32_t kOperatorFieldDistanceOption = 0x8BA;  // u8 (+0x8BB the battle one)
/// FieldCamera_SetDistanceOptions(u8 field, u8 battle): the options menu's Camera Distance writes both bytes on the field
/// root's operator (player 1's) and reads nothing else (RE-camera-distance-option.md). A further operator copies the
/// options at its BeginPlay only.
inline constexpr std::array<std::uint8_t, 16> kFieldCameraSetDistanceOptionsEntry{
    0x48, 0x8B, 0x05, 0x39, 0xB6, 0x81, 0x04, 0x44, 0x0F, 0xB6, 0xC9, 0x44, 0x8B, 0x80, 0xB4, 0x09};
inline constexpr platform::FunctionFact kFieldCameraSetDistanceOptions{L"FieldCamera_SetDistanceOptions", 0x140FCE390,
                                                                       kFieldCameraSetDistanceOptionsEntry};
/// Its two calls, OptionsMenu_SetOptionValue's cases 14 (CameraDistanceField) and 15 (CameraDistanceBattle). The second
/// crosses a block (placed from the game thread); its second block also holds case 16's call (MoveFollow): a redirect
/// there too could only be restored in reverse order.
inline constexpr std::array<std::uintptr_t, 2> kFieldCameraSetDistanceOptionsCalls{0x1412D6C66, 0x1412D6C7C};
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
inline constexpr std::uint32_t kControllerNavAreaComponent = 0x6F0;  // AEndPlayerController.PlayerExistNavAreaComponent
/// AEndPlayerController::LeaderCharacter, the controller's own field character: the member it plays (SwitchLeaderInternal
/// sets it), what its nav-area component's diff projects onto the navmesh and what its field move reads the walk caps
/// from (research/RE-room-walk-limit.md §1).
inline constexpr std::uint32_t kControllerLeaderCharacter  = 0x6F8;
/// The InDoor volume's Enter, Stay and Leave each write the room's walk limit (cap layer 6) onto
/// PartyManager_GetPartyLeader(PM, 1), the default leader, whichever controller's component runs them
/// (research/RE-room-walk-limit.md §5.2): the three `mov dl,1; call` sites. Enter and Stay each lie inside their own
/// 16-byte block; Leave crosses into the next (placed from the game thread, the only thread that runs the handlers).
inline constexpr std::array<std::uintptr_t, 3> kRoomLeaderCalls{0x1415E2232, 0x1415E29B5, 0x1415E2D9D};
inline constexpr std::uint8_t                  kCapLayerRoom = 6;
// The component's map of entered volumes, TMap<FName, AActor*> keyed by the volume's Tags[0]
// (RE-nav-area-reenter.md §4). The diff is its only writer; nothing in the game forgets it short of a new component.
inline constexpr std::uint32_t kNavAreaEnteredMap        = 0x100;  // elements of 24 bytes: key, actor, two hash links
inline constexpr std::uint32_t kNavAreaEnteredNum        = 0x108;  // ArrayNum (free slots included)
inline constexpr std::uint32_t kNavAreaEnteredBitsInline = 0x110;  // AllocationFlags, inline words
inline constexpr std::uint32_t kNavAreaEnteredBitsHeap   = 0x120;  // AllocationFlags heap pointer, 0 = inline
inline constexpr std::uint32_t kNavAreaEnteredNumBits    = 0x128;
inline constexpr std::uint32_t kNavAreaEnteredElement    = 24;
// (component, float dt): the diff the tick tail-jumps to; the game's own camera reset runs it out of band at dt 0.
inline constexpr std::array<std::uint8_t, 16> kNavAreaRefreshFromLeaderEntry{
    0x4C, 0x8B, 0xDC, 0x55, 0x41, 0x54, 0x49, 0x8D, 0xAB, 0x38, 0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC};
inline constexpr platform::FunctionFact kNavAreaRefreshFromLeader{
    L"EndPlayerExistNavAreaComponent::RefreshFromLeader", 0x1415F9FB0, kNavAreaRefreshFromLeaderEntry};
// (map, FName key) -> 1 if removed: the instantiation the diff itself uses to forget a left volume.
inline constexpr std::array<std::uint8_t, 16> kNavAreaMapRemoveKeyEntry{
    0x48, 0x83, 0xEC, 0x28, 0x8B, 0x41, 0x08, 0x4C, 0x8B, 0xD9, 0x3B, 0x41, 0x34, 0x0F, 0x84, 0xF5};
inline constexpr platform::FunctionFact kNavAreaMapRemoveKey{
    L"TMap<FName, AActor*>::Remove (the nav-area map's instantiation)", 0x140AEAC80, kNavAreaMapRemoveKeyEntry};
inline constexpr std::array<std::uint8_t, 16> kScriptLockNearestEntry{
    0x48, 0x85, 0xC9, 0x0F, 0x84, 0xF9, 0x03, 0x00, 0x00, 0x57, 0x41, 0x56, 0x48, 0x81, 0xEC, 0xC8};
inline constexpr platform::FunctionFact kScriptLockNearest{L"BattleTarget_ScriptLockNearest_FromExec", 0x14095C8B0,
                                                           kScriptLockNearestEntry};
inline constexpr std::uintptr_t kScriptLockNearestCall = 0x141873C31;  // E8 7A 8C 0E FF

// The enemy detail window's close delegate restores the lock and asks the Command camera flavor (row X5; named a
// command-menu state end before pass S-RMK-CMDCAM read it, RE-command-camera-two.md §2.5): the Execute and
// ExecuteIfSafe slots of the delegate instance the window's open binds (the third slot is the base vtable the
// constructor overwrites, never called). Which context runs them for a further player is read first.
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

// The field camera's Command flavor (13, FEndCameraFlavorCommand, RE-command-camera-two.md §2): its per-frame aim
// (vtable +0x20) runs inside the operator's tick and reads EndUISystem[19] and the focus path for the list row's
// target; the subject is the controller's member (option FriendCommandCameraType) or the operator's owner.
inline constexpr std::uintptr_t kCommandFlavorVTable = 0x144BB8060;
inline constexpr std::uint32_t  kFlavorOperator      = 0x10;   // a flavor object's operator
inline constexpr std::uint32_t  kFlavorStarted       = 0x39;   // a flavor object's started byte (its first tick ran)
inline constexpr std::uint32_t  kCommandFlavorTarget = 0x380;  // the Command flavor's stored target (a shared pointer's object)
inline constexpr std::array<std::uint8_t, 16> kCommandFlavorAimEntry{
    0x4C, 0x89, 0x4C, 0x24, 0x20, 0x55, 0x53, 0x57, 0x41, 0x54, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24};
inline constexpr platform::SlotFact kCommandFlavorAim{
    L"FEndCameraFlavorCommand vtable +0x20: the per-frame aim", kCommandFlavorVTable + 0x20,
    {L"FEndCameraFlavorCommand__Vt20", 0x1410A32B0, kCommandFlavorAimEntry}};

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
/// GetPartyLeader's selectors: 0 the current leader (PM+0x238), 1 the default party leader (the type byte behind
/// Get/SetDefaultPartyLeaderTypeBP; no scope of the mod swaps it).
inline constexpr std::uint8_t kLeaderCurrent = 0, kLeaderDefault = 1;
/// A character's walk caps (research/RE-forced-walk-two.md): EEndSpecificMoveType per EEndSpecificMoveTypeLayer at
/// +0x2258 (7; 6 = none) and EEndDashType per layer at +0x225F (7; 0 = dash allowed). Only each controller's own field
/// move reads them, from its own character (PC+0x6F8): a further player's member is capped by its own bytes.
inline constexpr std::uint32_t kCharacterMoveCaps  = 0x2258;
inline constexpr std::uint32_t kCharacterDashCaps  = 0x225F;
inline constexpr std::uint8_t  kMoveCapNone = 6, kDashCapNone = 0;
inline constexpr std::uint8_t  kCapLayerScript = 1, kCapLayerAfterScene = 2, kCapLayerGuide = 4;
/// The story acts that cap the party leader's walk (act runners: vtable +0x08 Begin, +0x10 End, +0x20 Tick(runner,
/// float dt) returning nonzero while running; the act sequence calls them on the game thread from the world tick).
/// They store the layer pair straight into the leader's character (no setter): PlayerMoveType (act 0x29, layer 1, set
/// every Tick, cleared at End; the default leader), AfterGlow (0x79, layer 2: Walk at Begin, Jog after a time, cleared
/// at End; the current leader) and AutoWalk (0x38, layer 4, a band by the leader's distance to a guide point every Tick,
/// cleared at End; the default leader).
inline constexpr std::array<std::uint8_t, 16> kActScriptWalkTickEntry{0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
inline constexpr platform::SlotFact kActScriptWalkTick{L"ActRunner29_PlayerMoveType_Tick_SetL1", 0x144B923E0, {L"ActRunner29_PlayerMoveType_Tick_SetL1", 0x140DE65B0, kActScriptWalkTickEntry}};
inline constexpr std::array<std::uint8_t, 16> kActScriptWalkEndEntry{0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xE8, 0x55, 0x15, 0x87, 0xFF, 0x48, 0x8B, 0xC8, 0xB2, 0x01};
inline constexpr platform::SlotFact kActScriptWalkEnd{L"ActRunner29_PlayerMoveType_End_ClearL1", 0x144B923D0, {L"ActRunner29_PlayerMoveType_End_ClearL1", 0x140DE6560, kActScriptWalkEndEntry}};
inline constexpr std::array<std::uint8_t, 16> kActGuideWalkTickEntry{0x40, 0x55, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xB1, 0x48, 0x81, 0xEC, 0xB8, 0x00, 0x00, 0x00, 0x48};
inline constexpr platform::SlotFact kActGuideWalkTick{L"ActRunner38_AutoWalk_Tick_SetL4ByDistance", 0x144B874A0, {L"ActRunner38_AutoWalk_Tick_SetL4ByDistance", 0x140DA9280, kActGuideWalkTickEntry}};
inline constexpr std::array<std::uint8_t, 16> kActGuideWalkEndEntry{0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48};
inline constexpr platform::SlotFact kActGuideWalkEnd{L"ActRunner38_AutoWalk_End_ClearL4", 0x144B87490, {L"ActRunner38_AutoWalk_End_ClearL4", 0x140DA9190, kActGuideWalkEndEntry}};
inline constexpr std::array<std::uint8_t, 16> kActAfterSceneBeginEntry{0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55};
inline constexpr platform::SlotFact kActAfterSceneBegin{L"ActRunner79_AfterGlow_Begin_SetL2Walk", 0x144B92418, {L"ActRunner79_AfterGlow_Begin_SetL2Walk", 0x140DE6110, kActAfterSceneBeginEntry}};
inline constexpr std::array<std::uint8_t, 16> kActAfterSceneTickEntry{0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x40};
inline constexpr platform::SlotFact kActAfterSceneTick{L"ActRunner79_AfterGlow_Tick_L2Jog", 0x144B92430, {L"ActRunner79_AfterGlow_Tick_L2Jog", 0x140DE6420, kActAfterSceneTickEntry}};
inline constexpr std::array<std::uint8_t, 16> kActAfterSceneEndEntry{0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xE8, 0xE5, 0x16, 0x87, 0xFF, 0x48, 0x8B, 0xC8, 0x33, 0xD2};
inline constexpr platform::SlotFact kActAfterSceneEnd{L"ActRunner79_AfterGlow_End_ClearL2", 0x144B92420, {L"ActRunner79_AfterGlow_End_ClearL2", 0x140DE63D0, kActAfterSceneEndEntry}};
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
inline constexpr std::array<std::uint8_t, 5> kBattleCompSetTargetCharFirst{0x48, 0x89, 0x5C, 0x24, 0x10};  // mov [rsp+10h], rbx
/// Who sets an enemy's target (battle/enemy_watch; research/RE-enemy-attack-cadence.md §3): the functions that call
/// BattleComp_SetTargetChar, by their IDA start, named for the log. sub_140922310 jumps to it (its caller's frame shows).
struct TargetSetter
{
    std::uintptr_t start;
    const wchar_t* name;
};
inline constexpr std::array<TargetSetter, 13> kTargetSetters{{
    {0x1408816D0, L"the script's SetTarget (variant)"},
    {0x14088E5A0, L"the script's SetTarget"},
    {0x14089D6A0, L"a normal attack"},
    {0x140922310, L"a script target call (sub_140922310)"},
    {0x1409223A0, L"the script's SetTargetByType"},
    {0x140931880, L"the valid-target check's hate re-pick"},
    {0x14093ED90, L"the hate update"},
    {0x1409B1A80, L"the battle's start"},
    {0x1409BAA80, L"the leader change"},
    {0x140A4E3F0, L"sub_140A4E3F0"},
    {0x140E5CAE0, L"sub_140E5CAE0"},
    {0x140E5DCF0, L"sub_140E5DCF0"},
    {0x141171C60, L"sub_141171C60"},
}};
/// BattleComp_IsHateEnemy(comp): true while the enemy targets by hate (BattleComp_SetTargetChar then takes the hate
/// pick instead of its argument). Reads only.
inline constexpr std::array<std::uint8_t, 16> kBattleCompIsHateEnemyEntry{
    0x4C, 0x8B, 0x89, 0xB8, 0x05, 0x00, 0x00, 0x4D, 0x85, 0xC9, 0x0F, 0x84, 0xA4, 0x00, 0x00, 0x00};
inline constexpr platform::FunctionFact kBattleCompIsHateEnemy{L"BattleComp_IsHateEnemy", 0x140982E30, kBattleCompIsHateEnemyEntry};
/// The enemy component's attack-ready test (vtable +0x1A8 of the 13 enemy component vtables; called each tick by
/// vt+0x1A0, which fires the Blueprint's OnATBLightAttackEvent on 1 and OnATBHardAttackEvent on 2): 0 unless the
/// decision gate is open; 2 when the ATB is full; 1 when it gained a unit (LightAttachATBIntervalTime of charge)
/// since the last ability ended. Reads only (research/RE-enemy-attack-cadence.md §2.6, §13).
inline constexpr std::array<std::uint8_t, 16> kBattleEnemyAttackReadyEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48};
inline constexpr platform::FunctionFact kBattleEnemyAttackReady{L"BattleEnemyComp_Vt1A8_ATBAttackReady_2Hard_1Light", 0x14096EDC0,
                                                                kBattleEnemyAttackReadyEntry};
inline constexpr std::array<std::uint8_t, 5> kBattleEnemyAttackReadyFirst{0x48, 0x89, 0x5C, 0x24, 0x08};  // mov [rsp+8], rbx
inline constexpr std::uint32_t kComponentOwnerGetterSlot = 0x20 / 8;  // battle component vtable: its owner character
inline constexpr std::array<std::uint8_t, 16> kGameFreeEntry{
    0x48, 0x85, 0xC9, 0x74, 0x2E, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x0D};
inline constexpr platform::FunctionFact kGameFree{L"FMemory::Free", 0x1416C0370, kGameFreeEntry};

// --- The equal-attention coin (RE-enemy-attention-coin.md §2-§4, §6; battle/enemy_coin) --------------------------
// Every enemy decides at one call: its battle AI tick calls the decision gate (vtable +0xA50, one function in 64
// vtables; Reno's mines override it), which is true only while no action runs; the Blueprint OnUpdate then chooses.
inline constexpr std::uint32_t kBattleAICurrentAction = 0x1A40;  // the controller's running action (an FName; 0 = none)
inline constexpr std::uint32_t kBattleCompPlayerType  = 0x58;    // the battle component's player type byte (vs PM+0x233)
// The battle component's own target (BattlePlayerComp_SetTarget38 / GetCurrentTarget 0x14096C410): a shared-pointer
// pair, the target object at +0x38 and its reference block at +0x40 (live while the block's strong count at +8 > 0);
// the target object's vtable slot +0x20 returns its character.
inline constexpr std::uint32_t kBattleCompOwnTarget    = 0x38;
inline constexpr std::uint32_t kBattleCompOwnTargetRef = 0x40;
inline constexpr std::uint32_t kSharedRefStrongCount   = 0x8;
inline constexpr std::uint32_t kTargetObjectCharacterSlot = 0x20 / 8;
inline constexpr std::size_t   kPartyManagerBytes     = 0x258;   // the party manager's size
inline constexpr std::uint32_t kFrameObject           = 0x18;    // FFrame: the object whose script is running (+0x20 = Code)
inline constexpr std::uint32_t kCharacterController   = 0xCF0;   // AEndCharacter: the controller possessing it
inline constexpr std::array<std::uint8_t, 16> kPartyManagerGetEntry{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x05, 0xBD, 0x1F, 0x19, 0x05, 0x48, 0x85, 0xC0, 0x0F, 0x85};
inline constexpr platform::FunctionFact kPartyManagerGet{L"PartyManager_Get", 0x140657AC0, kPartyManagerGetEntry};
inline constexpr std::array<std::uint8_t, 16> kIsABattleAIControllerEntry{
    0x4C, 0x8B, 0xDC, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05, 0xC7, 0x52, 0x31};
inline constexpr platform::FunctionFact kIsABattleAIController{L"IsA_EndBattleAIController", 0x140686CB0, kIsABattleAIControllerEntry};
inline constexpr std::array<std::uint8_t, 16> kBattleCompIsPartySideEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xD9};
inline constexpr platform::FunctionFact kBattleCompIsPartySide{L"BattlePlayerComp_IsPartySide", 0x14096CA90, kBattleCompIsPartySideEntry};
inline constexpr std::array<std::uint8_t, 16> kBattleAIGetPawnCompEntry{
    0x44, 0x8B, 0x81, 0x5C, 0x07, 0x00, 0x00, 0x45, 0x85, 0xC0, 0x74, 0x46, 0x48, 0x63, 0x81, 0x58};
inline constexpr platform::FunctionFact kBattleAIGetPawnComp{L"EndBattleAI_GetPawnComp", 0x140940190, kBattleAIGetPawnCompEntry};
/// The decision gate and the mines' override; the toss sits at their entry (the first instruction, a home-area store).
inline constexpr std::uintptr_t kBattleAIDecisionGate     = 0x14091F1F0;  // EndBattleAI_Vt0A50_DecisionGate_IdleHate
inline constexpr std::uintptr_t kBattleAIMineDecisionGate = 0x14081F050;  // EndBattleAIEB0003Mine_LeaderOrOwnInRange_Hate
inline constexpr std::array<std::uint8_t, 5> kBattleAIDecisionGateFirst{0x48, 0x89, 0x5C, 0x24, 0x18};
inline constexpr std::array<std::uint8_t, 5> kBattleAIMineDecisionGateFirst{0x48, 0x89, 0x5C, 0x24, 0x08};
/// The opening target: the enemy component's battle start (vtable 0x144B6B1C8 slot +0x100) reads the party leader
/// at one call (E8 D7 EA D5 00, inside its block) and sets it as the first target.
inline constexpr std::array<std::uint8_t, 16> kBattleEnemyCompBattleStartEntry{
    0x40, 0x55, 0x41, 0x55, 0x41, 0x56, 0x48, 0x8D, 0xAC, 0x24, 0x50, 0xFF, 0xFF, 0xFF, 0x48, 0x81};
inline constexpr platform::FunctionFact kBattleEnemyCompBattleStart{L"BattleEnemyComp_Vt100_BattleStart_TargetLeader", 0x140893190,
                                                                    kBattleEnemyCompBattleStartEntry};
inline constexpr platform::SlotFact kBattleEnemyCompBattleStartSlot{L"the enemy battle component's battle start (slot +0x100)",
                                                                    0x144B6B1C8 + 0x100, kBattleEnemyCompBattleStart};
inline constexpr std::uintptr_t kBattleOpeningLeaderCall = 0x140893204;
/// The inlined "is this member the player" reads (call PartyManager_Get; movzx ecx, [rax+0x233]; cmp [reg+0x58], cl;
/// the manager read for nothing else): the hate rows (member in r14), the hate pick's two tie-break reads (member in
/// rdi, the picking enemy's battle AI controller in r13, only rax and rcx read after the call; the second crosses its
/// block), IsAIPC (rbx; crosses), and a voice line's lethal-hit test (the attacker's battle component in rsi, read after
/// the hit amounts: its answer is the HP floors' "the attacker is the player", RE-voice-line-floor-test.md).
inline constexpr std::uintptr_t kBattleHateTypeCall = 0x14093F27A;                           // E8 41 88 D1 FF
inline constexpr std::array<std::uintptr_t, 2> kBattleTieTypeCalls{0x140940081, 0x14094009E};  // E8 3A 7A D1 FF, E8 1D 7A D1 FF
inline constexpr std::uintptr_t kBattleIsAipcTypeCall = 0x14187D25D;                         // E8 5E A8 DD FE
inline constexpr std::uintptr_t kBattleVoiceFloorTypeCall = 0x1416F2022;                     // E8 99 5A F6 FE
/// The expectation counts' reads of the leader type (movzx from PartyManager_Get's result, used for nothing else):
/// the coin's human's type answers.
inline constexpr std::array<std::uintptr_t, 2> kBattleExpectationTypeCalls{0x1409598F1, 0x14095A3A7};  // E8 CA E1 CF FF, E8 14 D7 CF FF
/// The enemy EQS target context keeps a party non-leader only if its controller IsA battle AI controller (E8 82 6A CD FF).
inline constexpr std::uintptr_t kBattleEqsIsACall = 0x1409B0229;

// --- A stalled hate enemy picks again (RE-enemy-stall-repick.md §3.3-§3.5, §5, §6; battle/enemy_stall) -------------
// The gate (kBattleAIDecisionGate) calls EndBattleAI_UpdateHate_ApplyTarget(aic, 9) at 0x14091F261, before its delay
// test. That update returns unless BattleComp_IsHateEnemy and the pending byte are set; it then adds the hate rows,
// picks (EndBattleAI_Hate_PickTarget) and sets the target (0x14093F71D) only while the component's target lock is 0,
// and clears the byte (0x14093F722). Blueprint SetHateUpdateFlag's whole body is the byte's write (0x1417B1525).
inline constexpr std::uint32_t kBattleAIHateUpdatePending = 0x1A60;  // u8 on the controller
inline constexpr std::uint32_t kBattleAIForcedHateCount   = 0x1B28;  // i32: above 0, the update only picks again from its list
inline constexpr std::uint32_t kBattleAIRequestFlags      = 0x1590;  // the controller's request flags (a pointer, as Accept reads it)
inline constexpr std::uint32_t kBattleAIRequestsBlocked   = 5;       // its byte +5: eBlockRequest, Accept refuses every request
inline constexpr std::uint32_t kBattleAILookChangeValues  = 0x15A0;  // a pointer the give-up tick reads at its start
inline constexpr std::uint32_t kBattleAILookChangeMode    = 0x20;    // its u32 +0x20: btlvalue[8], LookChange (0 off)
inline constexpr std::uint32_t kBattleAILightInterval     = 0x1FE4;  // float s: LightAttachATBIntervalTime
inline constexpr std::uint32_t kBattleCompTargetLock      = 0x310;   // u32 on the component: LockTargetCharaBase (0 unlocked)
/// A character's own area queries (its EnvironmentQueryComponent, char+0x1AA8) run in battle only for the leader or a
/// character whose controller IsA battle AI controller (EndEnvQueryComponent_TickQueries_BattleGate, E8 55 D0 00 FF;
/// rcx = char+0xCF0, only al used after): enemies read the area of the character they target (RE-enemy-circling.md).
inline constexpr std::uintptr_t kCharacterAreaIsACall    = 0x141679C56;
inline constexpr std::uint32_t  kCharacterAreaQueries    = 0x1AA8;  // AEndCharacter: its EnvironmentQueryComponent
inline constexpr std::uint32_t  kAreaQueryEntries        = 0xF8;    // the component's query entries (0x110 bytes each)
inline constexpr std::uint32_t  kAreaQueryCount          = 0x100;   // their count (i32)
inline constexpr std::uint32_t  kAreaEntryPointCount     = 0xB0;    // an entry's result points: the count (i32)
/// Hits: the apply (entry; the hit's attacker weak at +0x50, victim weak at +0x78) and its second form's one call
/// (E8 8F DF 2F FF, crossing its block; a voice line's lethal-hit test: its 2nd argument the attack's data row, its 3rd and
/// 4th the attacker and the victim).
inline constexpr std::uintptr_t kBattleHitApply = 0x1409E9C70;  // BattleDamage_ApplyHitToVictim
inline constexpr std::array<std::uint8_t, 5> kBattleHitApplyFirst{0x48, 0x89, 0x5C, 0x24, 0x18};
inline constexpr std::uint32_t  kBattleHitAttackerWeak = 0x50;
inline constexpr std::uint32_t  kBattleHitVictimWeak   = 0x78;
inline constexpr std::array<std::uint8_t, 16> kBattleHitApplyBEntry{
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xA0};
inline constexpr platform::FunctionFact kBattleHitApplyB{L"BattleDamage_ApplyHitToVictim_B", 0x1409EFE40, kBattleHitApplyBEntry};
inline constexpr std::uintptr_t kBattleHitApplyBCall = 0x1416F1EAC;
/// The attacker's ability start tells the enemies (OnReactionSensing): E8 D8 12 02 00, inside its block.
inline constexpr std::array<std::uint8_t, 16> kBattleReactionSensingEntry{
    0x4C, 0x8B, 0xDC, 0x4D, 0x89, 0x4B, 0x20, 0x4D, 0x89, 0x43, 0x18, 0x49, 0x89, 0x4B, 0x08, 0x55};
inline constexpr platform::FunctionFact kBattleReactionSensing{L"BattleReaction_NotifySensingEnemies", 0x140996200, kBattleReactionSensingEntry};
inline constexpr std::uintptr_t kBattleReactionSensingCall = 0x140974F23;
/// The remaining leader reads (research/RE-enemy-coin-remaining-reads.md; every read call E8, inside its block):
/// - the follow ticks (controller vtable +0x408) of Reno's mines (also the Ch. 12 pyramids and Weiss's black holes),
///   Ramuh's charge cores and Leviathan's water tornadoes, whose controller is recorded around the tick;
/// - inside them the reads of the leader's actor: a mine's follow, its area test (in the mine gate), its rolling
///   trigger and goal; a core's and a tornado's follow;
/// - Reno's FinishRollingAtack starting each mine's roll (the mine in rcx, recorded when nothing is);
/// - the flyers' point evaluation (its move struct recorded at its one call; its first field is the controller) and
///   its leader read;
/// - the height chase (R22) in SelectMoveType (its move struct recorded at its one call, which crosses its block) and
///   its leader read.
inline constexpr std::array<std::uint8_t, 16> kMineFollowTickEntry{
    0xF3, 0x0F, 0x11, 0x4C, 0x24, 0x10, 0x55, 0x53, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC};
inline constexpr std::array<std::uint8_t, 16> kCoreFollowTickEntry{
    0x48, 0x8B, 0xC4, 0xF3, 0x0F, 0x11, 0x48, 0x10, 0x55, 0x57, 0x41, 0x55, 0x48, 0x8D, 0xA8, 0x38};
inline constexpr std::array<std::uint8_t, 16> kTornadoFollowTickEntry{
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0x80, 0x48, 0x81, 0xEC, 0x80};
inline constexpr platform::SlotFact kMineFollowTickSlot{L"the mine controller's tick (slot +0x408)", 0x144C4A580 + 0x408,
                                                        {L"EndBattleAIEB0003Mine_Tick_FollowLeader", 0x14081D860, kMineFollowTickEntry}};
inline constexpr platform::SlotFact kCoreFollowTickSlot{L"the charge core controller's tick (slot +0x408)", 0x144C63328 + 0x408,
                                                        {L"EndBattleAISU0009_Tick_FocusLeader (Ramuh's cores)", 0x140850530, kCoreFollowTickEntry}};
inline constexpr platform::SlotFact kTornadoFollowTickSlot{L"the water tornado controller's tick (slot +0x408)", 0x144C6A668 + 0x408,
                                                           {L"EndBattleAISU0004_02_Tick_LeaderRead", 0x14084CF70, kTornadoFollowTickEntry}};
inline constexpr std::uintptr_t kMineFollowActorCall  = 0x14081D8A9;  // E8 A2 2D DD 00
inline constexpr std::uintptr_t kMineAreaActorCall    = 0x14081F1E6;  // E8 65 14 DD 00 (the mine gate)
inline constexpr std::uintptr_t kMineTriggerActorCall = 0x14081F478;  // E8 D3 11 DD 00
inline constexpr std::uintptr_t kMineGoalActorCall    = 0x140820049;  // E8 02 06 DD 00
inline constexpr std::uintptr_t kCoreFollowActorCall  = 0x1408505B3;  // E8 98 00 DA 00
inline constexpr std::uintptr_t kTornadoFollowActorCall = 0x14084CFB0;  // E8 9B 36 DA 00
inline constexpr std::uintptr_t kFlyerPointActorCall  = 0x14080ECE1;  // E8 6A 19 DE 00
inline constexpr std::uintptr_t kHeightChaseLeaderCall = 0x140805593;  // E8 28 42 12 00
inline constexpr std::array<std::uint8_t, 16> kMineStartRollingEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
inline constexpr platform::FunctionFact kMineStartRolling{L"EndBattleAIEB0003Mine_StartRollingAttack_AtLeader", 0x140820010,
                                                          kMineStartRollingEntry};
inline constexpr std::uintptr_t kRenoFinishRollingCall = 0x141812722;  // E8 E9 D8 00 FF (execFinishRollingAtack)
inline constexpr std::array<std::uint8_t, 16> kFlyerEvalPointEntry{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41};
inline constexpr platform::FunctionFact kFlyerEvalPoint{L"EndBattleAI_Aerial_EvalPoint_ByLeader", 0x14080EAE0, kFlyerEvalPointEntry};
inline constexpr std::uintptr_t kFlyerEvalPointCall = 0x14080DC0B;  // E8 D0 0E 00 00
inline constexpr std::array<std::uint8_t, 16> kSelectMoveTypeEntry{
    0x40, 0x55, 0x53, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00};
inline constexpr platform::FunctionFact kSelectMoveType{L"EndBattleAIMove_SelectMoveType", 0x140805290, kSelectMoveTypeEntry};
inline constexpr std::uintptr_t kSelectMoveTypeCall = 0x140803CBF;  // E8 CC 15 00 00, crosses its block
inline constexpr std::array<std::uint8_t, 16> kGetLeaderEndCharacterEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xE8, 0xF5, 0xE2, 0xD2, 0xFF, 0x48, 0x8B, 0xC8, 0x33, 0xD2};
inline constexpr platform::FunctionFact kGetLeaderEndCharacter{L"PartyManager_GetLeaderEndCharacter", 0x1409297C0,
                                                               kGetLeaderEndCharacterEntry};
/// The Blueprint natives that name "the player", by their exec functions (checked against UFunction::Func before a
/// swap), in the order of battle/enemy_coin's path table: the controller's nine (the context is the enemy), then the
/// library's seven (the calling object is the enemy).
inline constexpr std::array<std::uintptr_t, 16> kBattleCoinNativeExecs{
    0x1417B32B0,  // EndBattleAIController:GetLeader
    0x1417BC210,  // :SetTargetByType
    0x1417AD100,  // :GetActionCharacter
    0x1417AD360,  // :GetSyncActionCharacter
    0x1417AD5B0,  // :GetSpecialStatusChangeCharacter
    0x1417AD830,  // :GetStatusChangeCharacter
    0x1417ADC10,  // :HasStatusChangeEsuna
    0x1417ADD90,  // :HasStatusChangeDeSpell
    0x1417ADE70,  // :HasStatusChange
    0x1418783A0,  // EndBattleAPI:GetExpectationCountInConeAreaByLocation
    0x141878710,  // :GetExpectationCountInConeArea
    0x141878A60,  // :GetExpectationCountInCapsuleAreaByLocation
    0x141878DD0,  // :GetExpectationCountInCapsuleArea
    0x14187D180,  // :IsAIPC
    0x141B8E820,  // EndPartyAPI:GetPartyLeader
    0x141B8E780,  // EndPartyAPI:GetPartyLeaderType
};

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

// The shot's kind (RE-scene-like-battle-shots.md): an entry's mode byte is `{0,2,3,1}[row PriorityType]`, and each
// operator's apply writes the mode of the shot it applied to the lists (4 first: none), which the game's own summon
// check and leader-change camera read. Mode 3 is active on every operator: the phase changes, the summons' entries
// and specials, the boss finish.
inline constexpr std::uint32_t kSeqListsAppliedMode      = 0x50;
inline constexpr std::uint32_t kSeqEntryRow              = 0x00;   // the BattleCameraSequence row
inline constexpr std::uint32_t kSeqEntryMode             = 0x18;
inline constexpr std::uint32_t kSeqRowPriorityType       = 0x08;
inline constexpr std::uint32_t kSeqSequenceId            = 0x470;  // the id the starter returns
inline constexpr std::uint32_t kSeqFinished              = 0x474;  // set when the blend-out ends; the battle tick prunes the entry
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
/// BattleCamera_PlaySequenceForOwner(const FName* row, AActor* owner, AActor* holder): every battle shot's start; returns
/// the sequence's id or -1.
inline constexpr std::array<std::uint8_t, 16> kSeqPlayForOwnerEntry{
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
inline constexpr platform::FunctionFact kSeqPlayForOwner{L"BattleCamera_PlaySequenceForOwner", 0x1409FD5B0, kSeqPlayForOwnerEntry};
inline constexpr std::size_t            kSeqPlayForOwnerFirst = 5;  // mov [rsp+20h], rbx

// --- The battle switch (RE-battle-switch.md §4) -----------------------------------------------------------------
// BattleSystem_CycleLeader walks the members from the leader; BattleSystem_CanSwitchLeader counts them. Each fetches
// a candidate with PartyManager_GetPlayerActor(PM, 0, type, 0); a null answer is the game's own skip branch. The
// cycle's call sits inside a block; CanSwitch's crosses one (placed from the game thread, outside the function).
inline constexpr std::uintptr_t kCycleLeaderCandidateCall = 0x1409BAC99;
inline constexpr std::uintptr_t kCanSwitchCandidateCall   = 0x1409BA98C;
/// The one forced leader change in battle (research/RE-pyramid-trap-control.md): the KO'd leader's dead-state tick
/// (BattlePlayerComp_TickDeadState_KOAutoSwitch 0x1409864C0) calls BattleSystem_CycleLeader(sys, 1, forced 1,
/// input 0) every tick while it lies down (E8 6D 44 03 00, crossing its block; a jmp follows, the result unused). The
/// cycle's other callers are the d-pad (twice) and its retry buffer, which only a refused forced call writes.
inline constexpr std::uintptr_t kKoAutoSwitchCall = 0x14098660E;
inline constexpr std::array<std::uint8_t, 16> kCycleLeaderEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
inline constexpr platform::FunctionFact kCycleLeader{L"BattleSystem_CycleLeader", 0x1409BAA80, kCycleLeaderEntry};
/// The d-pad's two calls of the cycle in FieldCommandCtrl_Tick_LeaderOrdinal_SwitchReq01 (RE-battle-switch.md): the
/// next member (BattleSystem_CycleLeader(sys, 1, forced 0, input 1), E8 ED D0 59 FF) and the previous one (direction 0,
/// E8 9F D0 59 FF). Both cross their 16-byte block (placed from the game thread, outside the tick).
inline constexpr std::uintptr_t kDpadSwitchNextCall     = 0x14141D98E;
inline constexpr std::uintptr_t kDpadSwitchPreviousCall = 0x14141D9DC;
/// The command controller's input poll (FieldCommandCtrl_Tick_PollInputToRequests 0x14141C7B0, the one writer of the
/// switch requests; RE-battle-switch.md §15) reads no key at all while the member at the controller's cached leader
/// ordinal lies knocked out: its call of Party_IsMemberKOByType_State13(unused, u8 type) at 0x14141C9F5 (E8 A6 0D EA FF,
/// inside its block) answering true leaves the poll. Single player never needs that poll then (the forced switch
/// moves the leader off the body); with the forced switch refused, the d-pad is the only way off. The poll reads the
/// d-pad with FieldCommandCtrl_IsKeyPressed(const FKey*, widget), bool, a lookup in the widget's key maps that consumes
/// nothing, on the controller's command menu widget (FieldCommandCtrl_GetInputCommandMenuWidget(ctrl), null while the
/// widget is not up: the read would then fall back to the merged input, player 1's); d-pad Left/Right switch members
/// too unless the save data's byte at +0xB69BE is 1 (then they switch targets).
inline constexpr std::uintptr_t kKoPollGateCall = 0x14141C9F5;
inline constexpr std::array<std::uint8_t, 16> kMemberKnockedOutByTypeEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB6, 0xDA, 0xE8, 0x12, 0xA3, 0x39, 0xFF, 0x48, 0x8B};
inline constexpr platform::FunctionFact kMemberKnockedOutByType{L"Party_IsMemberKOByType_State13", 0x1412BD7A0, kMemberKnockedOutByTypeEntry};
inline constexpr std::array<std::uint8_t, 16> kCommandKeyPressedEntry{
    0x48, 0x83, 0xEC, 0x48, 0x4C, 0x8B, 0xC2, 0x48, 0x85, 0xD2, 0x74, 0x39, 0x48, 0x8B, 0x01, 0x48};
inline constexpr platform::FunctionFact kCommandKeyPressed{L"FieldCommandCtrl_IsKeyPressed", 0x141412250, kCommandKeyPressedEntry};
inline constexpr std::array<std::uint8_t, 16> kCommandInputWidgetEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x91, 0x04, 0x03, 0x00, 0x00, 0x85, 0xD2, 0x74, 0x5B};
inline constexpr platform::FunctionFact kCommandInputWidget{L"FieldCommandCtrl_GetInputCommandMenuWidget", 0x141421F50, kCommandInputWidgetEntry};
inline constexpr std::array<std::uint8_t, 16> kSaveDataGetEntry{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x05, 0xAD, 0xBF, 0xE9, 0x04, 0x48, 0x85, 0xC0, 0x75, 0x46};
inline constexpr platform::FunctionFact kSaveDataGet{L"SaveDataMgr_Get_cand", 0x14094D7B0, kSaveDataGetEntry};
inline constexpr std::uint32_t  kSaveDataSidewaysTargets = 0xB69BE;    // u8 1: d-pad Left/Right switch targets, not members
/// The d-pad's four FKey globals (each 24 bytes, its FName first), filled by the engine's static initialisers.
inline constexpr std::uintptr_t kKeyDPadUp    = 0x145920758;  // EKeys::Gamepad_DPad_Up
inline constexpr std::uintptr_t kKeyDPadDown  = 0x145920770;  // EKeys::Gamepad_DPad_Down
inline constexpr std::uintptr_t kKeyDPadRight = 0x145920788;  // EKeys::Gamepad_DPad_Right
inline constexpr std::uintptr_t kKeyDPadLeft  = 0x1459207A0;  // EKeys::Gamepad_DPad_Left

// --- The battle warps (RE-battle-warp.md §3) --------------------------------------------------------------------
// EncountMgr_BattleIn_DefaultLeaderAndTeleport's member loop fetches each present member with
// PartyManager_GetPlayerActor(PM, 0, type, 0) (E8 7A FB 63 00); a null answer is the game's own "no actor" branch, the
// next member. The battle end asks EncountMgr_BattleFinish_NeedsWarp_Victory / _Escape(unused, member, leader, groups)
// whether the member needs the warp back (E8 D3 1E 00 00, E8 55 21 00 00); false is the game's "close enough". Each
// call lies inside one code block.
inline constexpr std::uintptr_t kBattleInMemberCall      = 0x140FB0AD1;
inline constexpr std::uintptr_t kBattleFinishVictoryCall = 0x140FB17D8;
inline constexpr std::uintptr_t kBattleFinishEscapeCall  = 0x140FB17E6;
inline constexpr std::array<std::uint8_t, 16> kBattleFinishNeedsWarpVictoryEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x55};
inline constexpr platform::FunctionFact kBattleFinishNeedsWarpVictory{L"EncountMgr_BattleFinish_NeedsWarp_Victory",
                                                                      0x140FB36B0, kBattleFinishNeedsWarpVictoryEntry};
inline constexpr std::array<std::uint8_t, 16> kBattleFinishNeedsWarpEscapeEntry{
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xD8, 0xFE, 0xFF};
inline constexpr platform::FunctionFact kBattleFinishNeedsWarpEscape{L"EncountMgr_BattleFinish_NeedsWarp_Escape",
                                                                     0x140FB3940, kBattleFinishNeedsWarpEscapeEntry};

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
inline constexpr std::uint32_t  kCharacterAiModel      = 0x820;  // the character's own FEndAIModel (research/RE-interest-ladder-loop.md §7)
/// EndAIModel_CanRequestBehavior(model, u8 slot, u8 priority) -> bool: may an AI-model intention add a behaviour in
/// `slot` (0 = the move slot). Manual control adds its behaviours directly and never asks. Its first two instructions,
/// `mov r11, rsp` and `mov [r11+18h], r8b` (7 bytes, one aligned block, position-independent), read from IDA.
inline constexpr std::uintptr_t kAiModelCanRequestBehavior = 0x14064D790;
inline constexpr std::array<std::uint8_t, 7> kAiModelCanRequestBehaviorFirst{0x4C, 0x8B, 0xDC, 0x45, 0x88, 0x43, 0x18};
inline constexpr std::uint32_t  kCharacterFieldAi      = 0xC88;
/// The field AI controller's navigation-link arm (End AEndFieldAIController, RE-ladder-spot-offer.md §10.5): set within
/// a metre of a link's start on the member's path, with the link's id; the component's tick then starts the action by
/// it. Cleared at an unpossess. [CONFIRMED code]
inline constexpr std::uint32_t  kFieldAiLinkArmed      = 0x5630;  // u8
inline constexpr std::uint32_t  kFieldAiLinkId         = 0x5620;  // i32
/// Stock 4.18 movement (the reflection dump): ACharacter::CharacterMovement, UMovementComponent::Velocity,
/// UCharacterMovementComponent::MovementMode (EMovementMode: 1 walking, 2 nav walking, 3 falling, 5 flying, 6 custom).
inline constexpr std::uint32_t  kCharacterMovement     = 0x3E0;
inline constexpr std::uint32_t  kMovementVelocity      = 0x10C;  // FVector
inline constexpr std::uint32_t  kMovementMode          = 0x1B4;  // u8
inline constexpr std::uint32_t  kCharacter800          = 0x800;   // the EndCharacter800 interface the game's calls take
inline constexpr std::uint32_t  kCharacterActorMode    = 0x1200;  // the actor-mode object
inline constexpr std::uint32_t  kActorModeType         = 0xB8;
inline constexpr std::uint32_t  kActorModeMode         = 0xBA;
inline constexpr std::uint8_t   kActorTypePlayer       = 0;
inline constexpr std::uint8_t   kActorTypeParty        = 1;
inline constexpr std::uint8_t   kActorModeField        = 1;
inline constexpr std::uint8_t   kActorModeFieldCinema  = 3;
inline constexpr std::uint8_t   kActorModeCaution      = 4;
inline constexpr std::uint8_t   kActorModeBattle       = 5;
inline constexpr std::uint8_t   kActorModeCinema       = 7;
/// AEndFieldAIController: a stack of 15 request layers; the active layer (0 = the party's default layer, 1 = the
/// level's AI settings, 13 = the cinema copy, 16 = none) and its kind, the reflected FFieldAiType (the names and
/// the hold rule: players/field_hold.h; research/RE-fieldai-kinds.md). Aerith following Cloud in the field reads
/// layer 1 kind 11 ACCOMPANY [CONFIRMED live]; the level stages the members on layer 1 during gameplay
/// (STAY, FOLLOW_POSITION, LEAD, NONE) [CONFIRMED live].
inline constexpr std::uint32_t  kFieldAiActiveLayer    = 0xDF8;
inline constexpr std::uint32_t  kFieldAiActiveKind     = 0x55B8;
inline constexpr std::uint32_t  kFieldAiLayerRecords   = 0xE00;   // 15 records of 0x4C8 bytes; byte +0 = the kind (0 empty)
inline constexpr std::uint32_t  kFieldAiLayerBytes     = 0x4C8;
inline constexpr std::uint32_t  kFieldAiLayers         = 15;
/// The field control lock mask on the state manager ([root+0x138]): the reflected EndFieldExclusion bits (bit
/// 0x100000 while a push-button event holds the field; research/RE-script-release.md §3, §10.5). The first bit set hands the game's
/// leader to its AI, the last bit cleared returns it.
inline constexpr std::uint32_t  kRootStateManager      = 0x138;
inline constexpr std::uint32_t  kControlLockPushButton = 0x100000;  // _PushButtonAction: the push-button event (a switch, a
                                                                     // lever, a terminal), set only by
                                                                     // EventControl_Begin_HandLeaderToAI
inline constexpr std::uint32_t  kControlLockTreasure   = 0x4;       // _Treasure: a chest opened, an item picked up
/// Bit 21 of the same mask, EndFieldExclusion DisableController_MiniGame (research/RE-minigame-signal.md §2): held for the
/// whole of Fort Condor; switched by phase through a VR simulator or colosseum session (on/off 78 ShowColosseumMenu
/// held for the session), off in its fights.
inline constexpr std::uint32_t  kControlLockMiniGame   = 0x200000;
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
/// The battle's toad swaps (research/RE-toad-seat.md): the battle system's toad manager drains its queue each battle
/// tick (and once at the battle's end) into ApplyToad(mgr, u8 type) and CureToad(mgr, u8 type). Each copies the
/// status and gauges between the member and its toad actor, fades one out and the other in, and swaps the type's party
/// slot; only for the party leader does the swap move a controller (the leader setter onto the toad, and back). Both
/// open with three pushes (5 bytes), run on the game thread alone.
inline constexpr std::array<std::uint8_t, 16> kToadApplyEntry{
    0x40, 0x53, 0x55, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x0F, 0xB6, 0xFA, 0x48, 0x8B, 0xD9};
inline constexpr std::array<std::uint8_t, 16> kToadCureEntry{
    0x40, 0x53, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x0F, 0xB6, 0xFA, 0x48, 0x8B, 0xD9};
inline constexpr std::size_t            kToadSwapFirst = 5;  // the pushes' bytes
inline constexpr platform::FunctionFact kToadApply{L"BattleToadMgr_ApplyToad", 0x140A176A0, kToadApplyEntry};
inline constexpr platform::FunctionFact kToadCure{L"BattleToadMgr_CureToad", 0x140A17A50, kToadCureEntry};
/// BattlePlayerComp_AddStatus(comp, u8 status, u8 level, float scale, u8 force): the status sources' one setter. The time
/// is the status table's duration for the level (0-2; 64: no timer) times the scale (the game's own callers pass about
/// 1); force skips the resistance and stacking tests. Frog (59) on a party member's component queues the toad apply for
/// the next battle tick.
inline constexpr std::uint8_t kAddStatusLevelMiddle = 1;
inline constexpr std::array<std::uint8_t, 16> kAddStatusEntry{
    0x44, 0x88, 0x44, 0x24, 0x18, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55, 0x41, 0x57, 0x48, 0x8D, 0x6C};
inline constexpr platform::FunctionFact kAddStatus{L"BattlePlayerComp_AddStatus", 0x14096D430, kAddStatusEntry};
inline constexpr std::uint8_t           kStatusFrog = 59;
/// The formation's one writer, at the end of every player stream, reached by the Blueprint exec's tail jump.
inline constexpr std::array<std::uint8_t, 16> kFinishPlayerStreamEntry{
    0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41};
inline constexpr platform::FunctionFact kFinishPlayerStream{L"PartyManager_FinishPlayerStream_RebuildFormation", 0x1415F08F0,
                                                            kFinishPlayerStreamEntry};
inline constexpr std::uintptr_t kFinishPlayerStreamJump = 0x141B90183;  // E9 68 07 A6 FF, inside its block
/// The game's hand of its leader to its AI (the PLAYER slot cleared, the controller reconciled) and the return
/// (the slot set to the primary controller): the event's start and end, the field-action tick, and the field
/// control lock's first set and last clear (research/RE-script-release.md §1; three hand sites and two return sites,
/// one of each crossing a block, so the functions are heard at their entries). Both open position-independent:
/// the hand with `push rbx; sub rsp, 20h` (6 bytes, two instructions, the game thread alone runs it), the return
/// with `mov [rsp+8], rbx` (5).
inline constexpr std::array<std::uint8_t, 16> kHandLeaderToAiEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xE8, 0xE5, 0x13, 0x66, 0xFF, 0x48, 0x8B, 0xC8, 0x33, 0xD2};
inline constexpr std::size_t kHandLeaderToAiFirst = 6;
inline constexpr platform::FunctionFact kHandLeaderToAi{L"Party_HandLeaderToAI_ClearPlayerSlot", 0x140FF66D0, kHandLeaderToAiEntry};
inline constexpr std::array<std::uint8_t, 16> kReturnLeaderToPcEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x81, 0x38, 0x01, 0x00};
inline constexpr std::size_t kReturnLeaderToPcFirst = 5;
inline constexpr platform::FunctionFact kReturnLeaderToPc{L"Party_ReturnLeaderToPrimaryPC", 0x140FF6720, kReturnLeaderToPcEntry};
/// The reconcile (char800 vt+0x128: the PLAYER slot wins, else the AI for the mode) and the actor-type setter
/// (char800 vt+0x110), as the game's own hand-backs and the formation rebuild call them.
inline constexpr std::array<std::uint8_t, 16> kReconcileControllerEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48};
inline constexpr platform::FunctionFact kReconcileController{L"EndCharacter800_ReconcileActiveController", 0x1416450F0,
                                                             kReconcileControllerEntry};
inline constexpr std::array<std::uint8_t, 16> kSetActorTypeEntry{
    0x48, 0x8B, 0x81, 0x00, 0x0A, 0x00, 0x00, 0x88, 0x90, 0xB8, 0x00, 0x00, 0x00, 0x80, 0xFA, 0x02};
inline constexpr platform::FunctionFact kSetActorType{L"EndCharacter800_SetActorType", 0x14164DE50, kSetActorTypeEntry};

// --- A guide (a lead or a run to a mark): the level's waiters and the story's marker (RE-lead-arrival-wait.md,
// RE-navi-icon-target.md) ---
/// The level state manager ([root+0x138]) keeps one slot per level object and attribute (an object's attribute-4
/// slot holds its AI setting state); the active slots are a linked list (head at +0x67E1E8; a node's next at
/// kStateNodeNext and its slot index, into the array at +0xEE4A30, at kStateNodeSlotIndex). A slot: +0 the object FName, +8 the current state FName, +0x10 the
/// current FEndDataTableStateChange row. A row: StringArgumentList (+0x78, count +0x80; [2] = the arrival trigger of a
/// Lead/MoveToTarget/FollowActor setting), StateTriggerList (+0x98, count +0xA0; the triggers this state waits on, 8
/// read), NextStateChange (+0xA8, count +0xB0; FStrings of 16 bytes). [CONFIRMED code, read live from
/// outside the game: Tifa's Lead slot and the timer object waiting on her arrival]
inline constexpr std::uint32_t  kStateManagerActiveList  = 0x67E1E0;  // the list object (the walk stops back at it)
inline constexpr std::uint32_t  kStateManagerActiveHead  = 0x67E1E8;
inline constexpr std::uint32_t  kStateNodeNext           = 0x08;      // a list node's next node
inline constexpr std::uint32_t  kStateNodeSlotIndex      = 0x18;      // a list node's slot index (int32)
inline constexpr std::uint32_t  kStateManagerSlots       = 0xEE4A30;  // slot pointers, kStateManagerSlotCount of them
inline constexpr std::uint32_t  kStateManagerSlotCount   = 0x33F00;
inline constexpr std::uint32_t  kStateSlotObject         = 0x00;
inline constexpr std::uint32_t  kStateSlotState          = 0x08;
inline constexpr std::uint32_t  kStateSlotRow            = 0x10;
inline constexpr std::uint32_t  kStateRowEnterActions    = 0x40;  // EnterActionList (FName*, count +0x48): the actions started on entry
inline constexpr std::uint32_t  kStateRowStatus          = 0x50;  // the Status FString (count +0x58): "Lead", "MoveToTarget", "Stable", ...
inline constexpr std::uint32_t  kStateRowStringArgs      = 0x78;
/// A move setting's destination: StringArgumentList[1] of a MoveToTarget or Lead row names a field point
/// (FieldAISetting_MoveToTarget 0x1411663B0 looks it up and does not run without it; FieldAISetting_Lead_L1 the
/// same). For FollowActor it names the actor followed, for RouteMove the route's base name (points base+n).
/// [CONFIRMED code, RE-lead-arrival-wait.md §6]
inline constexpr std::uint32_t  kStateRowDestinationArg  = 1;
inline constexpr std::uint32_t  kStateRowIntArgs         = 0x88;  // IntegerArgumentList (int32*, count +0x90)
inline constexpr std::uint32_t  kStateRowTriggers        = 0x98;
inline constexpr std::uint32_t  kStateRowNextStates      = 0xA8;
inline constexpr std::uint32_t  kStateRowTriggersRead    = 8;
/// The field AI's handler table (+0xD88, one pointer per kind, FieldAI_CreateKindHandlers 0x140FBFA90 fills it in
/// kind order). The kinds whose arrival the level can wait on keep the member's level object FName and the setting's
/// state FName in their handler, the two values their arrival signal passes (FieldAI_NotifySettingArrived_Signal):
/// LEAD (7) at +0x154/+0x15C (FieldAI_K07Lead_OnMoveDone_Signal 0x1406C1BC0); the run to a mark (4, "MoveToTarget")
/// at +0x144/+0x14C (FieldAISetting_MoveToTarget writes them at +0x64/+0x6C of the record block that the kind's
/// enter copies to handler+0xE0; FieldAI_K04FollowPosition_OnMoveDone_Signal 0x1406BE390 and its goal-turn update
/// 0x1406BEE40 pass handler+324/+332) [CONFIRMED code]. A run a state-manager command writes
/// (StateMgrCmd_FieldAIRequest_Exec) names no object (0): nothing can wait on it.
inline constexpr std::uint32_t  kFieldAiHandlers         = 0xD88;
struct ArrivalHandlerNames
{
    std::uint8_t  kind;    // FFieldAiType (players/field_hold.h names it)
    std::uint32_t object;  // the level object FName in the kind's handler
    std::uint32_t state;   // the setting's state FName
};
inline constexpr ArrivalHandlerNames kArrivalHandlerNames[]{{7, 0x154, 0x15C}, {4, 0x144, 0x14C}};
/// The three state-trigger data tables (g_DT_StateTrigger; a row looked up through the table's virtual at +0x250
/// with the FName by value, as StateMgr_FireStateTrigger and StoryFlag_IsSet do) and the FEndDataTableStateTrigger
/// row: Target scope (+0x0C: 0 Self, 1 Location, 2 ResidentAndLocation, 3 SpecificObject), the direct sets
/// ObjectIDList (+0x30, count +0x38), AttributeNameList (+0x40), StateChangeIDList (+0x50), SendObject (+0x80, count
/// +0x88).
inline constexpr std::uintptr_t kStateTriggerTablesGlobal = 0x1458441F0;
inline constexpr std::uint32_t  kDataTableFindRowSlot    = 0x250;
inline constexpr std::uint32_t  kTriggerRowScope         = 0x0C;
// SpecialTypeList (uint8*, count +0x18): EEndStateTriggerSpecialType (1 EnvironmentAnimationLastFrame, 2 IgnoreDelay,
// 3 IgnoreSe) [CONFIRMED dump, EndGame_enums.hpp]
inline constexpr std::uint32_t  kTriggerRowSpecialTypes  = 0x10;
inline constexpr std::uint32_t  kTriggerRowObjects       = 0x30;
inline constexpr std::uint32_t  kTriggerRowAttributes    = 0x40;
inline constexpr std::uint32_t  kTriggerRowStates        = 0x50;
inline constexpr std::uint32_t  kTriggerRowSendObjects   = 0x80;
inline constexpr std::uint32_t  kTriggerRowDirectSetsMax = 32;
/// EnableBattleSnap (u8): non-zero makes a send of the trigger write a save record (a battle snap, an auto or safety
/// save) before it is queued or dispatched (StateTrigger_PreQueueBattleSnapSave, StateTrigger_ApplySaveDataType;
/// research/RE-send-signal.md).
inline constexpr std::uint32_t  kTriggerRowSaveStep      = 0x60;
/// ClearHistory (FName*, count +0x28): the triggers whose fired history a send clears in the save data, which story
/// conditions read; BackupSaveEndActionID (FName): the act a send's save step runs. [CONFIRMED dump +
/// research/RE-stuck-spot-repair.md]
inline constexpr std::uint32_t  kTriggerRowClearHistory  = 0x20;
inline constexpr std::uint32_t  kTriggerRowBackupAct     = 0x78;
/// StateMgr_SendSignal_Locked(FName trigger by value, FName sender, float delay, uint8 flags): the game's outside
/// sender of a state trigger, the call the walk-up exit sends a spot's end triggers with ((trigger, 0, -1.0f, 0)). It
/// takes its own lock, does nothing from field step 15 on, finds the row in the three trigger tables (an unknown name
/// touches nothing) and, for delay -1, queues it for the state manager's next tick (research/RE-send-signal.md). The
/// bytes stop before the 16th, which begins a RIP-relative load.
inline constexpr std::array<std::uint8_t, 15> kStateSendSignalEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40};
inline constexpr platform::FunctionFact kStateSendSignal{L"StateMgr_SendSignal_Locked", 0x140FD0570, kStateSendSignalEntry};
/// SaveData_GatherFieldBlock_IfValid(SaveSystem*, unused, Options*): the save-time gather of the live field block (the
/// save system, kDataSingletonGlobal, + kSaveFieldBlock) from the live level states, their act queues, the trigger history
/// and the party's transforms, as every save point and the field entry's snapshot do. It writes the block only (no retry
/// checkpoint, no disk, no flag); field init re-reads that block at an area change inside the same field, so a level
/// state repaired after the last gather comes back there unless the block is gathered again. Game thread; a zeroed
/// options buffer of kGatherOptionsBytes means "everything, the party from its actors"; does nothing unless the block is
/// present (+kFieldBlockPresent) and valid. The bytes stop at the block's offset (lea rbx, [rcx+0x50880]).
/// research/RE-field-block-regather.md. [CONFIRMED code]
inline constexpr std::array<std::uint8_t, 22> kGatherFieldBlockEntry{0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57,
                                                                     0x48, 0x83, 0xEC, 0x20, 0x48, 0x8D, 0x99, 0x80, 0x08, 0x05, 0x00};
inline constexpr platform::FunctionFact kGatherFieldBlock{L"SaveData_GatherFieldBlock_IfValid", 0x140B2BBB0, kGatherFieldBlockEntry};
inline constexpr std::uint32_t  kGatherOptionsBytes      = 0x440;
inline constexpr std::uint32_t  kSaveFieldBlock          = 0x50880;
inline constexpr std::uint32_t  kFieldBlockPresent       = 24;      // u8
/// The block's slot records (16 bytes each, at most 0x2000, the list ends at slot 0): i32 slot, i32 the state row's
/// UniqueIndex, u16 the first act id's index, u8 the act count (+10). A state row's UniqueIndex is at its +8.
inline constexpr std::uint32_t  kFieldBlockSlotRecords   = 216048;
inline constexpr std::uint32_t  kFieldBlockSlotRecordMax = 0x2000;
inline constexpr std::uint32_t  kFieldBlockRecordBytes   = 16;
inline constexpr std::uint32_t  kFieldBlockRecordActs    = 10;
inline constexpr std::uint32_t  kStateRowUniqueIndex     = 0x08;
/// Each slot's act sequence (SM + this + 8 * slot index): its running act's runner (+0x08, null when none) and its queue
/// of act names (+0x10, 32 FNames, the head first; None when empty). A rail camera act's runner (vtable kRailActVtable)
/// keeps the level object (+0x30) and the state it holds the rail for (+0x38); it ends once the object leaves that
/// state, and while its rail is not loaded it waits in the queue (research/RE-stuck-spot-repair.md). [CONFIRMED code]
inline constexpr std::uint32_t  kStateManagerActSequences = 0xD45230;
inline constexpr std::uint32_t  kActSequenceRunner        = 0x08;
inline constexpr std::uint32_t  kActSequenceQueue         = 0x10;
inline constexpr std::uintptr_t kRailActVtable            = 0x144B92280;
inline constexpr std::uint32_t  kRailActObject            = 0x30;
inline constexpr std::uint32_t  kRailActHoldState         = 0x38;
/// The three state-change data tables (g_DT_StateChange; a state row by its FName through the same virtual, as
/// StateSlot_SetState 0x141169C00 does when a slot steps to its next state). [CONFIRMED code]
inline constexpr std::uintptr_t kStateChangeTablesGlobal = 0x145843FB0;
/// The level's acts and act groups, each three data tables with the same row lookup: a state's EnterActionList names
/// either; ActSequence_QueueActionOrGroup 0x140FB6E00 tries the group tables first, a group being an FName list (+0x08,
/// count +0x10) of which its 32 accessors (0x1442A07E0) read at most 32 acts. An act row (FEndDataTableAction): Type
/// (+0x0C, i32, the runner kind ActSequence_CreateActRunner_ByActionType 0x140DAB480 switches on), StringArgumentList
/// (+0xC8, FString*, count +0xD0), IntegerArgumentList (+0xD8, i32*, count +0xE0), FloatArgumentList (+0xE8, float*,
/// count +0xF0). [CONFIRMED code, RE-lead-unlock.md §3, §5] The act tables and the Type field are
/// kTableAction, kActionRowType and kActionRowIntegers (below, with the talk gather's facts).
inline constexpr std::uintptr_t kActionGroupTablesGlobal  = 0x145839D60;
inline constexpr std::uint32_t  kActionGroupRowActions    = 0x08;
inline constexpr std::uint32_t  kActionGroupRowActionsMax = 32;
inline constexpr std::uint32_t  kActionRowStrings         = 0xC8;
inline constexpr std::uint32_t  kActionRowFloats          = 0xE8;
/// What an act does to the story besides its runner (FName arrays, count +8): the triggers its end sends
/// (EndStateTriggerList), the story flags its start and its end set (StartStoryFlag, EndStoryFlag), the location work
/// values its start writes (LocationWorkID) (Act_OnEnd_SetStoryFlags_FireEndTriggers 0x140DAADC0). [CONFIRMED dump + code,
/// research/RE-stuck-spot-repair.md]
inline constexpr std::uint32_t  kActionRowEndTriggers     = 0x68;
inline constexpr std::uint32_t  kActionRowStartFlags      = 0x78;
inline constexpr std::uint32_t  kActionRowEndFlags        = 0x88;
inline constexpr std::uint32_t  kActionRowLocationWork    = 0x98;
/// The AutoWalk act (Type 56, ActRunner56_AutoWalkType_*): a leash on the default party leader (player 1's character)
/// measured to Str[2] each frame while the slot of object Str[0] holds state Str[1]: walk-speed bands, past Int[8] cm
/// the look at Str[4] (when Int[6]), past Flt[0] cm the turn-back (when Int[4]). RE-lead-unlock.md §3.
inline constexpr std::int32_t   kActionTypeAutoWalk       = 56;
inline constexpr std::uint32_t  kAutoWalkStrObject        = 0;
inline constexpr std::uint32_t  kAutoWalkStrState         = 1;
inline constexpr std::uint32_t  kAutoWalkStrTarget        = 2;
inline constexpr std::uint32_t  kAutoWalkStrLookAt        = 4;
inline constexpr std::uint32_t  kAutoWalkIntTurnBack      = 4;
inline constexpr std::uint32_t  kAutoWalkIntLookAt        = 6;
inline constexpr std::uint32_t  kAutoWalkIntLookAtBand    = 8;
inline constexpr std::uint32_t  kAutoWalkFltTurnBack      = 0;
/// Each of those tables is a Square Enix data object (UEndDataObjectAction, vtable 0x144CA4C48; the act groups'
/// 0x144CA4778, the state changes' 0x144CDF988), not a reflected UDataTable: its rows by value in a TSet at +0x38 of
/// elements {FName key +0, row +8, hash links}, a UE 4.18 sparse array (platform/sparse_array.h; its Num at +0x40).
/// The FindRow (vt+0x250, UEndDataObjectAction__FindRow 0x141988B70) hashes into it and returns element + 8; the game's
/// own walk (vt+0x258 GetRowNames 0x141988BD0) reads the same bits. Rows are built at the asset's load; the slots are
/// emptied by UnInitDataTable. [CONFIRMED code, RE-data-table-rows.md]
inline constexpr std::uint32_t  kDataObjectRows           = 0x38;
inline constexpr std::uint32_t  kDataObjectRowCount       = 0x40;
inline constexpr std::uint32_t  kDataObjectElementRow     = 0x08;
inline constexpr std::uint32_t  kActionElementBytes       = 0x148;
inline constexpr std::int32_t   kDataObjectRowsMax        = 1 << 16;  // Ch. 4's act table: 6276 rows
/// The objective icon registry ([root+0x1F8]: a map of story step FName to a 96-byte record; the sparse array's data
/// at +8, count +0x10, 24-byte elements with the record at +8): a record's key (+8), its FEndDataTableStory row
/// (+0x18) and its "icon removed" byte (+0x29, 0 while the icon is shown). The story row's three icon slots:
/// IconOperationStatus (+0xA0, count +0xA8), TargetObjectID (+0x110, count +0x118), IconInvisibleFlag (+0x120, count
/// +0x128; a story flag that hides the slot); the fallback InvNaviIcon (flag +0x160, shown while ViewLocationID
/// +0x168 is set, target +0x17C). The first slot with a target and no set invisible flag is the one shown; its
/// target is resolved by name every frame (a field point, else the level object: a character). [CONFIRMED code, read
/// live: the Ch. 3 record targets opy030_Tifa]
inline constexpr std::uint32_t  kRootObjectiveIcons      = 0x1F8;
inline constexpr std::uint32_t  kObjectiveIconsData      = 0x08;  // the registry's array: data
inline constexpr std::uint32_t  kObjectiveIconsNum       = 0x10;  //   and count
inline constexpr std::uint32_t  kObjectiveIconRecordKey  = 0x08;
inline constexpr std::uint32_t  kObjectiveIconRecordRow  = 0x18;
inline constexpr std::uint32_t  kObjectiveIconRecordGone = 0x29;
inline constexpr std::uint32_t  kStoryRowIconStatus      = 0xA0;
inline constexpr std::uint32_t  kStoryRowIconTargets     = 0x110;
inline constexpr std::uint32_t  kStoryRowIconHideFlags   = 0x120;
inline constexpr std::uint32_t  kStoryRowInvNaviFlag     = 0x160;
inline constexpr std::uint32_t  kStoryRowInvNaviShown    = 0x168;
inline constexpr std::uint32_t  kStoryRowInvNaviTarget   = 0x17C;
inline constexpr std::uint32_t  kStoryRowIconSlots       = 3;
/// StoryFlag_IsSet(const FName*): the flag's row in the story-flag tables, then its bit in the save data.
inline constexpr std::array<std::uint8_t, 16> kStoryFlagIsSetEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48};
inline constexpr platform::FunctionFact kStoryFlagIsSet{L"StoryFlag_IsSet", 0x140B05060, kStoryFlagIsSetEntry};
/// The field points: a native manager at [root+0x140] holding the loaded levels' named points (actors register under
/// their first tag at BeginPlay, layout packs their TargetPoints; EndPlay removes them).
/// FieldPoint_FindByName(manager, const FName*) returns the point's FTransform (translation at +0x10) or null; a
/// pure hash probe on the game thread, the result copied at once (the table moves as levels stream). The objective
/// marker resolves its target through it first and draws at the record's shown position (+0x34, world cm).
/// [CONFIRMED code, RE-lead-arrival-wait.md §6, RE-navi-icon-target.md §1.3]
inline constexpr std::uint32_t  kRootFieldPoints         = 0x140;
inline constexpr std::uint32_t  kFieldPointLocation      = 0x10;
inline constexpr std::uint32_t  kObjectiveIconRecordAt   = 0x34;  // the shown position (FVector, world cm)
inline constexpr std::array<std::uint8_t, 16> kFieldPointFindByNameEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0x02, 0x48, 0x8D, 0x59, 0x08, 0x48, 0x8B, 0xCB};
inline constexpr platform::FunctionFact kFieldPointFindByName{L"FieldPoint_FindByName", 0x140FF89C0, kFieldPointFindByNameEntry};

// --- A scripted one-shot move written on a named character (research/RE-scripted-entry-cut.md) ----------
/// FieldAI_RequestEventMove_L6(FName actor by value, unused, params*): the game's writer of a scripted one-shot move
/// (kind 6 SINGLE_ANIMATION on field-AI layer 6) for a named character (an act's event move, type 18; an event's
/// placement). It finds the character through CharaRegistry_FindActorByName with the level FName at kNamedActorLevel,
/// returns 0 for a dying one, writes the request and re-selects the active layer, with no test of who possesses the
/// character. A possession change after the write (the field AI taking a character a player held) runs the request's
/// Exit, which reports it finished at once: a one-shot animation is cut. First instruction mov [rsp+8], rbx; the check
/// stops before the RIP-relative load of the registry. [CONFIRMED code]
inline constexpr std::uintptr_t                kEventMoveWrite      = 0x140FD0C10;
inline constexpr std::array<std::uint8_t, 16>  kEventMoveWriteEntry{0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
                                                                    0xEC, 0x20, 0x48, 0x8B, 0xD1, 0x49, 0x8B, 0xF8};
inline constexpr std::array<std::uint8_t, 5>   kEventMoveWriteFirst{0x48, 0x89, 0x5C, 0x24, 0x08};

// --- A held member's own story boxes (research/RE-held-member-story-boxes.md) ---------------------------
/// EndFieldStateTrigger_OnColliderBeginOverlap(fieldPoints, FName volume by value, AActor* collider): the story box's
/// enter, once per new box in a character's own sweep (the dispatcher's diff, game thread, world tick): records the
/// collider as an occupant, then sends the box's attribute-1 Status if the box is on and its group lists the collider.
/// The return is ignored by every caller. First instruction `mov [rsp+10h], rdx` (5 bytes, block-aligned, position-
/// independent); the end mirror takes the same arguments and only undoes a recorded occupant. [CONFIRMED code + entry
/// bytes, IDA]
inline constexpr std::uintptr_t kStoryBoxBegin = 0x14100CC70;
inline constexpr std::array<std::uint8_t, 5> kStoryBoxBeginFirst{0x48, 0x89, 0x54, 0x24, 0x10};
inline constexpr std::array<std::uint8_t, 16> kStoryBoxEndEntry{
    0x40, 0x55, 0x56, 0x57, 0x41, 0x55, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x40, 0x4C};
inline constexpr platform::FunctionFact kStoryBoxEnd{L"EndFieldStateTrigger_OnColliderEndOverlap", 0x14100D1B0, kStoryBoxEndEntry};
/// FieldStateMgr_FindTriggerByName_1F0(fieldPoints, const FName*): the actor registered under that first tag at its
/// BeginPlay (a TMap at +0x1F0, removed at EndPlay), or null. A pure hash probe.
inline constexpr std::array<std::uint8_t, 16> kFieldPointActorByTagEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0x02, 0x48, 0x8D, 0x99, 0xF0, 0x01, 0x00, 0x00};
inline constexpr platform::FunctionFact kFieldPointActorByTag{L"FieldStateMgr_FindTriggerByName_1F0", 0x140FF8A30,
                                                              kFieldPointActorByTagEntry};
/// The layout objects by box tag: g_Map_LayoutNodeName_ToObject, a TMap<FName, FName> of 24-byte pairs (value at +8),
/// probed with TMap_FNameKey24_FindIndex(map, int* out, FName by value) -> out (-1: none).
inline constexpr std::uintptr_t kLayoutObjectByNodeMap = 0x1453A7EE0;
inline constexpr std::uint32_t  kLayoutObjectPairBytes = 24;
inline constexpr std::uint32_t  kLayoutObjectPairValue = 8;
inline constexpr std::array<std::uint8_t, 16> kNameMapFindIndexEntry{
    0x48, 0x89, 0x74, 0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10, 0x8B, 0x41, 0x08, 0x48, 0x8B, 0xFA};
inline constexpr platform::FunctionFact kNameMapFindIndex{L"TMap_FNameKey24_FindIndex", 0x14067E200, kNameMapFindIndexEntry};
/// An ObjectLayout row's OverlapActorGroup (FName; 0 = none) and the ActorGroup tables (three, DataTableSet3_FindRowByName)
/// whose row holds the ActorList (TArray<FName>: data +0x08, Num +0x10; the game reads at most 32).
inline constexpr std::uint32_t  kLayoutRowActorGroup     = 0x28;
inline constexpr std::uintptr_t kTableActorGroup         = 0x145839DC0;
inline constexpr std::uint32_t  kActorGroupRowActors     = 0x08;
inline constexpr std::uint32_t  kActorGroupRowActorsNum  = 0x10;
inline constexpr std::int32_t   kActorGroupActorsRead    = 32;
/// A character's own overlap component (the sweep the dispatcher diffs) and its stored key set: a TArray<FName> with an
/// inline allocator (inline data +0x108, heap data +0x208 when non-null, Num +0x210): the boxes it is inside.
inline constexpr std::uint32_t kCharacterOverlapComponent = 0x1868;
inline constexpr std::uint32_t kOverlapKeysInline         = 0x108;
inline constexpr std::uint32_t kOverlapKeysHeap           = 0x208;
inline constexpr std::uint32_t kOverlapKeysNum            = 0x210;
/// Whether the box sends anything (the enter's own reads): StateMgr_GetSlotIndex_ByObjectName(stateManager, const FName*,
/// attribute) -> the slot index (attribute 1: the trigger slot, null when the object has none); its off byte +0x170
/// on the slot's linked base entry ([slot+0x18] then its first pointer) when there is one, else on the slot (1 = off);
/// StateMgr_IsObjectBaseSlotLoaded(stateManager, attribute-0 index).
inline constexpr std::array<std::uint8_t, 16> kStateSlotIndexEntry{
    0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48};
inline constexpr platform::FunctionFact kStateSlotIndex{L"StateMgr_GetSlotIndex_ByObjectName", 0x14113E030, kStateSlotIndexEntry};
inline constexpr std::array<std::uint8_t, 16> kStateBaseSlotLoadedEntry{
    0x8B, 0x41, 0x28, 0x3B, 0x41, 0x54, 0x74, 0x4E, 0x4C, 0x8B, 0x49, 0x60, 0x4C, 0x8D, 0x41, 0x58};
inline constexpr platform::FunctionFact kStateBaseSlotLoaded{L"StateMgr_IsObjectBaseSlotLoaded", 0x1406CE7D0,
                                                             kStateBaseSlotLoadedEntry};
inline constexpr std::uint32_t kStateSlotLinkedBase = 0x18;
inline constexpr std::uint32_t kStateSlotOffByte    = 0x170;
/// A trigger box's collision component (ATriggerBase::CollisionComponent, a UBoxComponent for ATriggerBox), its
/// ComponentToWorld (rotation quaternion kComponentWorldRotation 0x1A0, below with the duct facts; translation
/// kComponentWorldLocation 0x1B0; scale +0x1C0) and UBoxComponent::BoxExtent.
/// [CONFIRMED UE4SS header dump: Engine.hpp ATriggerBase, UBoxComponent; FieldPointMgr_AddActorByTag0 reads +0x1A0]
inline constexpr std::uint32_t kTriggerCollisionComponent = 0x378;
inline constexpr std::uint32_t kComponentWorldScale       = 0x1C0;
inline constexpr std::uint32_t kBoxComponentExtent        = 0x710;

// --- The battle target cursor (RE-target-cursor-two.md) --------------------------------------------------------
/// The one cursor widget's pointer on the battle system (outside the target fields), and the HUD keys the cursor
/// driver writes each frame: the enemy bar manager's (EndUISystem[27]) displayed target (a weak pointer pair) and
/// the off-screen icon manager's (EndUISystem[38]) target name (an FString at +0x88, its FName at +0x98).
inline constexpr std::uint32_t  kBattleSysCursorWidget        = 0x10;
inline constexpr std::uint32_t  kEnemyBarSlot                 = 27;  // EndUISystem[27]
inline constexpr std::uint32_t  kEnemyBarDisplayedTarget      = 0xD8;
inline constexpr std::uint32_t  kEnemyBarDisplayedTargetBytes = 0x10;
inline constexpr std::uint32_t  kOutsideIconSlot              = 38;  // EndUISystem[38]
inline constexpr std::uint32_t  kOutsideIconTargetName        = 0x88;
inline constexpr std::uint32_t  kOutsideIconTargetNameBytes   = 0x18;
inline constexpr std::uint32_t  kOutsideIconHighlightName     = 0x98;  // the FName the tick compares (8 bytes; +0x10 of the range)
// --- The off-screen enemy arrows' tick (RE-outside-icons-per-view.md) ---------------------------------------------
/// EndUISlot38_BattleEnemyOutsideIcons_Tick(manager): syncs the pooled arrows with the enemy targets, places each twice
/// through the arrow widget's player's controller (two lookups; the first crosses a block) and picks its picture. Its
/// only per-arrow state is the timer at +0x14; Place steps it by AEndGameMode::SystemDeltaTimeSeconds.
inline constexpr std::array<std::uint8_t, 16> kOutsideIconTickEntry{
    0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0x88, 0x48, 0x81, 0xEC};
inline constexpr platform::FunctionFact kOutsideIconTick{L"EndUISlot38_BattleEnemyOutsideIcons_Tick", 0x1413E4240,
                                                         kOutsideIconTickEntry};
inline constexpr std::array<std::uintptr_t, 2> kOutsideIconContextCalls{0x1413E471E, 0x1413E4742};  // the first crosses
/// The manager: the gate byte, and its set of arrows (a sparse array at +0x08, platform/sparse_array.h; 0x30 per
/// element: +0 the weak widget, +0x14 the timer, +0x18 the source at creation).
inline constexpr std::uint32_t kOutsideIconGate         = 0x80;
inline constexpr std::uint32_t kOutsideIconSetData      = 0x08;
inline constexpr std::uint32_t kOutsideIconStride       = 0x30;
inline constexpr std::uint32_t kOutsideIconTimer        = 0x14;
inline constexpr std::uint32_t kOutsideIconSource       = 0x18;
inline constexpr std::uint32_t kGameModeHudStep         = 0x778;  // AEndGameMode::SystemDeltaTimeSeconds (float)
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

// --- The world around two views (research/RE-camera-fades.md) ------------------------
/// EndCharacter_ApplyLeaderCameraFade(char, u8 enable, const FName* reason, u8 force, u8 forceOn): adds (enable 0) or
/// removes (enable 1) a named reason on the character's list of reasons not to near-fade; with any reason listed its
/// mesh's render near-fade is off, with none it is on at the EndParam distances. The leader change lists "Default" on
/// the new leader; force 1 skips the leader test; forceOn 1 turns the fade on whatever the list holds (never used here).
inline constexpr std::array<std::uint8_t, 16> kApplyCameraFadeEntry{
    0x40, 0x55, 0x53, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x60};
inline constexpr platform::FunctionFact kApplyCameraFade{L"EndCharacter_ApplyLeaderCameraFade", 0x14114F5B0, kApplyCameraFadeEntry};
/// FName::FName(const TCHAR*, EFindName): FNAME_Add (1) makes the name if it is new.
inline constexpr std::array<std::uint8_t, 16> kNameFromWideEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9, 0x48, 0x89, 0x54};
inline constexpr platform::FunctionFact kNameFromWide{L"FName::FName(const TCHAR*)", 0x141CE8700, kNameFromWideEntry};
inline constexpr std::int32_t  kNameAdd                   = 1;      // EFindName::FNAME_Add
inline constexpr std::uint32_t kCharacterMesh             = 0x3D8;  // ACharacter::Mesh
inline constexpr std::uint32_t kCharacterNoFadeReasons    = 0xFC0;  // AEndCharacter: TArray<FName> data; Num at +0xFC8
inline constexpr std::uint32_t kCharacterNoFadeReasonsNum = 0xFC8;
inline constexpr std::uint32_t kMeshNearFarFadeEnabled    = 0x8B0;  // u8 on the skinned mesh: the render near-fade is on

// --- Animation detail judged from two cameras (research/RE-anim-two-views.md) --------
/// The judge EndSkelMeshComp_AnimOptimizationEval(mesh, bool* forceOut, mesh** leadOut, bool cameraCut), slot +0xA48
/// of UEndSkeletalMeshComponent's and UEndSwitchableSkeletalMeshComponent's vtables (the only two slots holding it),
/// called serially on the game thread by the allocator's Update, the slot +0x50 of its vtable, at the world's
/// pre-actor tick. Update reads player 1's camera manager into the snapshot, then judges every registered mesh.
inline constexpr std::array<std::uint8_t, 16> kAnimJudgeEntry{
    0x40, 0x55, 0x53, 0x41, 0x54, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00};
inline constexpr platform::FunctionFact kAnimJudge{L"EndSkelMeshComp_AnimOptimizationEval", 0x1407E0ED0, kAnimJudgeEntry};
inline constexpr platform::SlotFact kAnimJudgeSlot{L"UEndSkeletalMeshComponent anim judge (slot +0xA48)", 0x144B51D68, kAnimJudge};
inline constexpr platform::SlotFact kAnimJudgeSwitchableSlot{L"UEndSwitchableSkeletalMeshComponent anim judge (slot +0xA48)",
                                                             0x144B51230, kAnimJudge};
inline constexpr std::array<std::uint8_t, 16> kAnimBudgetUpdateEntry{
    0x40, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xC1, 0x48, 0x81, 0xEC, 0xF8, 0x00};
inline constexpr platform::FunctionFact kAnimBudgetUpdate{L"AnimBudgetAllocator_Update_SE", 0x143B41540, kAnimBudgetUpdateEntry};
inline constexpr platform::SlotFact kAnimBudgetUpdateSlot{L"FAnimationBudgetAllocator::Update (slot +0x50)", 0x144F8A980,
                                                          kAnimBudgetUpdate};
inline constexpr std::uint32_t kComponentOwner            = 0xE0;    // UActorComponent::OwnerPrivate
inline constexpr std::uint32_t kComponentWorldLocation    = 0x1B0;   // USceneComponent::ComponentToWorld's translation
inline constexpr std::uint32_t kMeshBudgetAllocator       = 0x1098;  // USkeletalMeshComponentBudgeted: the allocator
inline constexpr std::uint32_t kMeshAnimDistance          = 0x14A4;  // UEndSkeletalMeshComponent: the judge's distance (cm)
inline constexpr std::uint32_t kMeshAnimAngle             = 0x14A8;  //   and angle (radians), written only by the judge
inline constexpr std::uint32_t kAllocatorMeshCount        = 0x20;    // the allocator's registered meshes (Num)
inline constexpr std::uint32_t kAllocatorSnapshot         = 0xDC;    // predicted location, forward, speed, turn (32 bytes)
inline constexpr std::uint32_t kCameraManagerPovLocation  = 0x400;   // APlayerCameraManager: the cached POV location
inline constexpr std::uint32_t kCameraManagerPovRotation  = 0x40C;   //   and rotation (pitch, yaw, roll degrees)
inline constexpr std::uint32_t kCameraManagerCutFlags     = 0x1E18;  // AEndPlayerCameraManager: bit 0x400 = a camera cut
inline constexpr std::uint32_t kCameraManagerCutBit       = 0x400;
inline constexpr std::uint32_t kCharacterKbd              = 0x1A90;  // AEndCharacter: the KineDriver + Bonamik component
inline constexpr std::uint32_t kKbdBonamiks               = 0x428;   //   its Bonamik array (data), the first one's mode below
inline constexpr std::uint32_t kBonamikUpdateMode         = 0x1C9C;  //   0 normal; 1 reduced (out of the camera's cone)

// --- NPCs near a further player: CharaPop (research/RE-npc-near-player-two.md) -------
/// CharaPop_SetEntryMinLODLevel_ByName(const FName* objectName, u8 level, bool evaluateNow) -> found: the entry's
/// minimum level (3 keeps it awake, shown and fully animated; 0 clears it), the setter event scripts and field-AI
/// animations use; off the game thread it posts itself to the game thread.
inline constexpr std::array<std::uint8_t, 16> kCharaPopSetMinLevelEntry{
    0x48, 0x81, 0xEC, 0x98, 0x00, 0x00, 0x00, 0x80, 0x3D, 0x72, 0xCC, 0x85, 0x04, 0x00, 0x48, 0x8B};
inline constexpr platform::FunctionFact kCharaPopSetMinLevel{L"CharaPop_SetEntryMinLODLevel_ByName", 0x140FC8A20,
                                                             kCharaPopSetMinLevelEntry};
/// The spawn score's distance part, (evaluator, record) -> int: measured from the override centre when its byte is
/// set, else from the party leader. Slot +8 of the distance evaluator's vtable (its only user is the evaluate list).
inline constexpr std::array<std::uint8_t, 16> kCharaPopDistanceScoreEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x81, 0xEC, 0x90, 0x00};
inline constexpr platform::FunctionFact kCharaPopDistanceScore{L"CharaPopEval_Distance_Evaluate", 0x140F81160,
                                                               kCharaPopDistanceScoreEntry};
inline constexpr platform::SlotFact kCharaPopDistanceScoreSlot{L"CharaPop distance evaluator (slot +8)", 0x144BA97C8,
                                                               kCharaPopDistanceScore};
inline constexpr std::uint32_t kRootCharaPop             = 0x148;    // g_EndGameRoot: the CharaPop manager
inline constexpr std::uint32_t kCharaPopRecordsHeap      = 0x40650;  // the 1024 records, when moved to the heap
inline constexpr std::uint32_t kCharaPopRecordsInline    = 0x4650;   //   else inline here
inline constexpr std::int32_t  kCharaPopRecordCount      = 1024;
inline constexpr std::uint32_t kCharaPopRecordBytes      = 0xF0;
inline constexpr std::uint32_t kCharaPopRecordManager    = 0x00;     // record: its manager
inline constexpr std::uint32_t kCharaPopRecordState      = 0x40;     //   state (3 fade-in, 4 visible, 5 hiding, 6 hidden, 7 showing, 8 despawn)
inline constexpr std::uint32_t kCharaPopRecordName       = 0x64;     //   object name (FName): the min-level setter's key
inline constexpr std::uint32_t kCharaPopRecordCategory   = 0x74;     //   1 hides by distance, 3 enemy
inline constexpr std::uint32_t kCharaPopRecordInUse      = 0x7C;
inline constexpr std::uint32_t kCharaPopRecordLevel      = 0xC8;     //   level last evaluated (1 Light, 2 Middle, 3 Heavy)
inline constexpr std::uint32_t kCharaPopRecordMinLevel   = 0xC9;
inline constexpr std::uint32_t kCharaPopRecordActor      = 0xD0;
inline constexpr std::uint32_t kCharaPopFov              = 0x4919C;  // the cone's half-angle (degrees)
inline constexpr std::uint32_t kCharaPopLightDraw        = 0x491BC;  // Light: draw, in-camera, out-of-camera, should-tick
inline constexpr std::uint32_t kCharaPopLightInCamera    = 0x491DC;
inline constexpr std::uint32_t kCharaPopLightOutCamera   = 0x491FC;
inline constexpr std::uint32_t kCharaPopLightShouldTick  = 0x4921C;
inline constexpr std::uint32_t kCharaPopMiddleDraw       = 0x4923C;  // Middle: the same four
inline constexpr std::uint32_t kCharaPopMiddleInCamera   = 0x4925C;
inline constexpr std::uint32_t kCharaPopMiddleOutCamera  = 0x4927C;
inline constexpr std::uint32_t kCharaPopMiddleShouldTick = 0x4929C;
inline constexpr std::uint32_t kCharaPopEnemyInCameraAdd = 0x492DC;
inline constexpr std::uint32_t kCharaPopOverrideDraw     = 0x49318;  // override: the enable byte, the float at +4
inline constexpr std::uint32_t kCharaPopOverrideInCamera = 0x49338;
inline constexpr std::uint32_t kCharaPopOverrideOutCamera = 0x49358;
inline constexpr std::uint32_t kCharaPopCentre           = 0x498B0;  // override centre: the byte, the FVector at +4 (16 bytes)
inline constexpr std::uint32_t kCharaPopCentreBytes      = 0x10;
inline constexpr std::uint32_t kCharaPopEvaluatorScore   = 0x08;     // the distance evaluator's last non-zero score

// --- The NPCs' senses and their look at a passer-by (research/RE-npc-reactions.md, pass S-RMK-NPCREACT) -------------
// The grid's code site and bytes are in world/npc_senses_code.h. The battle test is the one the grid's battle heat
// map makes (EndInterMgr_BuildBattleHeat 0x1406B73A0): [[g_EndGameRoot+0x170]+0xE2A9] == 2.
inline constexpr std::uint32_t kRootFieldBattleState = 0xE2A9;  // u8 on kEndGameRootCharaOwner's object: 2 in battle
inline constexpr std::uint8_t  kFieldBattleStateBattle = 2;

// --- A player's character brought into the fight (research/RE-arena-warp.md) ----------------------------------------
// BattleChar_WarpInFrontOfPartyLeader(comp, float dist) -> moved: the game's in-battle recovery warp of one member (its
// revive and its leader switch call it for a character outside the battle area): a navmesh point `dist` along the party
// leader's facing, then EndCharacter_BattleTeleport_FadeIn; false (nothing moved) for the party leader itself or no
// point. BattleCycleLeader_IsCharInsideBattleArea(sys, char): the leader switch's "inside" test (the character's nav
// area in a battle area of an active encounter group). The leader switch's gates: the battle system's no-recovery byte
// (+0x3E8) clear, its +0xDF18 non-zero, the component's action state (+0x611) not KO (13), and after the warp a running
// field action (char+0xE58 -> +0x159) cut short. The mod adds two stricter guards of its own: no running field action
// (refused rather than cut short) and the leader switch's "may the leader be left" rule (RE-battle-switch.md: state 2
// only with flags +0x168 & 0x81), so a member mid-action is not pulled out of it.
// The distance: the GameParam row BattleTeleportNavDistanceChangeCharacter (its FName cached by the game at
// 0x1459C5278 once the leader switch first ran; the three GameParam tables at 0x145843650, each looked up through the
// table's vt+0x250 as the game does; ParamFloat at row +0xC), else the game's default 300.
inline constexpr std::array<std::uint8_t, 16> kArenaWarpEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x01};
inline constexpr platform::FunctionFact kArenaWarp{L"BattleChar_WarpInFrontOfPartyLeader", 0x1408A76D0, kArenaWarpEntry};
inline constexpr std::array<std::uint8_t, 16> kInsideBattleAreaEntry{
    0x40, 0x55, 0x53, 0x41, 0x55, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x48, 0xEB, 0xFF, 0xFF, 0xB8};
inline constexpr platform::FunctionFact kInsideBattleArea{L"BattleCycleLeader_IsCharInsideBattleArea", 0x1409C0030,
                                                          kInsideBattleAreaEntry};
/// The field encounter, per group (research/RE-field-battle-start-seats.md): EncountGroup_TickStateMachine(group, dt), once
/// per group from the field encounter tick on the game thread; the group's state byte (0 idle, 1 caution, 2 engage,
/// 3 battle, 4 escaping, 5 escaped, 6 ended), its own name and its battle area's name (FNames). The area test
/// (character, the area's FName by value, kind 0 = any, a probe vector) reads the character's own nav-area record.
inline constexpr std::array<std::uint8_t, 16> kEncountGroupTickEntry{
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57, 0x48, 0x8D, 0xAC};
inline constexpr platform::FunctionFact kEncountGroupTick{L"EncountGroup_TickStateMachine", 0x140F9C610, kEncountGroupTickEntry};
inline constexpr std::size_t            kEncountGroupTickFirst = 5;  // mov [rsp+20h], rbx
inline constexpr std::array<std::uint8_t, 16> kCharInBattleAreaOfGroupEntry{
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41};
inline constexpr platform::FunctionFact kCharInBattleAreaOfGroup{L"EncountMgr_IsCharInBattleAreaOfGroup", 0x140FB24F0,
                                                                 kCharInBattleAreaOfGroupEntry};
/// The group's update (group, dt), every group every tick, waiting ones included; it runs the step only in lifecycle 1
/// (0 waiting: every counted member's actor spawned, with its battle component, and not hidden by reason 11 or 14).
/// Its first two instructions are copied (mov rax, rsp ; mov [rax+10h], rbx: none of 5 bytes alone).
inline constexpr std::array<std::uint8_t, 16> kEncountGroupUpdateEntry{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55};
inline constexpr platform::FunctionFact kEncountGroupUpdate{L"EncountGroup_Update_LeaderAreaRequests", 0x140F9B400, kEncountGroupUpdateEntry};
inline constexpr std::size_t            kEncountGroupUpdateFirst = 7;
/// The member test (actor): 1 when the actor is hidden by reason 11 or 14. Only on a character (the update tests that first).
inline constexpr std::array<std::uint8_t, 16> kEncountMemberHiddenEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x75, 0x08, 0x32, 0xC0};
inline constexpr platform::FunctionFact kEncountMemberHidden{L"EncountMember_ActorHasReason11or14", 0x140F9ECD0, kEncountMemberHiddenEntry};
inline constexpr std::uint32_t kEncountGroupLifecycle   = 0x11;
inline constexpr std::uint32_t kEncountGroupMemberCount = 0x50;  // u32
inline constexpr std::uint32_t kEncountGroupMembers     = 0x58;  // up to 16 record pointers, inline
inline constexpr std::uint32_t kEncountGroupMaxMembers  = 16;
inline constexpr std::uint32_t kEncountGroupMemberActor = 0x08;  // vtable byte offset (slot 1): actor (group, int member), the state manager's
inline constexpr std::uint32_t kEncountMemberName       = 0x2C;  // the enemy object's FName
inline constexpr std::uint32_t kEncountMemberOut        = 0x4B;  // non-zero: the member no longer counts
inline constexpr std::uint32_t kEncountGroupState    = 0x12;
inline constexpr std::uint32_t kEncountGroupName     = 0x24;
inline constexpr std::uint32_t kEncountGroupAreaName = 0x2C;
inline constexpr std::uint32_t  kBattleNoRecovery       = 0x3E8;   // u8 on the battle system: the game warps no member
inline constexpr std::uint32_t  kBattleRecoveryReady    = 0xDF18;  // u32 on the battle system: non-zero for the warp
inline constexpr std::uint32_t  kBattleCompFlags        = 0x168;   // the battle component's flags (0x81: may leave)
inline constexpr std::uint8_t   kBattleActionLocked     = 2;     // RE-battle-switch.md: the leader may not be left
inline constexpr std::uint8_t   kBattleCompMayLeave     = 0x81;  //   ... unless these flags are set
inline constexpr std::uintptr_t kNameWarpDistanceCache  = 0x1459C5278;  // FName BattleTeleportNavDistanceChangeCharacter
inline constexpr std::uintptr_t kTableGameParam         = 0x145843650;  // three UDataTable*
inline constexpr std::uint32_t  kTableFindRowSlot       = 0x250;
inline constexpr std::uint32_t  kGameParamFloat         = 0xC;
inline constexpr float          kArenaWarpDefaultCm     = 300.0f;
/// Dash (L3 on a pad): the action key the game's field move polls for the sprint; in battle it does nothing.
inline constexpr std::uintptr_t kDashKey = 0x145903BA8;
/// EndAIIntentionCuriosity_Update(intention, context): an NPC's look at the most interesting character it sees within
/// its reach; slot 6 of the Curiosity intention's vtable 0x144B24508.
inline constexpr std::array<std::uint8_t, 16> kCuriosityUpdateEntry{
    0x40, 0x55, 0x56, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0x60, 0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC};
inline constexpr platform::FunctionFact kCuriosityUpdate{L"EndAIIntentionCuriosity_Update", 0x140652DF0,
                                                         kCuriosityUpdateEntry};
inline constexpr platform::SlotFact kCuriosityUpdateSlot{L"Curiosity intention (slot 6)", 0x144B24538, kCuriosityUpdate};
inline constexpr std::uint32_t kCuriositySetting       = 0x10;  // intention: its settings (+0x30 f32 the interest threshold)
inline constexpr std::uint32_t kCuriosityLooking       = 0x18;  //   u8: a look is running
inline constexpr std::uint32_t kCuriosityTargetSerial  = 0x34;  //   the target's weak serial (+0x30 its index); 0 = none
inline constexpr std::uint32_t kCuriosityInterestThreshold = 0x30;
inline constexpr std::uint32_t kAiContextCharacter     = 0x00;  // the update's context: the NPC
inline constexpr std::uint32_t kAiContextBoard         = 0x10;  //   its blackboard (the cognition map at +0x48)
inline constexpr std::uint32_t kBoardCognitions        = 0x48;  // a TSparseArray (platform/sparse_array.h)
inline constexpr std::uint32_t kCognitionBytes         = 48;    // an entry: the actor (+0), its last seen place (+8),
inline constexpr std::uint32_t kCognitionTimer         = 0x14;  //   time since last seen (0 = seen this update),
inline constexpr std::uint32_t kCognitionInterest      = 0x18;  //   interest,
inline constexpr std::uint32_t kCognitionExperience    = 0x1C;  //   experience (100 after a look)

// --- EnvironmentSet: actors hidden by the ground under the leader (research/RE-picture-variants.md §2, §5, §9, §10) --
// Square Enix's EnvironmentSet manager traces down from the party leader each frame, takes the hit surface's material
// as the area's set, and hides (reason 13, world-wide) the actors the field's EnvironmentSetWrapper row lists for that
// set. Both calls below are in EnvSetMgr_TickEvaluate_LeaderDownTrace 0x14165FCA0 (game thread, once per world tick).
/// IssueDownTrace(unused, const {FName tag; FVector loc}*, unused, unused, FTraceHandle* out): stock async line trace,
/// object channel 16, 150 m down. Its one call crosses a block.
inline constexpr std::array<std::uint8_t, 16> kEnvSetIssueTraceEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55};
inline constexpr platform::FunctionFact kEnvSetIssueTrace{L"EnvSetMgr_IssueDownTrace_Ch16", 0x141661630,
                                                          kEnvSetIssueTraceEntry};
inline constexpr std::uintptr_t kEnvSetIssueTraceCall = 0x14166025E;
/// ReadTraceResult(mgr, const FTraceHandle*, block*) -> 1 when a set was built into the block. The handle is valid only
/// the frame after it was issued; no hit falls back to the save data's last set name (written on every hit).
inline constexpr std::array<std::uint8_t, 16> kEnvSetReadResultEntry{
    0x4C, 0x8B, 0xDC, 0x55, 0x56, 0x57, 0x41, 0x56, 0x49, 0x8D, 0xAB, 0x48, 0xFE, 0xFF, 0xFF, 0x48};
inline constexpr platform::FunctionFact kEnvSetReadResult{L"EnvSetMgr_ReadTraceResult_ResolveSet", 0x14165E990,
                                                          kEnvSetReadResultEntry};
inline constexpr std::uintptr_t kEnvSetReadResultCall = 0x14165FE55;
/// EnvSetBlock_Reset(block): empties the lists and ids, keeps the allocations.
inline constexpr std::array<std::uint8_t, 16> kEnvSetBlockResetEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xFF, 0x48, 0x8B, 0xD9, 0x48};
inline constexpr platform::FunctionFact kEnvSetBlockReset{L"EnvSetBlock_Reset", 0x1411FF5E0, kEnvSetBlockResetEntry};
inline constexpr std::uintptr_t kEnvSetManagerGlobal  = 0x1457E9A90;  // the manager (for the reading only)
inline constexpr std::uint32_t  kEnvSetMgrSetIds      = 0x40;   // the applied set's three ids (FName each, +0x48, +0x50)
inline constexpr std::uint32_t  kEnvSetMgrHoldMode    = 0x858;  // u8: hold mode (the block is replaced after the read)
inline constexpr std::uint32_t  kEnvSetMgrLookupLevels = 0x708; // TSet<FName>: the levels the apply looks actors up in
inline constexpr std::uint32_t  kEnvSetBlockBytes     = 0x1D0;  // a set block (zero-filled = constructed)
inline constexpr std::uint32_t  kEnvSetBlockLevels    = 0x08;   //   TArray<FName> LevelList (num +0x10)
// TArray<u8>, one byte per LevelList entry: 0 shown, 1 hidden (collision off), 2 hidden (collision kept). The slot-4
// call passes it as r9 (`lea r9,[rbp-0x78]` = the names' r8 + 0x10, 0x1416600FC; RE-world-streaming-levers.md §4.4).
inline constexpr std::uint32_t  kEnvSetBlockLevelsHidden = 0x18;
inline constexpr std::uint32_t  kEnvSetBlockIds       = 0x28;   //   the set's three ids
inline constexpr std::uint32_t  kEnvSetBlockActors    = 0x40;   //   {FName, u32 Hidden} x 32 inline,
inline constexpr std::uint32_t  kEnvSetBlockActorsHeap = 0x1C0; //   the heap data when grown, num +0x1C8, max +0x1CC
inline constexpr std::uint32_t  kEnvSetBlockActorsNum = 0x1C8;
inline constexpr std::uint32_t  kEnvSetBlockActorsMax = 0x1CC;

// --- The world's levels: what loads and shows them (research/RE-world-streaming-players.md, pass S-RMK-STREAM; the call
// sites, argument shapes and the load bits checked by hand in IDA) ---------------------------------------------
/// EnvSetMgr_TickEvaluate_LeaderDownTrace's two calls on a change of player 1's area set: the set's level list for the
/// visibility slot 4 (`EnvSet_RequestLevelList_cand(u8 slot, int prio, TArray<FName>* names, TArray<u8>* unvisible)`,
/// the bytes parallel to the names and possibly null; the callee sorts the names in place), and the area's planner level
/// set into residency slot 32 (`PlannerLevelSetSlots_SetSlot_LoadRelease(holder, slot, FName id)`: the loader adds the
/// new set, then releases the old). The first call's five bytes cross a 16-byte block (placed from the game thread).
inline constexpr std::uintptr_t kEnvSetLevelListCall   = 0x14166010C;  // E8 2F F5 07 00
inline constexpr std::uintptr_t kEnvSetPlannerSlotCall = 0x141660137;  // E8 D4 4D 9B FF
inline constexpr std::array<std::uint8_t, 16> kLevelListRequestEntry{
    0x4C, 0x8B, 0xDC, 0x55, 0x53, 0x49, 0x8D, 0x6B, 0xA1, 0x48, 0x81, 0xEC, 0xB8, 0x00, 0x00, 0x00};
inline constexpr platform::FunctionFact kLevelListRequest{L"EnvSet_RequestLevelList", 0x1416DF640, kLevelListRequestEntry};
inline constexpr std::array<std::uint8_t, 16> kPlannerSetSlotEntry{
    0x40, 0x53, 0x57, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x63, 0xC2, 0x49, 0x8B, 0xD8, 0x4C};
inline constexpr platform::FunctionFact kPlannerSetSlot{L"PlannerLevelSetSlots_SetSlot_LoadRelease", 0x141014F10,
                                                        kPlannerSetSlotEntry};
inline constexpr std::uint32_t kPlannerSlotIds   = 0x14;  // holder: FName id per slot, 8 bytes each (37 slots)
inline constexpr std::int32_t  kPlannerAreaSlot  = 32;    // the EnvironmentSet's slot (the leader's area)
inline constexpr std::int32_t  kPlannerSlots     = 37;    // story 0-31, area 32, field entry 33-36
/// EndLevelReq_Enqueue(int prio, FName level (by value), bool load, bool hide, bool flag): the global level request
/// queue every load and unload goes through (the loader's first reference enqueues a load, its last an unload).
inline constexpr std::array<std::uint8_t, 16> kLevelRequestEnqueueEntry{
    0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x44};
inline constexpr std::array<std::uint8_t, 5> kLevelRequestEnqueueFirst{0x48, 0x89, 0x54, 0x24, 0x10};  // mov [rsp+10h], rdx
inline constexpr platform::FunctionFact kLevelRequestEnqueue{L"EndLevelReq_Enqueue", 0x1416D9700, kLevelRequestEnqueueEntry};
/// Stock 4.18 (Engine.hpp): UWorld::StreamingLevels and ULevelStreaming's fields. The flags word: 0x20 should be loaded,
/// 0x40 should be visible (the game's load action ORs 0x60 at 0x1416DC10C, its unload tests 0x20 at 0x1416DB5AE).
inline constexpr std::uint32_t kWorldStreamingLevels    = 0x88;   // TArray<ULevelStreaming*>
inline constexpr std::uint32_t kLevelStreamingPackage   = 0x28;   // FName PackageName (empty in this build)
inline constexpr std::uint32_t kLevelStreamingPackageToLoad = 0x58;  // FName PackageNameToLoad
/// TSoftObjectPtr<UWorld> WorldAsset at 0x30 (0x28 bytes): a weak pointer and a tag (0x10), then its FSoftObjectPath,
/// whose FName AssetPathName comes first [ASSUMPTION: stock 4.18 TPersistentObjectPtr layout; the dump's size agrees].
inline constexpr std::uint32_t kLevelStreamingWorldAssetPath = 0x40;
inline constexpr std::uint32_t kLevelStreamingFlags     = 0xB0;   // u32 bit flags
inline constexpr std::uint32_t kLevelStreamingShouldLoad = 0x20;
inline constexpr std::uint32_t kLevelStreamingShouldShow = 0x40;
inline constexpr std::uint32_t kLevelStreamingLoaded    = 0x138;  // ULevel* LoadedLevel
/// How the game's apply finds a listed actor: for each level of the set's list that is in the manager's lookup set,
/// CharaRegistry_FindActorByName(level, name) (the per-level name map of Square Enix's character registry).
inline constexpr std::array<std::uint8_t, 16> kTSetNameFindEntry{
    0x48, 0x89, 0x74, 0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10, 0x8B, 0x41, 0x08, 0x48, 0x8B, 0xFA};
/// TSet<FName>::FindId(set, i32* out, FName): *out = the element's index or -1; returns out.
inline constexpr platform::FunctionFact kTSetNameFind{L"TSet_FName_FindIndex", 0x1406C7110, kTSetNameFindEntry};
inline constexpr std::array<std::uint8_t, 16> kFindActorByNameEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA, 0x48, 0x85, 0xC9, 0x0F, 0x84, 0xAB, 0x00};
/// CharaRegistry_FindActorByName(FName level, FName name) -> actor or null.
inline constexpr platform::FunctionFact kFindActorByName{L"CharaRegistry_FindActorByName", 0x1416C7CE0,
                                                         kFindActorByNameEntry};

// --- A player's hidden actors (stock UE 4.18; research/RE-picture-variants.md §10.2) ---------------------------------
// ULocalPlayer::CalcSceneView calls APlayerController::BuildHiddenComponentList (0x143018F8F, when bRenderPrimitive-
// Components) for that player's view only: the primitive components of each listed actor and their direct attach
// children leave that view. The engine compacts null entries; Square Enix does not override the update virtuals.
inline constexpr std::uint32_t kControllerHiddenActors    = 0x440;  // TArray<AActor*> HiddenActors (num +0x448, max +0x44C)
inline constexpr std::uint32_t kControllerRenderPrimitives = 0x460; // u8 bRenderPrimitiveComponents (1 from the ctor)
inline constexpr std::uint32_t kActorHiddenFlags          = 0x84;   // u8: bHidden is bit 0x02 (SetActorHiddenInGame)
inline constexpr std::uint8_t  kActorHiddenBit            = 0x02;
/// TArray<8-byte element, FDefaultAllocator>::ResizeTo(array, new max): GMalloc's quantize, then its realloc.
inline constexpr std::array<std::uint8_t, 16> kPointerArrayResizeEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x63, 0xDA, 0x48, 0x8B, 0xF9};
inline constexpr platform::FunctionFact kPointerArrayResize{L"TArray_Ptr_ResizeTo", 0x14067C430, kPointerArrayResizeEntry};
/// AActor::GetAttachedActors(actor, TArray<AActor*>* out): every actor attached below this one (the out array is
/// emptied first and grown with GMalloc; the caller frees it).
inline constexpr std::array<std::uint8_t, 16> kGetAttachedActorsEntry{
    0x40, 0x55, 0x53, 0x41, 0x54, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0x28, 0xFF, 0xFF, 0xFF, 0x48};
inline constexpr platform::FunctionFact kGetAttachedActors{L"AActor::GetAttachedActors", 0x142C8EB50,
                                                           kGetAttachedActorsEntry};
/// The save data is the data singleton (kDataSingletonGlobal; SaveDataMgr_Get_cand's singleton [ASSUMPTION: the save data]).
/// While its state is 7, the active data at +0x880 holds the EnvironmentSet's last set name, char[128], which a hit
/// writes and a no-hit reads.
inline constexpr std::uint32_t  kSaveDataActive       = 0x880;
inline constexpr std::uint32_t  kSaveDataState        = 0x890;
inline constexpr std::int32_t   kSaveDataStateActive  = 7;
inline constexpr std::uint32_t  kSaveDataEnvSetName   = 0x480AC;  // from the active data
inline constexpr std::uint32_t  kSaveDataEnvSetNameBytes = 0x80;

// --- The game window, the resolution and the Borderless letterbox (research/RE-multi-monitor-views.md, reviewed by
// hand, §13; RE-view-size-fov.md §7.2) ----------------------------------------------------------------
inline constexpr std::uintptr_t kGEngine                   = 0x145831188;  // UEngine* GEngine
inline constexpr std::uint32_t  kGameEngineViewportWindow  = 0x1078;       // UGameEngine::GameViewportWindow (TWeakPtr<SWindow>: object; referencer +0x1080)
inline constexpr std::uint32_t  kSWindowNativeWindow       = 0x6B8;        // SWindow::NativeWindow (TSharedPtr<FGenericWindow>: object)
inline constexpr std::uint32_t  kEngineGameViewport        = 0x980;        // UEngine::GameViewport (UGameViewportClient*)
inline constexpr std::uint32_t  kViewportClientViewport    = 0xA0;         // UGameViewportClient::Viewport (FViewport*)
inline constexpr std::uint32_t  kViewportSizeX             = 0xB8;         // FViewport::SizeX; SizeY +0xBC
/// FWindowsWindow's vtable (30 slots; stock 4.18 order, confirmed slot by slot). Written only by FWindowsWindow_Make.
inline constexpr std::uintptr_t kWindowsWindowVTable            = 0x144D26828;
inline constexpr std::size_t    kWindowsWindowSlotReshapeWindow = 1;
inline constexpr std::size_t    kWindowsWindowSlotSetWindowMode = 12;
inline constexpr std::size_t    kWindowsWindowSlotGetWindowMode = 13;
inline constexpr std::size_t    kWindowsWindowSlotGetOSWindowHandle = 24;
/// FWindowsWindow::ReshapeWindow(X, Y, W, H): GetWindowInfo, the OS border added when the definition has one,
/// SetWindowPos; no clamp to a monitor (bytes 15-16 start a rel32, so 14 are checked).
inline constexpr std::array<std::uint8_t, 14> kWindowsWindowReshapeWindowEntry{
    0x4C, 0x8B, 0xDC, 0x53, 0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05};
inline constexpr platform::FunctionFact kWindowsWindowReshapeWindow{L"FWindowsWindow::ReshapeWindow", 0x141D2DD90,
                                                                    kWindowsWindowReshapeWindowEntry};
/// FWindowsWindow::GetWindowMode: `mov eax, [rcx+38h]; ret` (a shared body).
inline constexpr std::array<std::uint8_t, 4> kWindowsWindowGetWindowModeEntry{0x8B, 0x41, 0x38, 0xC3};
inline constexpr platform::FunctionFact      kWindowsWindowGetWindowMode{L"FWindowsWindow::GetWindowMode", 0x14088C540,
                                                                         kWindowsWindowGetWindowModeEntry};
/// FWindowsWindow::GetOSWindowHandle: `mov rax, [rcx+28h]; ret` (COMDAT-folded with a CRT accessor).
inline constexpr std::array<std::uint8_t, 5> kWindowsWindowGetOSWindowHandleEntry{0x48, 0x8B, 0x41, 0x28, 0xC3};
inline constexpr platform::FunctionFact      kWindowsWindowGetOSWindowHandle{L"FWindowsWindow::GetOSWindowHandle", 0x141C148A0,
                                                                             kWindowsWindowGetOSWindowHandleEntry};
/// UKismetSystemLibrary::execSetWindowTitle: the witness of the chain (reads +0x1078/+0x1080 and calls SWindow::SetTitle).
inline constexpr std::array<std::uint8_t, 16> kSetWindowTitleExecEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
inline constexpr platform::FunctionFact kSetWindowTitleExec{L"UKismetSystemLibrary::execSetWindowTitle", 0x1435C3AF0,
                                                            kSetWindowTitleExecEntry};
/// FSystemResolution::RequestResolutionChange(W, H, EWindowMode): formats "WxH{f|wf|w}" into r.SetRes at console
/// priority; the engine applies it on its next frame (UpdateSystemResolution -> ResizeFrame). Writes nothing to disk.
inline constexpr std::array<std::uint8_t, 16> kRequestResolutionChangeEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x4C};
inline constexpr platform::FunctionFact kRequestResolutionChange{L"FSystemResolution::RequestResolutionChange", 0x143326EB0,
                                                                 kRequestResolutionChangeEntry};
/// GSystemResolution: ResX, ResY +4, WindowMode +8 (0 Fullscreen, 1 WindowedFullscreen = the game's Borderless and
/// Fullscreen options, 2 Windowed). One writer: FSystemResolution::UpdateSystemResolution.
inline constexpr std::uintptr_t kSystemResolution           = 0x1453E3AE0;
/// Dynamic resolution (research/RE-split-upscale.md, S-RMK-UPSCALE): each global
/// holds a pointer, set at the console variable's registration, to its shadowed values (the game thread's at +0, the
/// render thread's at +4); FDefaultDynResDriver takes fminf(Max / 100, the heuristic) as every view's fraction.
inline constexpr std::uintptr_t kDynResMaxScreenPercentageRef = 0x145952648;  // r.DynamicRes.MaxScreenPercentage (float)
inline constexpr std::uintptr_t kDynResPausingDeniedRef       = 0x145958B30;  // r.DynamicRes.PausingDenied (int)
/// UEngine::GetDynamicResolutionStatus(engine, out): out {status (0 unsupported, 1 disabled, 2 paused, 3 enabled),
/// the fraction, its upper bound}; reads GEngine itself.
inline constexpr std::array<std::uint8_t, 16> kGetDynamicResolutionStatusEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48};
inline constexpr platform::FunctionFact kGetDynamicResolutionStatus{L"UEngine::GetDynamicResolutionStatus", 0x14331B080,
                                                                    kGetDynamicResolutionStatusEntry};
inline constexpr std::int32_t kDynamicResolutionEnabled = 3;
/// The bloom area (FViewInfo +0xA0, research/RE-split-bloom-streaks.md, S-RMK-BLOOMSTREAK): FSceneRenderer::
/// PrepareViewRectsForRendering (0x1425C70B0) sizes it Ceil(Unscaled size x r.ScreenPercentage), without the dynamic-
/// resolution fraction that ViewRect's size has (xmm7 = SP x f, xmm8 = SP). Bloom, the glare passes and the narrow
/// filter draw over it into targets sized from the scene buffer, so in split, with the views in window pixels wider than
/// the buffer, everything past the buffer's width repeats their last column (light streaks). 0x1425C727F through
/// 0x1425C72EB as the research read them: xmm7 = SP x f, then the width's and the height's ViewRect and bloom-area
/// products.
inline constexpr std::uintptr_t kBloomAreaSizeStart = 0x1425C727F;
inline constexpr std::uint8_t   kBloomAreaSize[]    = {
    0x41, 0x0F, 0x28, 0xF8,                          // movaps xmm7, xmm8                 SP
    0x48, 0x8B, 0x4F, 0x40,                          // mov rcx, [rdi+40h]
    0xF3, 0x42, 0x0F, 0x59, 0x3C, 0x28,              // mulss xmm7, [rax+r13]             x f (this view's)
    0x8B, 0x84, 0x2B, 0x88, 0x00, 0x00, 0x00,        // mov eax, [rbx+rbp+88h]            Unscaled.Max.X
    0x2B, 0x84, 0x2B, 0x80, 0x00, 0x00, 0x00,        // sub eax, [rbx+rbp+80h]            - Unscaled.Min.X
    0x66, 0x0F, 0x6E, 0xD8,                          // movd xmm3, eax
    0x0F, 0x5B, 0xDB,                                // cvtdq2ps xmm3, xmm3
    0x0F, 0x28, 0xCB,                                // movaps xmm1, xmm3
    0xF3, 0x0F, 0x59, 0xDF,                          // mulss xmm3, xmm7                  ViewRect width
    0xF3, 0x41, 0x0F, 0x59, 0xC8,                    // mulss xmm1, xmm8                  bloom area width
    0xF3, 0x0F, 0x58, 0xDB,                          // addss xmm3, xmm3
    0xF3, 0x0F, 0x58, 0xC9,                          // addss xmm1, xmm1
    0xF3, 0x0F, 0x5C, 0xC1,                          // subss xmm0, xmm1
    0xF3, 0x0F, 0x2D, 0xC0,                          // cvtss2si eax, xmm0
    0x0F, 0x28, 0xC6,                                // movaps xmm0, xmm6
    0xD1, 0xF8,                                      // sar eax, 1
    0x89, 0x84, 0x24, 0xA8, 0x00, 0x00, 0x00,        // mov [rsp+0A8h], eax
    0x8B, 0x84, 0x2B, 0x8C, 0x00, 0x00, 0x00,        // mov eax, [rbx+rbp+8Ch]            Unscaled.Max.Y
    0x2B, 0x84, 0x2B, 0x84, 0x00, 0x00, 0x00,        // sub eax, [rbx+rbp+84h]            - Unscaled.Min.Y
    0x66, 0x0F, 0x6E, 0xD0,                          // movd xmm2, eax
    0x0F, 0x5B, 0xD2,                                // cvtdq2ps xmm2, xmm2
    0x0F, 0x28, 0xCA,                                // movaps xmm1, xmm2
    0xF3, 0x0F, 0x59, 0xD7,                          // mulss xmm2, xmm7                  ViewRect height
    0xF3, 0x41, 0x0F, 0x59, 0xC8,                    // mulss xmm1, xmm8                  bloom area height
};
inline constexpr std::uintptr_t kBloomAreaWidthMul  = 0x1425C72A9;  // in the block at 0x1425C72A0
inline constexpr std::uintptr_t kBloomAreaHeightMul = 0x1425C72E6;  // in the block at 0x1425C72E0
inline constexpr std::uint8_t   kBloomAreaBySp[]    = {0xF3, 0x41, 0x0F, 0x59, 0xC8};  // mulss xmm1, xmm8
/// mulss xmm1, xmm7 as ONE instruction of the original's length (an empty REX prefix): a 4-byte form plus a nop would
/// add an instruction start the original lacks, and a thread stopped on it when the original is put back would resume
/// inside `mulss xmm1, xmm8` (review).
inline constexpr std::uint8_t   kBloomAreaByViewScale[] = {0xF3, 0x40, 0x0F, 0x59, 0xCF};
inline constexpr std::uint32_t  kSystemResolutionWindowMode = 0x8;
/// The 321 copies of `cmp dword [GSystemResolution.WindowMode], 1` (83 3D rel32 01): the final tonemap pass's own
/// and one in each of its 320 pixel-shader setter permutations, each followed by a jnz over the single-view fit.
inline constexpr std::uintptr_t kLetterboxFirstCompare = 0x14228BF96;
inline constexpr std::uintptr_t kLetterboxLastCompare  = 0x14251767C;
inline constexpr std::size_t    kLetterboxCompares     = 321;
/// ULocalPlayer::CalcSceneView, upstream of the final pass: `call [rax+0F0h]` (FViewport::GetWindowMode, the byte at
/// FViewport+0xC4), `cmp eax, 1`, `jnz`. In window mode 1 the view init options' rect and constrained rect both
/// become (0, 0, GSystemResolution.ResX, ResY) and the player's own rect is kept aside at +0xF0: Square Enix's one
/// view at the option resolution, upscaled by the final pass. With two local players every view then covers the
/// whole target and the last drawn wins (seen live on the spanned window: one view over both monitors
/// with the 321 compares already patched). The patch rewrites this cmp's imm8 with the others.
inline constexpr std::array<std::uint8_t, 11> kViewRectOverrideWitness{0xFF, 0x90, 0xF0, 0x00, 0x00, 0x00,  // call [rax+0F0h]
                                                                        0x83, 0xF8, 0x01,                    // cmp eax, 1
                                                                        0x75, 0x38};                         // jnz +0x38
inline constexpr std::uintptr_t kViewRectOverrideSite      = 0x143018FAF;
inline constexpr std::size_t    kViewRectOverrideImmediate = 8;  // the cmp's imm8 within the witness
/// UGameMapsSettings (reflection dump EngineSettings.hpp): TwoPlayerSplitscreenLayout, 0 Horizontal (top and
/// bottom), 1 Vertical (side by side); read by UGameViewportClient::UpdateActiveSplitscreenType every layout pass.
inline constexpr std::uint32_t kGameMapsTwoPlayerLayout = 0x69;
/// ThreePlayerSplitscreenLayout: 0 FavorTop, 1 FavorBottom (4.18 has no three-column value; RE-multi-monitor-views §7).
inline constexpr std::uint32_t kGameMapsThreePlayerLayout = 0x6A;
/// UGameViewportClient::SplitscreenInfo (research/RE-layout-fov.md §2.1, RE-three-views.md §1): TArray<FSplitscreenData>
/// of the six 4.18 split types (0 None, 1 TwoPlayer_Horizontal, 2 TwoPlayer_Vertical, 3 ThreePlayer_FavorTop,
/// 4 ThreePlayer_FavorBottom, 5 FourPlayer), filled by the constructor (0x142F61900) and read by LayoutPlayers
/// (0x142F68540) on every redraw; each entry a 16-byte TArray<FPerPlayerSplitscreenData>, each record four floats
/// {SizeX, SizeY, OriginX, OriginY}. A TArray's Num is the int32 after its data pointer.
inline constexpr std::uint32_t kViewportClientSplitscreenInfo = 0x60;
/// UGameViewportClient::bDisableSplitScreenOverride (research/RE-layout-fov.md §2.2 [CONFIRMED static]): while set, the
/// split type is None whatever the player count (player 1 fills the screen, the others get a zero-size view). Not
/// written by the constructor; no reflected setter in 4.18 (players/split_switch.h writes the byte).
inline constexpr std::uint32_t kViewportClientDisableSplitOverride = 0x600;
inline constexpr std::uint32_t kTArrayNum                     = 0x08;
inline constexpr std::int32_t  kSplitscreenTypes              = 6;
inline constexpr std::uint32_t kSplitscreenDataBytes          = 0x10;
inline constexpr std::uint32_t kPerPlayerSplitBytes           = 0x10;
inline constexpr std::uint32_t kSplitTypeTwoPlayerHorizontal  = 1;  // top and bottom (RE-layout-fov.md §9: two records)
inline constexpr std::uint32_t kSplitTypeTwoPlayerVertical    = 2;  // side by side
inline constexpr std::uint32_t kSplitTypeThreePlayerFavorTop  = 3;

// --- The camera manager's pose slots and the scene signals (research/RE-camera.md §2.2-2.4, RE-lifecycle-world.md) --
/// AEndPlayerCameraManager keeps 25 pose slots (48 bytes each) inline at +0x1E48 (its own SetCameraSlot 0x141687EB0
/// reads them there); each frame the lowest active slot wins (24 = the field camera, always on). The field camera
/// writes 24, a cutscene starts in 6 and ends through 3/5/7 reading 8/14/18, events use 16-18, minigames 20.
inline constexpr std::uint32_t kCameraManagerSlots        = 0x1E48;
inline constexpr std::uint32_t kCameraSlotBytes           = 48;
inline constexpr std::uint32_t kCameraSlotCount           = 25;
/// The pose slots a scene writes on the primary manager (research/RE-camera.md §2.2-2.4 table; RE-cutscene-camera-two.md
/// §2): 8 the cutscene, 14 the talk camera (a look-at, a shop's framing), 18 the event camera (the event camera system
/// writes it by priority: a scripted talk's angles, an event; a field area's rail cameras write it too, §2.3), 24 the
/// field camera.
inline constexpr std::uint32_t kCameraSlotCutscene        = 8;
inline constexpr std::uint32_t kCameraSlotTalk            = 14;
inline constexpr std::uint32_t kCameraSlotEvent           = 18;
inline constexpr std::uint32_t kCameraSlotField           = 24;
inline constexpr std::uint32_t kCameraSlotFov             = 0x24;  // float
inline constexpr std::uint32_t kCameraSlotActive          = 0x2C;  // byte
inline constexpr std::uint32_t kCameraSlotBlendRecords    = 0x2308;  // 25 blend records, 60 bytes each (kind, ms, rates, interps)
inline constexpr std::uint32_t kCameraSlotBlendBytes      = 60;
inline constexpr std::uint32_t kCameraManagerCurrentSlot  = 0x29E8;  // byte: the slot shown (UpdateCamera 0x1416871B4)
/// AEndPlayerCameraManager::SetCameraSlot(this, u8 slot, const FEndCameraSlot*): the 48-byte copy into the inline table.
inline constexpr std::array<std::uint8_t, 16> kCameraManagerSetSlotEntry{
    0x80, 0xFA, 0x19, 0x73, 0x44, 0x41, 0x0F, 0x10, 0x00, 0x48, 0x8D, 0x81, 0x48, 0x1E, 0x00, 0x00};
inline constexpr platform::FunctionFact kCameraManagerSetSlot{L"AEndPlayerCameraManager::SetCameraSlot", 0x141687EB0,
                                                              kCameraManagerSetSlotEntry};
/// PrimaryPCM_SetCameraSlotBlend(u8 slot, const record*): the 60-byte record into the primary manager's table (the
/// manager the primary controller names: a further player's inside its primary scope); slots 13/16 also refresh +0x2A2D.
inline constexpr std::array<std::uint8_t, 16> kPrimarySetSlotBlendEntry{
    0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48};
inline constexpr platform::FunctionFact kPrimarySetSlotBlend{L"PrimaryPCM_SetCameraSlotBlend", 0x141696990,
                                                             kPrimarySetSlotBlendEntry};
/// AEndPlayerCameraManager::ResetSlotsToField24(this): every active byte cleared, slot 24 on, the view target its own
/// main camera. The cutscene's end (EV/MV) calls it on the primary controller's manager at 0x140ADFBE9 (E8 rel32).
inline constexpr std::array<std::uint8_t, 16> kCameraManagerResetSlotsEntry{
    0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0x81, 0xF8, 0x22, 0x00, 0x00, 0x4C, 0x8D, 0x99, 0x48, 0x1E};
inline constexpr platform::FunctionFact kCameraManagerResetSlots{L"AEndPlayerCameraManager::ResetSlotsToField24", 0x141687E00,
                                                                 kCameraManagerResetSlotsEntry};
inline constexpr std::uintptr_t kCutsceneEndResetCall = 0x140ADFBE9;  // E8 12 82 BA 00 in CutsceneInstance_End_ResetCharaPopOverride
inline constexpr std::uint32_t kCameraManagerBlendRunning = 0x29F9;  // byte (0x1416873BF)
inline constexpr std::uint32_t kCameraManagerPendingCut   = 0x2A38;  // byte (0x141687192)
/// ACameraActor (stock 4.18): CameraComponent +0x380; UCameraComponent: FieldOfView +0x2B0, AspectRatio +0x2C0,
/// bConstrainAspectRatio = bit 0 of +0x2C4 (ACameraActor::Ctor 0x142DC66F3).
inline constexpr std::uint32_t kCameraActorComponent      = 0x380;
inline constexpr std::uint32_t kCameraComponentFov        = 0x2B0;
inline constexpr std::uint32_t kCameraComponentAspect     = 0x2C0;
inline constexpr std::uint32_t kCameraComponentFlags      = 0x2C4;
/// The cutscene manager (one native object, allocated on first use; the global holds its pointer): "a cutscene is
/// playing" = EndCutsceneMgr_IsPlaying_ByNameOrAny(mgr, 0) (the reflected UEndCutAPI::IsPlaying); the skip byte
/// at +0x410 fast-forwards the scene under a black fade.
inline constexpr std::uintptr_t kCutsceneManagerGlobal = 0x1457E9758;
inline constexpr std::uint32_t  kCutsceneSkip          = 0x410;
inline constexpr std::array<std::uint8_t, 16> kCutsceneIsPlayingEntry{
    0x40, 0x55, 0x53, 0x48, 0x8D, 0x6C, 0x24, 0xB1, 0x48, 0x81, 0xEC, 0x98, 0x00, 0x00, 0x00, 0x48};
inline constexpr platform::FunctionFact kCutsceneIsPlaying{L"EndCutsceneMgr_IsPlaying_ByNameOrAny", 0x140A99610,
                                                           kCutsceneIsPlayingEntry};
/// The state manager (ROOT+0x138) holds the field control lock mask (the game's own IsEnableFieldControler reads it
/// with the counter beside it; that counter is never raised in this build, only zeroed and counted down:
/// research/RE-script-release.md §10) and the on/off table (one dword counter per id: 12 ShowSkipPauseMenu = a skippable scene,
/// 39/40/53 a held black screen, 41 the loading screen) followed by the owner of each id (an FName: the last non-zero
/// reason a SetOn passed, zeroed when the count drops to 0; research/RE-minigame-signal.md §1). kRootStateManager
/// (0x138) is above.
inline constexpr std::uint32_t kFieldControlLockMask    = 0x11062B0;  // dword
inline constexpr std::uint32_t kFieldControlLockCounter = 0x1109C88;  // dword
inline constexpr std::uint32_t kOnOffTable              = 0x11063B4;  // dword per id, 92 ids
inline constexpr std::uint32_t kOnOffCount              = 92;
inline constexpr std::uint32_t kOnOffOwners             = 0x1106524;  // FName per id: who turned it on
static_assert(kOnOffOwners == kOnOffTable + 4 * kOnOffCount, "the owners follow the counts");
/// The bike chases (chapters 4 and 18) are a mode of their own, neither a cutscene nor an act with on/off 36
/// (research/RE-minigame-signal.md §4 [CONFIRMED code]): the bike act fills a shared state and the game's own test (its
/// pause-kind chooser) is "the shared state is set and its reference block's count at +8 is above 0".
inline constexpr std::uintptr_t kBikeGameShared         = 0x14585A958;  // void*: the shared bike state, null outside a chase
inline constexpr std::uintptr_t kBikeGameSharedRefCtrl  = 0x14585A960;  // void*: its reference block; int32 +8 the count
inline constexpr std::uint32_t  kBikeGameSharedRefCount = 0x8;

// Walk-up traversal for further players: ladders, squeezes, crawls, monkey bars, climbs and jumps
// (research/RE-traversal-walkup-two.md, S-RMK-TRAVERSE3B; RE-traversal-action-two.md, S-RMK-TRAVERSE3A; both read in
// IDA). Every walk-up spot is a field-action actor: the primary controller's
// PlayerTrigger enter stores the party leader as the actor's one user, a per-frame TryStart requests the action on that
// user's own field-action component, and the component runs it.

/// The trigger sweep (PC vtable slot 417, called only for *(PM+0) by the game mode's late tick): issues the
/// controller's trigger component (PC+0x6E8), which appends its owner's Tags[0] to the pending names.
inline constexpr std::uintptr_t kPlayerTriggerSweepSlot = 0x144C17998;
inline constexpr std::array<std::uint8_t, 16> kPlayerTriggerSweepEntry{
    0x48, 0x8B, 0xC4, 0x57, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00, 0x48, 0x83, 0xB9, 0xE8, 0x06};
inline constexpr platform::FunctionFact kPlayerTriggerSweep{L"EndPlayerController_UpdatePlayerTriggerSweep", 0x1415F6980,
                                                            kPlayerTriggerSweepEntry};
inline constexpr std::uint32_t  kControllerPlayerTrigger    = 0x6E0;  // AEndPlayerController::PlayerTrigger (the actor)
inline constexpr std::uint32_t  kControllerTriggerComponent = 0x6E8;  // AEndPlayerController: its trigger's overlap component
/// The sweep first asks the controller's leader (PC+0x6F8) through its End actor interface whether to reset its
/// trigger: that read (the interface's vt+0xB0 body, reached by per-class thunks passing the character's reference
/// block, AEndCharacter +0xA00) resolves the weak object the block names, casts it to the interface with the stock
/// UObject::GetInterfaceAddress and calls the result with no test (research/RE-referenced-actor-crash.md):
/// world/referenced_actor.h guards its entry. The first instruction is 3 bytes, so the redirect sits at the second.
inline constexpr std::array<std::uint8_t, 16> kReferencedActorReadEntry{
    0x4C, 0x8B, 0xDC, 0x57, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00, 0x44, 0x8B, 0x41, 0x04, 0x49};
inline constexpr std::array<std::uint8_t, 3> kReferencedActorReadFirst{0x4C, 0x8B, 0xDC};              // mov r11, rsp
inline constexpr std::array<std::uint8_t, 8> kReferencedActorReadSecond{0x57, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00};  // push rdi; sub rsp, 90h
inline constexpr platform::FunctionFact kReferencedActorRead{L"EndActorInterface_RefActorStateByte_vtB0", 0x1416276E0,
                                                              kReferencedActorReadEntry};
inline constexpr std::array<std::uint8_t, 16> kGetInterfaceAddressEntry{
    0x4C, 0x8B, 0xDC, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x33, 0xF6, 0x48};
inline constexpr platform::FunctionFact kGetInterfaceAddress{L"UObject::GetInterfaceAddress", 0x141EB0AE0,
                                                              kGetInterfaceAddressEntry};
inline constexpr std::uintptr_t kEndActorInterfaceClass = 0x14599AD20;  // UClass*: UEndActorInterface's class, 0 until first asked
/// The game's party setup for a controller (once per controller: the trigger's component bound to it) and its one leader
/// change, every switch's end (players/party_seat.h).
inline constexpr std::array<std::uint8_t, 16> kInitPartyLevelEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
inline constexpr std::array<std::uint8_t, 16> kSwitchLeaderEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48};
inline constexpr platform::FunctionFact kInitPartyLevel{L"EndPlayerController::InitPartyLevel", 0x1415F6E00,
                                                        kInitPartyLevelEntry};
inline constexpr platform::FunctionFact kSwitchLeaderInternal{L"EndPlayerController::SwitchLeaderInternal", 0x1415F7DB0,
                                                              kSwitchLeaderEntry};
inline constexpr std::uint32_t  kControllerNaviVolumes      = 0x750;  // AEndPlayerController: the navi volumes it is in (TArray)
/// The pending collider names (TArray<FName>: data, then the count at +8), emptied by the dispatcher.
inline constexpr std::uintptr_t kPendingOverlapNames = 0x14586A4E0;
inline constexpr std::uintptr_t kFNamePlayerTrigger  = 0x145903968;  // the FName "PlayerTrigger" (8 bytes)
/// EndCapsuleOverlap_DispatchPendingColliders 0x141659FF0: the call sites (each `call rel32` inside one 16-byte block).
inline constexpr std::uintptr_t kDispatchResolveCall     = 0x14165A109;  // PartyManager_Get: [rax] = the trigger's controller
inline constexpr std::uintptr_t kDispatchBeginRouterCall = 0x14165A548;  // EndTrigger_OnColliderBeginOverlap(volume, collider)
inline constexpr std::uintptr_t kDispatchNaviListCall    = 0x14165A644;  // PartyManager_Get: [rax]+0x750 gets the volume
inline constexpr std::uintptr_t kDispatchNaviEnterCall   = 0x14165A6D8;  // player 1's minimap area enter
inline constexpr std::uintptr_t kDispatchEndRouterCall   = 0x14165A8EA;  // EndTrigger_OnColliderEndOverlap(volume, collider)
inline constexpr std::uintptr_t kDispatchNaviRemoveCall  = 0x14165AA06;  // removes the volume from rcx = [PM+0]+0x750
inline constexpr std::uintptr_t kDispatchNaviLeaveCall   = 0x14165AA42;  // player 1's minimap area leave
inline constexpr std::array<std::uint8_t, 16> kTriggerBeginRouterEntry{
    0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x48, 0x8D, 0x6C, 0x24, 0xC9};
inline constexpr platform::FunctionFact kTriggerBeginRouter{L"EndTrigger_OnColliderBeginOverlap", 0x140FCF5E0, kTriggerBeginRouterEntry};
inline constexpr std::array<std::uint8_t, 16> kTriggerEndRouterEntry{
    0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48};
inline constexpr platform::FunctionFact kTriggerEndRouter{L"EndTrigger_OnColliderEndOverlap", 0x140FCFE10, kTriggerEndRouterEntry};
inline constexpr std::array<std::uint8_t, 16> kNaviEnterAreaEntry{
    0x40, 0x53, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x40, 0x4C, 0x8B, 0x02, 0x48, 0x8B, 0xF9, 0x4C};
inline constexpr platform::FunctionFact kNaviEnterArea{L"NaviMapMgr_OnPlayerTriggerEnterArea", 0x1416856D0, kNaviEnterAreaEntry};
inline constexpr std::array<std::uint8_t, 16> kNaviLeaveAreaEntry{
    0x40, 0x55, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0x02, 0x48, 0x8D, 0x69, 0x58, 0x4C};
inline constexpr platform::FunctionFact kNaviLeaveArea{L"NaviMapMgr_OnPlayerTriggerLeaveArea", 0x141685930, kNaviLeaveAreaEntry};
inline constexpr std::array<std::uint8_t, 16> kNaviListRemoveEntry{
    0x48, 0x89, 0x54, 0x24, 0x10, 0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x48, 0x4C, 0x63, 0xA1};
inline constexpr platform::FunctionFact kNaviListRemove{L"NaviVolumeList_Remove", 0x140FA7EA0, kNaviListRemoveEntry};
/// The field-action manager's volume map (FieldActionMgr+0x280, TMap<FName, actor>, 24-byte pairs: key, actor),
/// searched by the game's own FName find (out: the index, -1 if absent).
inline constexpr std::array<std::uint8_t, 16> kFieldActionMgrGetEntry{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x05, 0x1D, 0x97, 0x17, 0x05, 0x48, 0x85, 0xC0, 0x75, 0x5C};
inline constexpr platform::FunctionFact kFieldActionMgrGet{L"FieldActionMgr_Get", 0x140670300, kFieldActionMgrGetEntry};
inline constexpr std::array<std::uint8_t, 16> kNameMapFindEntry{
    0x48, 0x89, 0x74, 0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10, 0x8B, 0x41, 0x08, 0x48, 0x8B, 0xFA};
inline constexpr platform::FunctionFact kNameMapFind{L"TMap_FNameKey24_FindIndex", 0x14067E200, kNameMapFindEntry};
inline constexpr std::uint32_t kFieldActionMgrVolumes = 0x280;
inline constexpr std::uint32_t kNameMapPairSize       = 24;
inline constexpr std::uint32_t kNameMapPairValue      = 8;
/// A field-action spot actor's state (End FieldActionBase, RE-ladder-spot-offer.md §2.2). NPC-only: its level state row
/// said NPCONLY, so its collision is off (no PlayerTrigger reaches it, player 1's included) and only a companion's AI takes
/// it, by its navigation link. Deactivated: collision and link off. [CONFIRMED code]
inline constexpr std::uint32_t kFieldSpotNpcOnly      = 0x58D;  // u8, 1 NPC-only
inline constexpr std::uint32_t kFieldSpotActivation   = 0x4FC;  // u8
inline constexpr std::uint8_t  kFieldSpotDeactivated  = 2;
/// A field-action spot's level signals (AEndFieldActionActorBase, reflected): OnceOnlyReact (u8, a one-shot spot) and six
/// FNames from +0x414: the default start and end, the left side's start and end, the right side's start and end. A pass
/// sends the default start with its side's start, and at its end the side's end then the default end
/// (FieldActionBase_SendStartStateTriggers_LeaderType 0x140EED560, FieldActionBase_SendEndStateTriggers 0x140EEDDB0;
/// research/RE-stuck-spot-repair.md). Only that class and its children have them: other actors in the manager's map
/// hold other fields there. [CONFIRMED dump + code]
inline constexpr std::uint32_t  kFieldSpotOnceOnly     = 0x410;
inline constexpr std::uint32_t  kFieldSpotSignals      = 0x414;
inline constexpr std::uint32_t  kFieldSpotSignalCount  = 6;
inline constexpr const wchar_t* kFieldSpotClass        = L"/Script/EndGame.EndFieldActionActorBase";
/// A door (AEndFieldActionActorDoor, reflected) also signals turning its handle: StartTurnHandleTrigger and
/// EndTurnHandleTrigger (FNames). Like the base's six, they go out for the party leader's open only, so a door that names
/// any of them is one the level listens to (chapter 13's entrance to Aerith's house: DefaultStartStateTrigger
/// trg080_ArtDoorEnt_Night_Start). [CONFIRMED dump + level data]
inline constexpr const wchar_t* kFieldDoorClass        = L"/Script/EndGame.EndFieldActionActorDoor";
inline constexpr const wchar_t* kFieldHeavyDoorClass   = L"/Script/EndGame.EndFieldActionActorHeavyDoor";  // not a door child
inline constexpr std::uint32_t  kFieldDoorTurnSignals  = 0x654;
inline constexpr std::uint32_t  kFieldDoorTurnSignalCount = 2;
/// AActor::bActorEnableCollision (stock 4.18 bitfield byte and bit), what an NPC-only state turns off. [CONFIRMED code]
inline constexpr std::uint32_t kActorCollisionByte    = 0x86;
inline constexpr std::uint8_t  kActorCollisionBit     = 0x04;

/// The walk-up classes the further players take (the vault, the rubble step-over and the duck-under from
/// research/RE-traversal-vault-two.md, the slides, the fence gap and the step from RE-traversal-slides-two.md, the
/// balance beam, the fan, the counter gate and the lifting rope from RE-traversal-spline-two.md; the
/// vault and the slides have no arrows, their `+0x610` is the shared empty function): native vtables (placed Blueprint actors keep them), TryStart
/// (`+0x608`, void(actor, float dt), game thread), the enter and leave the install check expects, the markers update
/// (`+0x610`, void(actor, float dt, const FVector* point): the arrows spawn within 40 m of the point, despawn past 50 m),
/// the user block the enter writes (the user's weak pair and the nearer end's transform; read by the ctor, enter,
/// leave and TryStart only) and the member branch's ExceptionFlg byte (0: none).
struct WalkUpClass
{
    const wchar_t* name;
    std::uintptr_t vtable, tryStart, enter, leave, markers;
    std::uint32_t  blockA, sizeA, blockB, sizeB;  // sizeB 0: one range
    std::uint32_t  exceptionFlag;
};
inline constexpr std::uint32_t kVtTryStart = 0x608, kVtMarkers = 0x610, kVtEnter = 0x688, kVtLeave = 0x690;
inline constexpr WalkUpClass kWalkUpClasses[] = {
    {L"ladder", 0x144BA2098, 0x140F35140, 0x140F367C0, 0x140F36C50, 0x140F35FB0, 0x648, 0x38, 0, 0, 0x625},
    {L"squeeze", 0x144B97D30, 0x140EFF570, 0x140EE5EA0, 0x140EE6190, 0x140EE5700, 0x658, 0x38, 0, 0, 0x654},
    {L"crawl", 0x144BA8FC8, 0x140F05A60, 0x140F067D0, 0x140F06AC0, 0x140F05FF0, 0x660, 0x08, 0x670, 0x30, 0},
    {L"climb", 0x144B995F0, 0x140EF8C40, 0x140EF9540, 0x140EF9830, 0x140EF9190, 0x630, 0x08, 0x640, 0x30, 0},
    {L"jump", 0x144BA30E8, 0x140F30300, 0x140F312E0, 0x140F314F0, 0x140F05FF0, 0x668, 0x38, 0, 0, 0},
    {L"monkey bars", 0x144BA11A8, 0x140F3BE30, 0x140F3CCD0, 0x140F3CFF0, 0x140F3C380, 0x698, 0x38, 0, 0, 0},
    {L"vault", 0x144B9E840, 0x140F42DB0, 0x140F43320, 0x140F435E0, 0x14062CFC0, 0x640, 0x08, 0x650, 0x30, 0},
    {L"rubble step-over", 0x144BB2038, 0x140F4FBE0, 0x140F1DCA0, 0x140F1DF90, 0x140EF9190, 0x638, 0x38, 0, 0, 0},
    {L"duck-under", 0x144BA78E8, 0x140F1D130, 0x140F1DCA0, 0x140F1DF90, 0x140EF9190, 0x638, 0x38, 0, 0, 0},
    {L"slide", 0x144BB0838, 0x140F591D0, 0x140F58560, 0x140F36C50, 0x14062CFC0, 0x648, 0x38, 0, 0, 0},
    {L"slide-down", 0x144BB0F68, 0x140F577C0, 0x140F58560, 0x140F36C50, 0x14062CFC0, 0x648, 0x38, 0, 0, 0},
    {L"fence gap", 0x144BB3F08, 0x140F46C80, 0x140EF9540, 0x140EF9830, 0x140EF9190, 0x630, 0x08, 0x640, 0x30, 0},
    {L"step", 0x144BAD280, 0x140F62320, 0x140EF9540, 0x140EF9830, 0x140EF9190, 0x630, 0x08, 0x640, 0x30, 0},
    {L"balance beam", 0x144B9C850, 0x140EE51B0, 0x140EE5EA0, 0x140EE6190, 0x140EE5700, 0x658, 0x38, 0, 0, 0},
    {L"fan", 0x144BA4EB0, 0x140F1FD50, 0x140F204A0, 0x140F20790, 0x140EE5700, 0x650, 0x08, 0x660, 0x30, 0},
    {L"counter gate", 0x144B9D110, 0x140EE9040, 0x140EE5EA0, 0x140EE6190, 0x140EE9710, 0x658, 0x38, 0, 0, 0},
    {L"lifting rope", 0x144BA1998, 0x140F39190, 0x140F3A160, 0x140F1DF90, 0x140EF9190, 0x638, 0x38, 0, 0, 0},
    // research/RE-traversal-deferred-two.md §7: the game's AI companions take these three too (its nav-link route).
    {L"gap jump", 0x144B9F7F0, 0x140F40E10, 0x140F41380, 0x140F36C50, 0x14062CFC0, 0x648, 0x38, 0, 0, 0},
    {L"pole slide", 0x144BB3018, 0x140F4A3C0, 0x140F4AE50, 0x140F1DF90, 0x140F4AE10, 0x638, 0x38, 0, 0, 0},
    {L"wire fence", 0x144BAC390, 0x140F67C90, 0x140F68410, 0x140F68770, 0x140F681E0, 0x620, 0x08, 0x630, 0x30, 0},
};
inline constexpr std::size_t   kWalkUpCrawl          = 2;      // its TryStart counts a cooldown down: kept for the re-run
inline constexpr std::size_t   kWalkUpPole           = 18;     // admitted only with its finish's fix (world/traversal_action.h)
inline constexpr std::uint32_t kCrawlCooldown        = 0x588;  // float
inline constexpr std::uint32_t kFieldActionIgnore    = 0x520;  // the actor's ignore group: TArray<FName> (data, count +8)
inline constexpr std::uint32_t kFieldActionIgnoreRow = 0x3D4;  // FName: the ignore group's ActorGroup row (IgnoreActorGroup)
inline constexpr std::uint32_t kRootStickLatch       = 0x9E4;  // g_EndGameRoot: player 1's camera-cut stick latch (byte)
inline constexpr std::uint32_t kOnOffDisableFieldAction = 4;  // set by the party leader's action start (actor not forcing its icons), read by the pick and every start

/// The field-action tick (research/RE-traversal-action-two.md §2): the FieldActionSystem's tick function at +0x10
/// ticks every character's field-action component; it is registered to run on any thread (flags +0x1C bit 0x20,
/// FTickFunction::bRunOnAnyThread, read each frame by the engine's queue), so in practice a worker runs every action.
inline constexpr std::uintptr_t kFieldActionSystemGlobal = 0x1457E99C0;
inline constexpr std::uint32_t  kFieldActionTickFlags    = 0x1C;
inline constexpr std::uint8_t   kTickRunOnAnyThread      = 0x20;
/// EndField_TickAllFieldActionComps_AnyThreadTick: `call FieldActionComp_Tick(comp = [char+0xE58], float dt)`.
inline constexpr std::uintptr_t kFieldActionCompTickCall = 0x140FBE528;
inline constexpr std::array<std::uint8_t, 16> kFieldActionCompTickEntry{
    0x40, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xC1, 0x48, 0x81, 0xEC, 0x98, 0x00};
inline constexpr platform::FunctionFact kFieldActionCompTick{L"FieldActionComp_Tick", 0x140FEE320, kFieldActionCompTickEntry};
inline constexpr std::uint32_t kCharacterFieldAction = 0xE58;  // EndCharacter: its field-action component
inline constexpr std::uint32_t kFieldActionState     = 0x159;  // the component's running action (EFieldActionActorType; 0 none)
inline constexpr std::uint32_t kFieldActionPending   = 0x1F0;  // a requested action waits for the next component tick (byte)
inline constexpr std::uint32_t kFieldActionTookLocks = 0x1FC;  // the action start took its global switches (byte)
inline constexpr std::uint32_t kFieldActionActorWeak = 0x8;  // the running action's actor (weak pair; StartByName sets it)
/// AEndFieldActionActorBase::ForceActiveTargetIcon (reflected, byte): set, the leader's action start leaves "no new
/// action" (OnOff 4) off (FieldActionComp_OnActionStartEnd 0x140FEFB70, research/RE-hookshot-press.md §2.1).
inline constexpr std::uint32_t kFieldActorForceTargetIcon = 0x3D0;
inline constexpr std::uint32_t kControllerCharacter = 0x390;  // AController::Character (reflected): the possessed character
inline constexpr std::uint32_t kSceneRelativeRotation = 0x188;  // USceneComponent::RelativeRotation (reflected): pitch, yaw, roll
inline constexpr std::uint8_t  kFieldActionLadder = 1, kFieldActionCrawl = 3, kFieldActionSqueeze = 4, kFieldActionMonkeyBars = 10;
inline constexpr std::uint8_t  kFieldActionVault = 18, kFieldActionRubble = 28, kFieldActionDuck = 32;
inline constexpr std::uint8_t  kFieldActionStep = 9, kFieldActionSlide = 16, kFieldActionSlideDown = 17, kFieldActionFenceGap = 27;
inline constexpr std::uint8_t  kFieldActionJump = 8, kFieldActionClimb = 21;
inline constexpr std::uint8_t  kFieldActionBalance = 6, kFieldActionFan = 20, kFieldActionCounterGate = 31, kFieldActionRope = 40;
inline constexpr std::uint8_t  kFieldActionGapJump = 19, kFieldActionWireFence = 38, kFieldActionPole = 39;
inline constexpr std::uint8_t  kFieldActionHookShot = 15, kFieldActionCurtain = 26;
/// The grappling hook's state (15, research/RE-hookshot-two.md §2.5): for the party leader's type its Start queues the
/// field lock bit 1 (HookShotDisableController: hands the party leader to its AI and holds every controller for the
/// turn), its sub-state 1 enter clears it after the turn and its Exit clears it again at every end (the only clear on an
/// abort; the clears are plain bit clears). Three calls of the queued lock (void(int8 bit, u64 reason)), each inside one
/// 16-byte block. Its camera: flavor 16 (on at sub-state 1 for the party leader, off at the landing by the action's own
/// delegate).
inline constexpr std::uintptr_t kHookShotLockCall = 0x141047786;        // E8 95 52 F8 FF, to kQueueLockBitSet
inline constexpr std::uintptr_t kHookShotUnlockCall = 0x141048614;      // E8 D7 44 F8 FF, to kQueueLockBitClear
inline constexpr std::uintptr_t kHookShotExitUnlockCall = 0x141047FC6;  // E8 25 4B F8 FF, to kQueueLockBitClear
inline constexpr std::uint8_t   kFlavorHookShot = 16;
/// The hook area's search arrow (research/RE-hookshot-arrows.md). EndUISystem[55] is one 128-byte Square Enix object (no
/// vtable): a search compass widget (weak, +0x00), two more weak pairs, a state (+0x28: 0 run, 1 fade all out, 2 clear
/// when faded, 3 clear) and a map of arrows (TMap<weak widget, {FVector, u8 removeState}> at +0x30). A hook area's enter and
/// its target-lost re-show call Begin on it (ClearAll, a new compass, then their arrow); its leave, focus clear and EndPlay
/// set state 1 (every arrow fades). The HUD tick ticks it once a frame (AnimStates, then PlaceArrow for each arrow through
/// the arrow's own player, +0x1E8). The area actor (AEndFieldActionActorHookShotChild) keeps one in-area flag (+0x638)
/// and one arrow (+0x640); its nav-area interface sits at +0x3C8 (enter, stay, leave take the interface).
inline constexpr std::uint32_t  kSearchSlot      = 55;  // EndUISystem[55]
inline constexpr std::uint32_t  kSearchSlotBytes = 0x80;
inline constexpr std::uint32_t  kSearchSlotCompass = 0x00, kSearchSlotState = 0x28;
inline constexpr std::uint32_t  kSearchSlotMapNum = 0x38, kSearchSlotMapBits = 0x40, kSearchSlotMapFree = 0x60, kSearchSlotMapFreeCount = 0x64;
inline constexpr std::uint32_t  kHookAreaIface = 0x3C8;
inline constexpr std::uint32_t  kHookAreaFields = 0x638, kHookAreaFieldsBytes = 16;  // the flag (u8), padding, the arrow (widget*)
inline constexpr std::uint32_t  kHookAreaArrow = 0x640;
/// A field-action actor's in-use value (i32; research/RE-hookshot-in-use.md): 0 free, 1 in use, 2 switched off. Every
/// flight's start sets 1 through vtable +0x660; the one clear (+0x668, from the flight's exit) needs the point's user list
/// empty, and the finishing user is still on it then, so a hook point's 1 stays until the level reloads. Its enter and its
/// target-lost re-show make no arrow unless it is 0: a used point shows no guide arrow again.
inline constexpr std::uint32_t  kFieldActionInUse    = 0x4FC;
inline constexpr std::int32_t   kFieldActionInUseOff = 2;
inline constexpr std::array<std::uint8_t, 16> kFieldActionSetInUseEntry{
    0xC7, 0x81, 0xFC, 0x04, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
inline constexpr std::array<std::uint8_t, 16> kHookAreaEnterEntry{
    0x40, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48, 0x81, 0xEC, 0xD0, 0x00};
inline constexpr std::array<std::uint8_t, 16> kHookAreaStayEntry{
    0x48, 0x83, 0xEC, 0x48, 0x4C, 0x8B, 0x81, 0x78, 0x02, 0x00, 0x00, 0x4D, 0x85, 0xC0, 0x74, 0x43};
inline constexpr std::array<std::uint8_t, 16> kHookAreaLeaveEntry{
    0x40, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x30, 0x45, 0x33, 0xF6, 0x48};
inline constexpr std::array<std::uint8_t, 16> kHookAreaFocusClearEntry{
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x83, 0xB9, 0x40, 0x06, 0x00, 0x00, 0x00, 0x48, 0x8B};
inline constexpr std::array<std::uint8_t, 16> kHookAreaTargetLostEntry{
    0x40, 0x55, 0x53, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00};
inline constexpr std::array<std::uint8_t, 16> kHookAreaEndPlayEntry{
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xF9, 0xE8, 0x32, 0x21, 0xFC, 0xFF, 0x48, 0x83};
inline constexpr platform::SlotFact kHookAreaEnter{L"the hook area's nav-area enter (interface slot 2)", 0x144BA3FE8,
    {L"HookShotChild_NavArea_OnEnter_SearchRowWidgetCamPush", 0x140F2B920, kHookAreaEnterEntry}};
inline constexpr platform::SlotFact kHookAreaStay{L"the hook area's nav-area stay (interface slot 3)", 0x144BA3FF0,
    {L"HookShotChild_NavArea_OnStay_PlaceSearchWidget", 0x140F2BEA0, kHookAreaStayEntry}};
inline constexpr platform::SlotFact kHookAreaLeave{L"the hook area's nav-area leave (interface slot 4)", 0x144BA3FF8,
    {L"HookShotChild_NavArea_OnLeave_ClearRowWidgetCamPop", 0x140F2BF00, kHookAreaLeaveEntry}};
inline constexpr platform::SlotFact kHookAreaFocusClear{L"the hook area's focus clear (vtable +0x6B8)", 0x144BA3F90,
    {L"HookShotChild_vt6B8_OnFocused_ClearSearchWidget", 0x140F2B1E0, kHookAreaFocusClearEntry}};
inline constexpr platform::SlotFact kHookAreaTargetLost{L"the hook area's target-lost re-show (vtable +0x6C0)", 0x144BA3F98,
    {L"HookShotChild_vt6C0_OnTargetLost_ShowSearchIfLeaderIdle", 0x140F2B2D0, kHookAreaTargetLostEntry}};
inline constexpr platform::SlotFact kHookAreaEndPlay{L"the hook area's EndPlay (vtable +0x4D0)", 0x144BA3DA8,
    {L"HookShotChild_vt4D0_EndPlay_ClearSearchWidget", 0x140F2AF10, kHookAreaEndPlayEntry}};
inline constexpr platform::SlotFact kHookAreaSetInUse{L"the hook point's in-use mark at a flight's start (vtable +0x660)", 0x144BA3F38,
    {L"FieldActionBase_vt660_SetState4FC_InUse", 0x140ECE740, kFieldActionSetInUseEntry}};
/// The slot's own functions (each takes the slot object), the bit array's init the constructor calls on the map's
/// allocation bits (void(bits, bool value, int32 count)), and the one ClearAll of player 1's slot in the game mode's reset
/// (crosses a block).
inline constexpr std::array<std::uint8_t, 16> kSearchSlotClearAllEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x4C};
inline constexpr platform::FunctionFact kSearchSlotClearAll{L"EndUISlot55_FieldTargetSearch_ClearAll", 0x14140E0F0,
                                                            kSearchSlotClearAllEntry};
inline constexpr std::array<std::uint8_t, 16> kSearchSlotTickEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0xA9, 0x48, 0x81, 0xEC, 0x90, 0x00};
inline constexpr platform::FunctionFact kSearchSlotTick{L"EndUISlot55_FieldTargetSearch_Tick", 0x14140EED0, kSearchSlotTickEntry};
/// The tick's per-arrow placement, bool(void* unused, UUserWidget* arrow, const FVector* worldLocation): projects the
/// point through the arrow's own player (one FLocalPlayerContext::GetPlayerController call, the site below, which crosses
/// a block) and the overlay's host set of the pass, and writes only the arrow's angle (+0xA8, UpdateRenderTransform) and
/// place (+0x1B0..+0x1C7): no timer, state or map entry (research/RE-hookshot-arrows.md §8). The tick walks the slot's
/// map (a TSparseArray at +0x30 of 32-byte elements: the weak widget pair, then the world location at +0x08).
inline constexpr std::array<std::uint8_t, 16> kSearchPlaceArrowEntry{
    0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48, 0x81, 0xEC};
inline constexpr platform::FunctionFact kSearchPlaceArrow{L"EndUISlot55_FieldTargetSearch_PlaceArrow", 0x14140F0A0,
                                                          kSearchPlaceArrowEntry};
inline constexpr std::uintptr_t kSearchPlaceArrowContextCall = 0x14140F11E;  // E8 CD 7B C0 01, to kPlayerContextController
inline constexpr std::uint32_t  kSearchSlotMap = 0x30, kSearchArrowStride = 0x20, kSearchArrowLocation = 0x08;
inline constexpr std::array<std::uint8_t, 16> kBitArrayInitEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48};
inline constexpr platform::FunctionFact kBitArrayInit{L"TBitArray init (bits, value, count)", 0x14063F170, kBitArrayInitEntry};
inline constexpr std::uintptr_t kSearchSlotResetCall = 0x1413ADF8C;  // E8 5F 01 06 00, to kSearchSlotClearAll
/// The slide-down state's post-tick (state vtable +0x20, void(state, float dt)): its side steer reads the merged stick.
/// The engine calls every character's post-tick at the top of the walk-up start loop (game thread), outside the
/// component tick (research/RE-traversal-slides-two.md §4).
inline constexpr std::uintptr_t kSlideDownPostTickSlot = 0x144BB7178;
inline constexpr std::array<std::uint8_t, 16> kSlideDownPostTickEntry{
    0x40, 0x55, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xB1, 0x48, 0x81, 0xEC, 0xF8, 0x00, 0x00, 0x00, 0x48};
inline constexpr platform::FunctionFact kSlideDownPostTick{L"FieldActionSlideDownState_PostTick_SideSteerLeftX", 0x14106C0B0,
                                                           kSlideDownPostTickEntry};
inline constexpr std::uint32_t kStateComponent = 0x30;  // a field-action state: its component
/// The overlap fade (research/RE-overlap-fade-two.md): eight field-action states' vtable +0x58, called only from their
/// post-tick (+0x20) on the game thread, outside the component tick. While the acting character is not the party
/// leader's type (its component's +0x158 against PM+0x233) and a party member before it in formation order is near
/// (and, but on a ladder, in an action too), it fades the character to 0.3 (the vent crawl's to 0) on the generic hide
/// reason bit 0, and back when none is near; state+0x48 is 1 while this has it unfaded. The action's base exit fades it
/// back at the end. Ladder, crawl, squeeze, beam, fan, jump, trap and vent crawl; every other state's +0x58 is a return.
inline constexpr std::uint32_t kFieldActionCompPlayerType = 0x158;  // the component's player type byte
inline constexpr std::uint32_t kStateNotFaded              = 0x48;   // byte: 1 while the overlap fade has it unfaded
inline constexpr std::array<std::uint8_t, 16> kOverlapFadeLadderEntry{
    0x40, 0x55, 0x53, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xB0, 0x48, 0x81, 0xEC, 0x50, 0x01, 0x00, 0x00};
inline constexpr std::array<std::uint8_t, 16> kOverlapFadeD0Entry{
    0x40, 0x55, 0x53, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00};
inline constexpr std::array<std::uint8_t, 16> kOverlapFadeC0Entry{
    0x40, 0x55, 0x53, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00};
inline constexpr platform::FunctionFact kOverlapFadeLadder{L"FieldActionLadderState_vt58_NonLeaderProximityFade", 0x141051660,
                                                           kOverlapFadeLadderEntry};
inline constexpr platform::FunctionFact kOverlapFadeShared{L"FieldActionState_vt58_NonLeaderProximityFade_CrawlSqueezeBeamFan",
                                                           0x141021EF0, kOverlapFadeD0Entry};
inline constexpr platform::FunctionFact kOverlapFadeJump{L"FieldActionJumpState_vt58_NonLeaderProximityFade", 0x14104E640,
                                                         kOverlapFadeD0Entry};
inline constexpr platform::FunctionFact kOverlapFadeTrap{L"FieldActionTrapState_vt58_NonLeaderProximityFade", 0x14107DBB0,
                                                         kOverlapFadeC0Entry};
inline constexpr platform::FunctionFact kOverlapFadeDuct{L"FieldActionDuctMoveState_vt58_ProximityFadeToZero_LeaderPC860",
                                                         0x14103AFC0, kOverlapFadeC0Entry};
inline constexpr std::uintptr_t kOverlapFadeLadderSlot = 0x144BB76F0, kOverlapFadeCrawlSlot = 0x144BB6860,
                                kOverlapFadeSqueezeSlot = 0x144BB6940, kOverlapFadeBeamSlot = 0x144BB6B70,
                                kOverlapFadeFanSlot = 0x144BB6580, kOverlapFadeJumpSlot = 0x144BB7760,
                                kOverlapFadeTrapSlot = 0x144BB6FF0, kOverlapFadeDuctSlot = 0x144BB6710;
/// The balance beam's end delegate (animation side, void(capture* = {state}, char reached)): it leaves camera flavor 20
/// only if the operator it reads unscoped (player 1's) holds the flavor the start stored at state+0x178, and clears that
/// store either way, so the state's exit (inside the action start, on the seat's operator; it has no leader test: the
/// game's eased leave) finds nothing to leave for a seat's member (research/RE-traversal-spline-two.md). First
/// instruction: mov [rsp+20h], rsi.
inline constexpr std::array<std::uint8_t, 16> kBalanceEndDelegateEntry{
    0x48, 0x89, 0x74, 0x24, 0x20, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0xA9, 0x48, 0x81, 0xEC, 0xB0, 0x00};
inline constexpr platform::FunctionFact kBalanceEndDelegate{L"FieldActionBalanceMoveState_EndDelegate_Flavor20Leave", 0x141024570,
                                                            kBalanceEndDelegateEntry};
inline constexpr std::size_t   kBalanceEndDelegateFirst = 5;
inline constexpr std::uint32_t kBalanceStoredFlavor     = 0x178;
/// Camera flavor 20 (customizable): its current phase entry (+0x468, a pointer into the phase array: data +0x458, count
/// +0x450, 56 bytes each, the first dword the phase's kind). The beam's and the monkey bars' exits leave it by pointing it
/// at the entry of kind 2 (the way out).
inline constexpr std::uint32_t kFlavorCurrentPhase = 0x468;
inline constexpr std::uint32_t kFlavorPhaseLeaving = 2;
/// The monkey bars' Start and Exit and the balance beam's Start (state vtable +0x08 / +0x10, void(state); the callers
/// discard the return). The action start calls them inside it (0x140FF0278 the new state's Start, 0x140FEFECD the old
/// state's Exit), where the mod holds player 1's leader for a seat's member (research/RE-traversal-deferred-two.md §5.4).
/// What in them reads the party leader: camera flavor 20 (the leader type's Start activates it, then the spot's camera by
/// name, and stores it at state+0x178; the monkey bars' Exit eases the stored one out; the beam's Exit has no leader
/// test); the monkey bars' in-use markers (actor vt+0x660, aimed from the leader's position; the beam's is the base
/// in-use count); and the base exit's end signals (FieldActionState_BaseExit -> SendEndStateTriggers, the leader set or
/// the member's by PM+0x233), called first in the monkey bars' Exit at 0x141056D22 (fits its block). The base start
/// reads no leader, and both states' vt+0x60/+0x68 are the shared no-op (no OnOff 25).
inline constexpr std::uintptr_t kMonkeyBarsStartSlot = 0x144BB7550, kMonkeyBarsExitSlot = 0x144BB7558, kBalanceStartSlot = 0x144BB6B20;
inline constexpr std::array<std::uint8_t, 16> kMonkeyBarsStartEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0xE8, 0x1E, 0xF5};
inline constexpr platform::FunctionFact kMonkeyBarsStart{L"FieldActionMonkeyBarsState_Start_Flavor20IfLeader", 0x141056A60,
                                                         kMonkeyBarsStartEntry};
inline constexpr std::array<std::uint8_t, 16> kMonkeyBarsExitEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48};
inline constexpr platform::FunctionFact kMonkeyBarsExit{L"FieldActionMonkeyBarsState_Exit_Flavor20IfLeader", 0x141056D10,
                                                        kMonkeyBarsExitEntry};
inline constexpr std::array<std::uint8_t, 16> kBalanceStartEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55, 0x48, 0x8B, 0xEC, 0x48, 0x83};
inline constexpr platform::FunctionFact kBalanceStart{L"FieldActionBalanceMoveState_Start_Flavor20IfLeaderCustomName", 0x1410211E0,
                                                      kBalanceStartEntry};
inline constexpr std::uintptr_t kMonkeyBarsBaseExitCall = 0x141056D22;  // E8 E9 F7 FC FF
inline constexpr std::uintptr_t kStateBaseExit          = 0x141026510;  // FieldActionState_BaseExit_ASSUMED, void(state)
/// The action's start and end: for the party leader's component the start turns on OnOff 11/4/1 for everyone (the
/// field menu, every walk-up start, encounters) until its end. Its first instruction: mov [rsp+10h], rbx.
inline constexpr std::array<std::uint8_t, 16> kActionStartEndEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
inline constexpr std::size_t kActionStartEndFirst = 5;
inline constexpr platform::FunctionFact kActionStartEnd{L"FieldActionComp_OnActionStartEnd_LeaderOnOffLocks", 0x140FEFB70,
                                                        kActionStartEndEntry};
/// A spot's start signals (spot actor, a pointer to a copy of the character's first tag (an FName), side), sent from the
/// action's state enters inside the component tick, by every walk-up kind: for the party leader's name the spot's default
/// start signals (actor +0x414, and +0x424 or +0x434 by side) go to the state manager, which saves the states they set;
/// for any other name only its own entries. The end signals (0x140EEDDB0) go from the state's Exit. Its first
/// instruction: mov [rsp+18h], rbx (research/RE-camera-slot14-stuck.md, RE-cave-spot-camera-toggle.md).
inline constexpr std::array<std::uint8_t, 16> kSpotStartSignalsEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48};
inline constexpr std::size_t            kSpotStartSignalsFirst = 5;
inline constexpr platform::FunctionFact kSpotStartSignals{L"FieldActionBase_SendStartStateTriggers_LeaderType", 0x140EED560,
                                                          kSpotStartSignalsEntry};
/// The crawl's and the squeeze's field control lock (reason "EndFieldActionCrack"), taken for the party leader's
/// component only: the first lock hands the party leader to its AI and stops player 1's command input. Three of the
/// four calls cross a 16-byte block (placed from the game thread once the field-action tick runs there).
inline constexpr std::uintptr_t kCrawlLockCall      = 0x141034A5F;  // EndStateMgr_FieldControlLock_Set(sm, 1, reason)
inline constexpr std::uintptr_t kCrawlUnlockCall    = 0x141032E7E;  // EndStateMgr_FieldControlLock_Clear(sm, 1, reason)
inline constexpr std::uintptr_t kSqueezeLockCall    = 0x14102FE4D;  // StateMgr_QueueLockBit_Set(0, reason)
inline constexpr std::uintptr_t kSqueezeUnlockCall  = 0x14102E3EA;  // StateMgr_QueueLockBit_Clear(0, reason)
inline constexpr std::uintptr_t kFieldControlLockSet   = 0x14113ED90;
inline constexpr std::uintptr_t kFieldControlLockClear = 0x14113F0C0;
inline constexpr std::uintptr_t kQueueLockBitSet       = 0x140FCCA20;
inline constexpr std::uintptr_t kQueueLockBitClear     = 0x140FCCAF0;
/// The field camera's flavors (per operator: kOperatorFlavors + 8 x id, kFlavorActive above): 15 ladder, 17 duct (squeeze,
/// crawl), 20 the customizable one (monkey bars). The game's deactivate resolves the operator from g_EndGameRoot+0x9B0.
inline constexpr std::uintptr_t kFieldCameraDeactivateFlavor = 0x140FCE480;  // void(u8 id)
inline constexpr std::array<std::uint8_t, 8> kFieldCameraDeactivateFlavorEntry{0x48, 0x8B, 0x05, 0x49, 0xB5, 0x81, 0x04, 0x44};
inline constexpr std::uint8_t   kFlavorLadder = 15, kFlavorDuct = 17, kFlavorCustomizable = 20;  // 18 rope/pole: kFlavorRope
/// The duct flavor's leave, as the squeeze's own exit calls it (0x141031A30 via GetFlavor(17)): with weight left (+0x4CC)
/// it starts its out phase and eases back to the follow camera, else it switches itself off. void(flavor).
inline constexpr std::uintptr_t kDuctFlavorLeave = 0x1410AC620;
inline constexpr std::array<std::uint8_t, 16> kDuctFlavorLeaveEntry{
    0xF3, 0x0F, 0x10, 0x81, 0xCC, 0x04, 0x00, 0x00, 0x48, 0x8B, 0xD1, 0x0F, 0x54, 0x05, 0x6E, 0x89};
/// The duct flavor's fields read by the squeeze-camera watch (research/RE-traversal-duct-camera-two.md; read only).
/// The squeeze's camera enter (0x1410308A0) seeds a side-on view (the gap end's yaw +-90); its phase 1 snapshots the
/// operator's pose and eases from it to that view over the in time (a cut when that time is ~0).
inline constexpr std::uint32_t kDuctBegun = 0x39, kDuctOutputYaw = 0x250, kDuctWeight = 0x344, kDuctTargetYaw = 0x44C;
inline constexpr std::uint32_t kDuctSeeded = 0x460, kDuctSeedYaw = 0x468, kDuctAnchorWeight = 0x470, kDuctSeedRequest = 0x48C;
inline constexpr std::uint32_t kDuctInTime = 0x490, kDuctStartYaw = 0x4A4, kDuctYawDelta = 0x4C0, kDuctPhase = 0x510;
inline constexpr std::uint32_t kDuctPhaseTimer = 0x524, kDuctPendingPhase = 0x518;  // pending: the entry the next tick enters
/// The duct flavor's leaving phases (out, then return): its current phase entry (+0x510) has one of these kinds.
inline constexpr std::uint32_t kDuctPhaseOut = 3, kDuctPhaseReturn = 4;
/// The squeeze's near-exit delegate (animation side, void(capture* = {state}, char end); research/RE-traversal-duct-
/// camera-two.md): fired by the squeeze's behaviour 70 cm before an end, it records the member's end spot and, for the
/// party leader's type only, starts the duct camera's leave. Without it the camera stays in its hold, whose anchor
/// fades to the squeezer's facing: a 90-degree turn mid-squeeze. Same prologue as the beam's end delegate.
inline constexpr std::array<std::uint8_t, 16> kSqueezeNearExitDelegateEntry{
    0x48, 0x89, 0x74, 0x24, 0x20, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0xA9, 0x48, 0x81, 0xEC, 0xB0, 0x00};
inline constexpr platform::FunctionFact kSqueezeNearExitDelegate{L"FieldActionCrackState_ExtraAction_Delegate800_DuctIfLeader",
                                                                 0x141031A30, kSqueezeNearExitDelegateEntry};
inline constexpr std::size_t kSqueezeNearExitDelegateFirst = 5;
/// The crawl's near-exit delegate (same signal, same leader-type-only leave of the duct camera, if begun): its first
/// instruction is too short for an entry redirect, so the stub that reaches it (movzx edx, byte [rdx]; jmp) has its
/// tail jump redirected.
inline constexpr std::uintptr_t kCrawlNearExitDelegate = 0x1410364E0;
inline constexpr std::uintptr_t kCrawlNearExitJump     = 0x14104D533;  // jmp rel32 in sub_14104D530
/// The ladder behaviour's phase delegate (animation side, void(capture* = {state}, char phase); research/RE-traversal-
/// phases-two.md §2): on the exit phases (3 top exit and 11 jump off: flag 1, no end-yaw turn; 6 bottom exit and 10
/// slide landing: flag 0) it asks the ladder camera (flavor 15) to leave, for the party leader's type only and outside a
/// cutscene; it reads only the capture's state. Its first instruction is too short for an entry redirect; its one
/// caller is a stub (movzx edx, byte [rdx]; jmp) whose tail jump is redirected (block 0x141093F20).
inline constexpr std::array<std::uint8_t, 14> kLadderPhaseDelegateEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x0F, 0xB6, 0xC2, 0x41, 0x83, 0xE8, 0x03};
inline constexpr platform::FunctionFact kLadderPhaseDelegate{L"FieldActionLadder_CameraPhaseDelegate_Flavor15_LeaderType",
                                                             0x1410548F0, kLadderPhaseDelegateEntry};
inline constexpr std::uintptr_t kLadderPhaseJump = 0x141093F23;  // E9 C8 09 FC FF
inline constexpr std::uint8_t   kLadderTopExit = 3, kLadderBottomExit = 6, kLadderSlideLanding = 10, kLadderJumpOff = 11;
/// The ladder camera's leave request (void(flavor, char keepYaw)): if the flavor is active, its pending phase becomes
/// the Leave entry; the Leave eases back and switches the flavor off itself. Its phase machine's current and pending
/// entries (+0x3F0, +0x3F8: pointers to entries whose first int is the kind; 2 = Leave).
inline constexpr std::array<std::uint8_t, 16> kLadderLeaveEntry{
    0x80, 0x79, 0x38, 0x00, 0x4C, 0x8B, 0xC1, 0x0F, 0x84, 0xA3, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x10};
inline constexpr platform::FunctionFact kLadderLeave{L"FEndCameraFlavorLadder_RequestState2", 0x1410AED90, kLadderLeaveEntry};
inline constexpr std::uint32_t kLadderPhaseCurrent = 0x3F0, kLadderPhasePending = 0x3F8, kLadderPhaseLeave = 2;
/// A field-action state's done byte (the component's tick ends the action when it is set).
inline constexpr std::uint32_t kStateDone = 0x38;
/// The gap jump's long-jump finish (animation side, void(capture* = {state}); research/RE-traversal-deferred-two.md §3):
/// sets the state done, then ticks the owner's component there (the jump's end and the queued slide-down start), outside
/// the mod's scopes. First instructions mov rax,[rcx]; mov byte [rax+38h],1 (7 bytes, aligned, no RIP-relative operand).
inline constexpr std::array<std::uint8_t, 16> kGapJumpFinishEntry{
    0x48, 0x8B, 0x01, 0xC6, 0x40, 0x38, 0x01, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0x48, 0x30, 0x44, 0x8B};
inline constexpr platform::FunctionFact kGapJumpFinish{L"FieldActionParkourJump_OnFinish_TickOwnerComp", 0x141093D40, kGapJumpFinishEntry};
inline constexpr std::size_t           kGapJumpFinishFirst = 7;
/// The pole slide's finish (animation side, void(capture* = {state, u8 wasLeader})): done, then if the "was the leader"
/// byte its slide start captured (+8; set for a seat's member, whose start runs in the seat's scopes) is set, the leave
/// of the rope/pole camera (flavor 18) on "the" operator, player 1's from there (§4). First instructions sub rsp,28h;
/// mov rax,[rcx] (7 bytes, aligned).
inline constexpr std::array<std::uint8_t, 16> kPoleFinishEntry{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x01, 0xC6, 0x40, 0x38, 0x01, 0x80, 0x79, 0x08, 0x00, 0x74};
inline constexpr platform::FunctionFact kPoleFinish{
    L"FieldActionPoleSliderState_ExtraAction_OnFinish_SetDone_Flavor18LeaveIfWasLeader_AnimSide", 0x141093CD0, kPoleFinishEntry};
inline constexpr std::size_t   kPoleFinishFirst     = 7;
inline constexpr std::uint32_t kPoleFinishWasLeader = 8;
/// The rope/pole camera (flavor 18) leave (void(flavor)): nothing if the current phase (+0x4E0, an entry whose first int is
/// its kind) is past kind 1, else the pending phase (+0x4E8) becomes the way out.
inline constexpr std::uint8_t kFlavorRope = 18;
inline constexpr std::array<std::uint8_t, 16> kRopeFlavorLeaveEntry{
    0x48, 0x8B, 0x81, 0xE0, 0x04, 0x00, 0x00, 0x4C, 0x8B, 0xC1, 0x48, 0x85, 0xC0, 0x74, 0x09, 0x83};
inline constexpr platform::FunctionFact kRopeFlavorLeave{L"FEndCameraFlavor18Rope_BeginLeave_ASSUMED", 0x1410BBDF0, kRopeFlavorLeaveEntry};
inline constexpr std::uint32_t kRopePhaseCurrent = 0x4E0, kRopePhasePending = 0x4E8;
/// The wire-fence climb's state (§5): its Start (camera flavor 20 for the leader type; for the party leader the doorway
/// markers and the actor's "the party leader is on this mesh" byte, +0x4D0, which AI members follow) and Exit (the end
/// signals through a direct call, 0x141084516, fits its block; for the party leader the markers cleared and +0x4D0
/// cleared; the stored camera eased out). The state's actor is its weak pair at +0x120.
inline constexpr std::uintptr_t kWireFenceStartSlot = 0x144BB6EC0, kWireFenceExitSlot = 0x144BB6EC8;
inline constexpr std::array<std::uint8_t, 16> kWireFenceStartEntry{
    0x4C, 0x8B, 0xDC, 0x55, 0x56, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0x90, 0x48, 0x81, 0xEC, 0x70, 0x01};
inline constexpr platform::FunctionFact kWireFenceStart{L"FieldActionWireMeshMoveState_Start_Flavor20IfLeaderType_Set4D0IfPMLeader",
                                                        0x141083480, kWireFenceStartEntry};
inline constexpr std::array<std::uint8_t, 16> kWireFenceExitEntry{
    0x4C, 0x8B, 0xDC, 0x55, 0x57, 0x41, 0x55, 0x49, 0x8D, 0xAB, 0xF8, 0xFE, 0xFF, 0xFF, 0x48, 0x81};
inline constexpr platform::FunctionFact kWireFenceExit{L"FieldActionWireMeshMoveState_Exit_Clear4D0IfPMLeader_Flavor20Leave",
                                                       0x1410841B0, kWireFenceExitEntry};
inline constexpr std::uintptr_t kWireFenceEndSignalsCall = 0x141084516;  // E8 95 98 E6 FF
/// The fence Start's first call, the base start, ends by tail-jumping the state's vt+0x60, which for this state is the
/// leader-typed OnOff 25 (FieldActionState_vt60_OnOff25IfLeader 0x141026AA0; the Exit's vt+0x68 releases it by the
/// state's own flag +0x54): a global switch, so it runs under the pause in a resumed Start.
inline constexpr std::uintptr_t kWireFenceBaseStartCall = 0x1410834BA;  // E8 D1 2A FA FF
inline constexpr std::uintptr_t kStateBaseStart = 0x141025F90;  // FieldActionState_BaseStart_ASSUMED, void(state)
inline constexpr std::uintptr_t kEndSignals = 0x140EEDDB0;  // FieldActionBase_SendEndStateTriggers_LeaderType_ASSUMED
inline constexpr std::uint32_t  kStateActorWeak = 0x120, kWireFenceLeaderOn = 0x4D0;
inline constexpr std::uint32_t kOperatorPoseYaw = 0x42C;  // the operator's pose rotation (+0x428) yaw, phase 1's start
/// A scene component's world transform rotation (FQuat x, y, z, w; the duct update reads the owner's root here as the
/// anchor its hold fades to).
inline constexpr std::uint32_t kComponentWorldRotation = 0x1A0;
/// A field-action component's states (by action type) and the squeeze state's path byte, side and end yaw.
inline constexpr std::uint32_t kFieldActionStates = 0x10;
inline constexpr std::uint32_t kSqueezeFromPoint = 0x3A, kSqueezeSide = 0x70, kSqueezeEndYaw = 0x84;

// --- Field targets: chests, ground items, doors for a further player (research/RE-chest-two.md §8) ---------------------
/// The field target manager (g_EndGameRoot+0x158, one for the game's life): its pick each frame (the nearest target, the
/// icons, the prompt row) and its press (FieldAction on the current target), both void(mgr, float dt), each called once
/// by a game-mode tick on the game thread. The pick's call crosses a 16-byte block, the press's fits.
inline constexpr std::uint32_t  kRootFieldTargets     = 0x158;
inline constexpr std::uintptr_t kFieldTargetPick      = 0x14101C140;  // FieldActionMgr__TickTargetIcons_LeaderBased
inline constexpr std::uintptr_t kFieldTargetPickCall  = 0x1416BBDDE;  // E8 5D 03 96 FF
inline constexpr std::uintptr_t kFieldTargetPress     = 0x14101BE20;  // FieldActionMgr__TickPressed_LeaderBased
inline constexpr std::uintptr_t kFieldTargetPressCall = 0x140FF5DFB;  // E8 20 60 02 00
/// The pick's screen test asks for "the first local player" (UWorld::GetFirstLocalPlayerFromController, void*(world)):
/// a row flagged NeedScreenSpace passes only on that player's screen. Only caller on this path; crosses a block.
inline constexpr std::array<std::uint8_t, 16> kFirstLocalPlayerEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x56, 0x48, 0x83, 0xEC, 0x30, 0xC7, 0x44, 0x24, 0x28, 0x00, 0x00};
inline constexpr platform::FunctionFact kFirstLocalPlayer{L"UWorld::GetFirstLocalPlayerFromController", 0x143370570,
                                                          kFirstLocalPlayerEntry};
inline constexpr std::uintptr_t kFieldTargetScreenCall = 0x141017B7F;  // E8 EC 89 35 02
/// The manager's per-player state (§8.3): the last pick's visited entries (TArray, inline 64) and its name-to-trace map
/// (TMap) in one range; the current target (name, state name, actor tag), its prompt text (FString), hold timer and
/// duration, the trimmed nearest name and its location, the last pressed name; FieldAction held / pressed / latch;
/// the far-icon sweep timer. The ctor zeroes all of it but the map's first free index (-1).
inline constexpr std::uint32_t kTargetsVisited = 0x130, kTargetsVisitedSize = 0x260;  // +0x130..+0x38F
inline constexpr std::uint32_t kTargetsMapFirstFree = 0x370;
inline constexpr std::uint32_t kTargetsCurrent = 0xBD00, kTargetsCurrentSize = 0x4C;  // +0xBD00..+0xBD4B
inline constexpr std::uint32_t kTargetsState = 0xBD08, kTargetsPromptText = 0xBD18;
inline constexpr std::uint32_t kTargetsHeld = 0xBD57, kTargetsPressed = 0xBD58, kTargetsLatch = 0xBD59;  // 3 bytes
inline constexpr std::uint32_t kTargetsSweepTimer = 0xBD5C;                                            // 4 bytes
/// The heap blocks a copy of that state owns (the dtor frees the same five): visited heap, map data, map bits, map hash,
/// prompt text.
inline constexpr std::array<std::uint32_t, 5> kTargetsOwned{0x330, 0x340, 0x360, 0x380, 0xBD18};
/// The pool's 256 items (stride 0xB8 from +0x390): each entry's "was the current target last pick" byte (item +0xA5,
/// entry +0x8D), which widens its range check.
inline constexpr std::uint32_t kTargetsPool = 0x390, kTargetsItemSize = 0xB8, kTargetsItemWasCurrent = 0xA5;
inline constexpr std::size_t   kTargetsItems = 256;
/// The press's inline FieldAction read (the caller of the press): the gate, then the held and pressed getters on a copy
/// of the FieldAction key (24 bytes: FName, details object, reference controller; each getter releases the copy's
/// reference), into held / pressed, the latch cleared on release.
inline constexpr std::uintptr_t kFieldActionKey = 0x145903B48;
inline constexpr std::array<std::uint8_t, 13> kFieldActionGateEntry{0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x40, 0x32, 0xFF};
inline constexpr platform::FunctionFact kFieldActionGate{L"FieldAction_PressGate_CamLockUi19Pause", 0x140FD5E40, kFieldActionGateEntry};
inline constexpr std::array<std::uint8_t, 16> kActionHeldByKeyEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48};
inline constexpr platform::FunctionFact kActionHeldByKey{L"Input_IsActionHeld_ByKey", 0x1416E26C0, kActionHeldByKeyEntry};
inline constexpr std::array<std::uint8_t, 16> kActionPressedByKeyEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xD9};
inline constexpr platform::FunctionFact kActionPressedByKey{L"Input_IsActionPressed_ByKey", 0x1416E25C0, kActionPressedByKeyEntry};
/// The press writes the push-button manager's FieldAction held byte (g_EndGameRoot+0x1C8, +8) from the game's own read.
inline constexpr std::uint32_t kRootPushButtons = 0x1C8, kPushButtonsHeld = 8;
/// The dispatch's row tables (each an array of three UDataTable*) and the game's lookup over them, void*(tables, FName);
/// its prefix test, bool(FName*, const char*).
inline constexpr std::array<std::uint8_t, 16> kFindRowInTablesEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48};
inline constexpr platform::FunctionFact kFindRowInTables{L"DataTableSet3_FindRowByName", 0x140B61EF0, kFindRowInTablesEntry};
inline constexpr std::array<std::uint8_t, 11> kNameStartsWithEntry{0x40, 0x53, 0x56, 0x57, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00};
inline constexpr platform::FunctionFact kNameStartsWith{L"FName_StartsWithAnsi_NoCase", 0x140AFB480, kNameStartsWithEntry};
inline constexpr std::uintptr_t kTableSituationTrigger = 0x145843EF0;  // keyed by the state name: talk and events
inline constexpr std::uintptr_t kTableObjectLayout     = 0x1458428D0;
inline constexpr std::uintptr_t kTableObjectActor      = 0x145842750;
inline constexpr std::uintptr_t kTableObjectEnemy      = 0x145842810;
inline constexpr std::uintptr_t kTableObjectTreasure   = 0x145842B70;  // chests and ground items
inline constexpr std::uintptr_t kTableObjectParty      = 0x145842AB0;
/// The chests waiting for their open (state manager +0x10D4C80: a pointer to a TArray<FName>; the dispatch appends, the
/// open's end pops the head) and the open's Begin (vtable slot; it takes the head's chest and, unless the act row names
/// an actor, the party leader at that moment as the opener: char(runner)). The runner's chest name (+0x6C, unaligned)
/// and "the opener is the leader" (+0x8C).
inline constexpr std::uint32_t  kStateChestQueue = 0x10D4C80;
inline constexpr std::uintptr_t kChestOpenBeginSlot = 0x144B913D8;
inline constexpr std::array<std::uint8_t, 16> kChestOpenBeginEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56};
inline constexpr platform::SlotFact kChestOpenBegin{L"the chest open's Begin", kChestOpenBeginSlot,
                                                    {L"ActTreasure_Begin_ActorNamedElseLeader", 0x140E16E60, kChestOpenBeginEntry}};
inline constexpr std::uint32_t kChestRunnerName = 0x6C, kChestRunnerActor = 0x28, kChestRunnerOpenerIsLeader = 0x8C;

// --- Push-button events for a further player (RE-further-player-levers.md) -----------------------------------------------
/// The push-button manager (g_EndGameRoot+0x1C8): 64 event objects (0xA0 bytes each from +0x10), its active list (a linked
/// list: the head at +0x3038, 0 when empty; a node's next at +8 and its slot at +0x18) and the lock its dispatcher takes.
inline constexpr std::uint32_t  kPushButtonsObjects    = 0x10;
inline constexpr std::uint32_t  kPushObjStride         = 0xA0;
inline constexpr std::int32_t   kPushSlotCount         = 64;
inline constexpr std::uint32_t  kPushButtonsActiveHead = 0x3038;
inline constexpr std::uint32_t  kPushNodeNext          = 0x08;
inline constexpr std::uint32_t  kPushNodeSlot          = 0x18;
inline constexpr std::uintptr_t kPushButtonsLock       = 0x14584E0B0;  // a CRITICAL_SECTION
/// An event object: its step (the game's switch), its request name (the row's id, the key of the finished-animation
/// notify) and its held byte (copied from the manager's FieldAction byte before each step).
inline constexpr std::uint32_t kPushObjStep     = 0x0C;
inline constexpr std::uint32_t kPushObjActionId = 0x2C;
inline constexpr std::uint32_t kPushObjHeld     = 0x90;
inline constexpr std::int32_t kPushStepLevelWait = 5, kPushStepPhaseZero = 6, kPushStepHold = 7, kPushStepPress = 8, kPushStepPhaseTwoWait = 9,
                              kPushStepCancelWait = 11, kPushStepEnd = 12, kPushStepFreed = 13;
/// An ObjectLayout row's PushButtonActionID (FName), the push-button table set (three UEndDataTable*, the dispatch's
/// shape) and its row: the actor names (TArray<FName>) and the actions (TArray<FName>, four per actor).
inline constexpr std::uint32_t  kLayoutPushButtonId    = 0x68;
inline constexpr std::uintptr_t kTablePushButtonAction = 0x145843590;
inline constexpr std::uint32_t  kPushRowActorNames = 0x18, kPushRowActorCount = 0x20, kPushRowActions = 0x28, kPushRowActionCount = 0x30;
/// The four calls: the press's Init of an event (in FieldActionMgr__TickPressed_LeaderBased), the field step's step and free
/// of each active event, and the dispatcher's finished-animation notify; each inside one 16-byte block.
inline constexpr std::uintptr_t kPushInitCall   = 0x14101C0B9;  // E8 B2 28 10 00
inline constexpr std::uintptr_t kPushStepCall   = 0x140FF5AA1;  // E8 4A 97 12 00
inline constexpr std::uintptr_t kPushNotifyCall = 0x140FF71C7;  // E8 B4 9B 12 00
inline constexpr std::uintptr_t kPushFreeCall   = 0x140FF5AE7;  // E8 C4 94 12 00
inline constexpr std::array<std::uint8_t, 16> kPushInitEntry{0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x40};
inline constexpr platform::FunctionFact kPushInit{L"PushButtonEvent_Init_StoreLeaderWeak", 0x14111E970, kPushInitEntry};
inline constexpr std::array<std::uint8_t, 16> kPushStepEntry{0x40, 0x55, 0x41, 0x56, 0x48, 0x8D, 0xAC, 0x24, 0x48, 0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xB8};
inline constexpr platform::FunctionFact kPushStep{L"EventControl_TickSteps_PushButton", 0x14111F1F0, kPushStepEntry};
inline constexpr std::array<std::uint8_t, 16> kPushNotifyEntry{0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56};
inline constexpr platform::FunctionFact kPushNotify{L"EventControl_OnActorRequestFinished", 0x141120D80, kPushNotifyEntry};
inline constexpr std::array<std::uint8_t, 16> kPushFreeEntry{0x40, 0x57, 0x41, 0x54, 0x48, 0x83, 0xEC, 0x38, 0x48, 0x63, 0x81, 0x88, 0x00, 0x00, 0x00, 0x45};
inline constexpr platform::FunctionFact kPushFree{L"PushButtonEvent_Free_OnOffsOff", 0x14111EFB0, kPushFreeEntry};
/// The operation board state's Exit (a vtable slot): its end triggers go to the level as the party leader's.
inline constexpr std::uintptr_t kBoardExitSlot = 0x144BB75C8;
inline constexpr std::array<std::uint8_t, 16> kBoardExitEntry{0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0xD9, 0xE8, 0x1E, 0xD2};
inline constexpr platform::FunctionFact kBoardExit{L"FieldActionOperationBoardState_Exit_ClearCtrlFlags3000", 0x1410592E0, kBoardExitEntry};
/// A party member's field AI while a further player's event runs on it (RE-further-player-levers.md §14.8): its follow
/// behaviour reads the party leader every frame, in the AI's per-frame update (AEndFieldAIController_UpdateActiveKind,
/// the controller's own tick function, run on a worker thread: the state machine, a direct turn toward the leader, the
/// AI model) and in its actor tick (AEndFieldAIController_Tick, the game thread: the behaviour tree's leader key). A
/// member that is not the leader turns toward the leader during its hold, and the pick drops the lever. Both are vtable
/// slots (void(controller, float dt)) of the field AI controller's class and its three subclasses.
inline constexpr std::array<std::uintptr_t, 4> kFieldAiControllerVtables{0x144BB5210, 0x144CF72E0, 0x144CF7E90, 0x144CF89B0};
inline constexpr std::uint32_t kFieldAiUpdateSlot = 0x7D8, kFieldAiTickSlot = 0x408;
inline constexpr std::array<std::uint8_t, 16> kFieldAiUpdateEntry{0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x80, 0xB9, 0xF8, 0x0D, 0x00, 0x00, 0x10, 0x48, 0x8B, 0xD9};
inline constexpr platform::FunctionFact kFieldAiUpdate{L"AEndFieldAIController_UpdateActiveKind", 0x140FBF630, kFieldAiUpdateEntry};
inline constexpr std::array<std::uint8_t, 16> kFieldAiTickEntry{0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x0F, 0x29, 0x74, 0x24, 0x20, 0x48, 0x8B, 0xD9, 0x0F, 0x28};
inline constexpr platform::FunctionFact kFieldAiTick{L"AEndFieldAIController_Tick", 0x140FBF5C0, kFieldAiTickEntry};
/// The pool's items as the pick and the target removal read them (item = entry - 0x18): the target name (entry +0x0C),
/// its state-object name (entry +0x14, the dispatch's SituationTrigger key), the icon widget (entry +0x60), the location
/// dirty byte (entry +0x92: posted outside the pick, consumed by the first pick that meets it) and the disabled byte
/// (entry +0x94: the pick's rejection test, next to "in use" +0x90; no icon, never the current target). The active
/// items are a list from mgr+0xBC50 through item +0x08 (RemoveTargetByName's own walk).
inline constexpr std::uint32_t kTargetsItemEntry = 0x18, kTargetsItemName = 0x24, kTargetsItemStateName = 0x2C;
inline constexpr std::uint32_t kTargetsItemWidget = 0x78, kTargetsItemDirty = 0xAA, kTargetsItemDisabled = 0xAC;
inline constexpr std::uint32_t kTargetsActiveHead = 0xBC50, kTargetsItemNext = 0x08;
/// The entry's place (an FVector at entry +0x24 = item +0x3C; entry +0x3C is its row A: AddTarget's location argument,
/// RE-talk-far.md §4.1), the
/// pressed object's own spot. A talk target carries its state object's name (an ObjectActor row, "oba030_..."), which
/// the character registry's lookup by name does not know (none found live): the entry is the game's own
/// record of where the talked-to object stands, read while it exists (the talk state removes it a tick later).
inline constexpr std::uint32_t kTargetsEntryLocation = 0x24;
/// The pick's pending write of one entry's disabled byte (the field-action component's start/end posts it): an 8-byte
/// flag at +0xBD4C, the entry named by +0xBD44 (inside the per-player copy), the value (0 or 1) at +0xBD54. The pick
/// writes that value into the named entry's byte and clears the flag (the name too when the value is 0).
inline constexpr std::uint32_t kTargetsPendingEnable = 0xBD4C, kTargetsPendingName = 0xBD44;

// --- The show-menu act (type 43) and the shop UI (RE-shop-two.md §2.1, §8) ----------------------------------------
// The act factory creates it with FEndActionPERelativeCamera__Ctor(runner, &act id, type, channel, flag) at one call
// (E8, inside block 0x140DABED0), which runs its Begin; its vtable (this act's alone) holds the dtor (+0x00,
// (runner, free)), OnFinish (+0x10, (runner)) and Update (+0x20, (runner, float dt)). The act row's kind is its
// IntegerArgumentList[0] (row +0xD8 data, +0xE0 count): 1 = a shop. The action rows: g_DT_Action, a set of three
// tables (DataTableSet3_FindRowByName). EndUISystem[44] is the shop UI; its byte +0x370 is "a shop is up" (the act's
// own end test).
inline constexpr std::uintptr_t kShowMenuActCtorCall = 0x140DABED3;
inline constexpr std::array<std::uint8_t, 16> kShowMenuActCtorEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x0F, 0xB6, 0x44, 0x24, 0x60, 0x48, 0x8B, 0xD9, 0x88, 0x44};
inline constexpr platform::FunctionFact kShowMenuActCtor{L"FEndActionPERelativeCamera__Ctor", 0x140DF5E40, kShowMenuActCtorEntry};
inline constexpr std::array<std::uint8_t, 16> kShowMenuActDtorEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8D, 0x05, 0x0F, 0xBB, 0xD9};
inline constexpr std::array<std::uint8_t, 16> kShowMenuActFinishEntry{
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0xF1};
inline constexpr std::array<std::uint8_t, 16> kShowMenuActUpdateEntry{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x70, 0x20, 0x55, 0x57, 0x41, 0x54, 0x41};
inline constexpr std::uintptr_t kShowMenuActVtable = 0x144B919C0;
inline constexpr platform::SlotFact kShowMenuActDtor{L"the show-menu act's destructor", kShowMenuActVtable + 0x00,
                                                     {L"FEndActionPERelativeCamera__Dtor", 0x140DF5EA0, kShowMenuActDtorEntry}};
inline constexpr platform::SlotFact kShowMenuActFinish{L"the show-menu act's OnFinish", kShowMenuActVtable + 0x10,
                                                       {L"FEndActionPERelativeCamera__OnFinish", 0x140DF5F50, kShowMenuActFinishEntry}};
inline constexpr platform::SlotFact kShowMenuActUpdate{L"the show-menu act's Update", kShowMenuActVtable + 0x20,
                                                       {L"FEndActionPERelativeCamera__Update", 0x140DF6080, kShowMenuActUpdateEntry}};
inline constexpr std::uintptr_t kTableAction       = 0x145839B80;  // g_DT_Action, three tables
inline constexpr std::uint32_t  kActionRowIntegers = 0xD8;         // FEndDataTableAction::IntegerArgumentList (TArray<int32>)
inline constexpr std::uint32_t  kShopSlot          = 44;           // EndUISystem[44]
inline constexpr std::uint32_t  kShopUp            = 0x370;        // the shop UI's "a shop is up" byte

// --- A further player's talk: player 1's character brought over (RE-talk-far.md §3, §4, §6) ----------------------
// The act tick ActSequence_TickSteps(channel, float dt, u8* out), called at two sites of the state manager's tick
// (E8, each inside its block). The game's navmesh query Nav_FindPointAlongDirInAreas(FVector* out, origin, dir, float
// distance, agent props (a 32-byte default at kNavAgentDefault), 0, 0, 0, 0) -> found, as its catch-up teleport calls
// it; the battle-warp move EndCharacter_BattleTeleport_FadeIn(char, loc, FRotator*, direct, fade) (stops the AI move,
// Party_TeleportCharacterTo, a 0.5 s fade-in); the field camera reset FieldCamera_ResetBehindLeader(params, 32 bytes,
// all zero as the game's own init).
inline constexpr std::array<std::uint8_t, 16> kActTickEntry{
    0x4C, 0x89, 0x44, 0x24, 0x18, 0x41, 0x55, 0x41, 0x56, 0x48, 0x81, 0xEC, 0xC8, 0x00, 0x00, 0x00};
inline constexpr platform::FunctionFact kActTick{L"ActSequence_TickSteps", 0x140FB64B0, kActTickEntry};
inline constexpr std::array<std::uintptr_t, 2> kActTickCalls{0x14113D3F7, 0x14113D457};
inline constexpr std::array<std::uint8_t, 16> kNavPointAlongDirEntry{
    0x40, 0x53, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x81, 0xEC, 0xE0, 0x00, 0x00, 0x00};
inline constexpr platform::FunctionFact kNavPointAlongDir{L"Nav_FindPointAlongDirInAreas", 0x1415C3790, kNavPointAlongDirEntry};
inline constexpr std::array<std::uint8_t, 16> kBattleTeleportFadeInEntry{
    0x48, 0x85, 0xC9, 0x0F, 0x84, 0x2A, 0x01, 0x00, 0x00, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x41};
inline constexpr platform::FunctionFact kBattleTeleportFadeIn{L"EndCharacter_BattleTeleport_FadeIn", 0x140A029E0,
                                                              kBattleTeleportFadeInEntry};
inline constexpr std::array<std::uint8_t, 16> kFieldCameraResetBehindLeaderEntry{
    0x48, 0x8B, 0x05, 0x19, 0xB7, 0x81, 0x04, 0x4C, 0x8B, 0xC9, 0x44, 0x8B, 0x80, 0xB4, 0x09, 0x00};
inline constexpr platform::FunctionFact kFieldCameraResetBehindLeader{L"FieldCamera_ResetBehindLeader", 0x140FCE2B0,
                                                                      kFieldCameraResetBehindLeaderEntry};
/// An act channel's act id (FName, +0x10); an action row's type (+0xC; 43 = the show-menu act).
inline constexpr std::uint32_t  kActChannelAct         = 0x10;
inline constexpr std::uint32_t  kActChannelStep        = 0x135;  // the channel's step byte (2: the lock taken)
inline constexpr std::uint32_t  kActionRowType         = 0xC;
inline constexpr std::int32_t   kActionTypeShowMenu    = 43;
/// The planner event acts (a talk, a staged conversation: both types build the planner event runner, RE-talk-far.md);
/// type 0 is a cutscene act.
inline constexpr std::int32_t   kActionTypePlanEvent   = 1, kActionTypePlanEventAlt = 16;
/// The field-action component's "an action runs" byte (component +0x15C, RE-shop-two.md §2.1, RE-talk-far.md §6).
inline constexpr std::uint32_t  kFieldActionRunning    = 0x15C;
inline constexpr std::uintptr_t kNavAgentDefault       = 0x1453ABA90;
inline constexpr std::size_t    kNavAgentSize          = 32;
inline constexpr std::size_t    kCameraResetParamsSize = 32;
/// The FName the game passes as CharaRegistry_FindActorByName's level when its field-target dispatch and its named-point
/// resolver look up an NPC by name (research/.ida-mcp/cats-ch3: FieldTarget_OnPressed_DispatchByKind_LeaderBased,
/// ResolveNamedPointTransform): read at the call, as the game does.
inline constexpr std::uintptr_t kNamedActorLevel       = 0x145879EE8;
/// The field target icon manager (EndUISystem[15], one for the game's life): its hold gauge's last ratio (+0x54, one
/// float for every widget: the "gauge full" sound plays when a ratio reaches 1 from below it); its icon map's element
/// count (+0x08) and free slots (+0x34): the live icons are their difference.
inline constexpr std::uint32_t  kTargetIconSlot       = 15;  // EndUISystem[15]
inline constexpr std::uint32_t  kTargetIconGaugeRatio = 0x54;
inline constexpr std::uint32_t  kTargetIconMapCount = 0x08, kTargetIconMapFree = 0x34;
/// The icon setter's one Show call (UEndFieldTargetIcon* (UI15, UWorld*, FName name by value, const FVector*, u8 state,
/// u8 type); fits its block) and, inside the widget creation it makes, the "first local player" call whose answer becomes
/// the widget's owner (FLocalPlayerContext at widget +0x1E8, weak {index, serial}; every later placement projects
/// through that player's controller; crosses a block) (research/RE-field-icons-two.md §2.1).
inline constexpr std::uintptr_t kTargetIconShow         = 0x141429770;  // EndUI15_FieldTargetIcon_Show
inline constexpr std::uintptr_t kTargetIconShowCall     = 0x14101B85A;  // E8 11 DF 40 00
inline constexpr std::uintptr_t kWidgetCreateOwnerCall  = 0x1428F842C;  // E8 3F 81 A7 00, to kFirstLocalPlayer
inline constexpr std::uint32_t  kWidgetOwnerPlayer      = 0x1E8;
/// The game's destroy of one icon (void(UI15, widget): hidden, removed from the manager's map, then destroyed).
inline constexpr std::array<std::uint8_t, 16> kTargetIconHideDestroyEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xED, 0x48, 0x8B};
inline constexpr platform::FunctionFact kTargetIconHideDestroy{L"EndUI15_FieldTargetIcon_HideDestroy", 0x141429EB0,
                                                               kTargetIconHideDestroyEntry};
/// The slot-15 tick (void(UI15)): every icon placed through its own owner's controller into the overlay's host set;
/// re-runnable per HUD pass with the game mode's HUD step at 0 (the widget clock then only telescopes).
inline constexpr std::array<std::uint8_t, 16> kTargetIconTickEntry{
    0x48, 0x89, 0x74, 0x24, 0x20, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0xA9, 0x48, 0x81, 0xEC, 0xA0, 0x00};
inline constexpr platform::FunctionFact kTargetIconTick{L"EndUISlot15_FieldTargetIcon_Tick", 0x14142B740, kTargetIconTickEntry};
/// The icon tick's one controller lookup, on the widget's player context (+0x1E8) before each icon's placement
/// (RE-field-icons-two.md §9): the only player read of the placement.
inline constexpr std::uintptr_t kTargetIconTickContextCall = 0x14142B907;  // E8 E4 B3 BE 01, to kPlayerContextController
/// The one path that frees a pool item (void(mgr, const FName*)): destroys the item's live widget, then frees the item.
/// First instruction mov [rsp+10h], rbx (5 bytes, no RIP-relative operand, aligned): the one relocated, exactly the
/// jump's length (then push rdi; sub rsp, 30h).
inline constexpr std::array<std::uint8_t, 16> kRemoveTargetEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x99, 0x50, 0xBC, 0x00};
inline constexpr platform::FunctionFact kRemoveTarget{L"FieldTargetMgr_RemoveTargetByName", 0x14101DCA0, kRemoveTargetEntry};
inline constexpr std::size_t           kRemoveTargetFirst = 5;
/// The game's lookup of an entry by target name (entry* (mgr, const FName*), the item + 0x18; null if none).
inline constexpr std::array<std::uint8_t, 16> kFindTargetEntryEntry{
    0x48, 0x8B, 0x81, 0x50, 0xBC, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x13, 0x48, 0x8B, 0x0A, 0x90};
inline constexpr platform::FunctionFact kFindTargetEntry{L"FieldTargetMgr_FindEntryByName", 0x14101DC70, kFindTargetEntryEntry};

// --- Sound: SEAD's one listener and the footstep gates (research/RE-audio-listener.md, pass S-RMK-AUDIO) -----------
// SEAD (Square Enix's middleware) measures every placed or moving sound from one listener object, which the SEAD audio
// device sets each audio frame from the engine's listener [0] only (player 1's view). Positions are UE world cm, axes
// as UE's (the push copies the translation as is). audio/listeners measures each sound from the nearest player's
// listener (RE §10: the build checks).

/// The SEAD listener singleton (SQEX::Sd::Lay::Listener*, 0xA0 bytes): its position at +0x30. The object holds its
/// vtable (one slot, the deleting destructor), padding at +0x08 and values only (no pointer, no count): a byte copy is
/// a listener of its own while its vtable is never called.
inline constexpr std::uintptr_t kSeadListenerGlobal = 0x1457E8F68;
inline constexpr std::uint32_t  kSeadListenerPosition = 0x30;
inline constexpr std::size_t    kSeadListenerSize     = 0xA0;
/// SeadLayListener_Get: `mov rax, [g_SeadLayListener]` (7 bytes, RIP-relative), `ret`, int3 padding to the next 16;
/// every reader of the listener calls it (21 calls in 17 functions), nothing enters it after its first byte.
inline constexpr std::array<std::uint8_t, 16> kSeadListenerGetEntry{
    0x48, 0x8B, 0x05, 0x61, 0x2A, 0x81, 0x01, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
inline constexpr platform::FunctionFact kSeadListenerGet{L"SeadLayListener_Get", 0x143FD6500, kSeadListenerGetEntry};
inline constexpr std::size_t           kSeadListenerGetFirst = 7;
/// SeadLayListener_Set (int(this, px, py, pz, fx, fy, fz, ux, uy, uz): position in xmm1-xmm3, the rest on the stack):
/// writes this+0x20 and +0x30 (the position), +0x40 front, +0x50 up, and the matrix +0x60 when it is finite; 0, or -1
/// (the matrix not finite, kept; the rest already written). SeadLayListener_UpdateVelocity (int(this)): +0x10 = +0x30 -
/// +0x20, then +0x20 = +0x30. Only `this` is written by either.
inline constexpr std::array<std::uint8_t, 16> kSeadListenerSetEntry{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20, 0x57};
inline constexpr platform::FunctionFact kSeadListenerSet{L"SeadLayListener_Set", 0x143FDD640, kSeadListenerSetEntry};
inline constexpr std::array<std::uint8_t, 16> kSeadListenerUpdateVelocityEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x70, 0x48, 0x8B, 0x05, 0x7B, 0x32, 0x3C, 0x01, 0x48, 0x33, 0xC4};
inline constexpr platform::FunctionFact kSeadListenerUpdateVelocity{L"SeadLayListener_UpdateVelocity", 0x143FDD840,
                                                                    kSeadListenerUpdateVelocityEntry};
/// SEAD's frame (SEAD_Lay_ThreadLoop_SeadLayUpdate, its own thread, every 32 ms): the commands (the listener push's Set
/// among them), then this call of the singleton's velocity update (no argument read; its return ignored), then the
/// sounds' tests in one loop and their updates in a second.
inline constexpr std::array<std::uint8_t, 16> kSeadListenerSpeedOfSingletonEntry{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x0D, 0xCD, 0x29, 0x81, 0x01, 0x0F, 0x28, 0xC8, 0xE8, 0x9D};
inline constexpr platform::FunctionFact kSeadListenerSpeedOfSingleton{L"SeadLayListener_UpdateVelocityOfSingleton",
                                                                      0x143FD6590, kSeadListenerSpeedOfSingletonEntry};
inline constexpr std::uintptr_t kSeadListenerSpeedCall = 0x143F9DD0B;  // E8, in the SEAD frame loop
/// The SEAD audio device's listener push (u64(device), vt+0xF0 of vt_SeadAudioDevice 0x144F75D98; audio thread
/// [ASSUMPTION]): only for the engine's main device; copies Listeners[0] (translation, front = Q.X, up = -(Q.Z)).
inline constexpr std::array<std::uint8_t, 16> kSeadPushListenerEntry{
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48};
inline constexpr platform::SlotFact kSeadPushListener{
    L"SEAD audio device: listener push (slot 30)", 0x144F75E88,
    {L"SeadAudioDevice_PushListener0ToSead", 0x1439E08C0, kSeadPushListenerEntry}};
/// FAudioDevice (stock 4.18): Listeners (audio thread) data / num, stride 0xB0, FTransform first (quat +0x00,
/// translation +0x10); ListenerTransforms (game thread, SetListener's copy) data / num, stride 0x30.
inline constexpr std::uint32_t kAudioDeviceListeners        = 0x78;
inline constexpr std::uint32_t kAudioDeviceListenersNum     = 0x80;
inline constexpr std::uint32_t kAudioDeviceListenerStride   = 0xB0;
inline constexpr std::uint32_t kAudioDeviceGameListeners    = 0x88;
inline constexpr std::uint32_t kAudioDeviceGameListenersNum = 0x90;
inline constexpr std::uint32_t kAudioDeviceGameListenerStride = 0x30;
inline constexpr std::uint32_t kTransformRotation           = 0x00;  // quaternion x, y, z, w
inline constexpr std::uint32_t kTransformTranslation        = 0x10;
/// The SEAD device's copy of what its push last sent (position, 3 floats; the push queues it as a SEAD command, which
/// SEAD's thread applies to the listener later), and its override byte (set: the push sends the override transform,
/// not Listeners[0]; no writer found).
inline constexpr std::uint32_t kSeadDeviceSentPosition = 0x788;
inline constexpr std::uint32_t kSeadDeviceOverride     = 0x794;
/// APlayerController::GetAudioListenerPosition(this, FVector& location, FVector& front, FVector& right) (vt+0xBB8,
/// called per view by the viewport client's Draw before SetListener, 0x142F66210).
inline constexpr std::uint32_t kControllerAudioListenerSlot = 0xBB8;

/// The SEAD sound classes' audibility tests (u64(object), vt+0x150; SEAD's own thread, SeadLayUpdate): set
/// object+0x190 to |listener - measured point| < object+0x194. Point and spherical wedge share one function, as do
/// polygon and cylindrical sector.
inline constexpr std::array<std::uint8_t, 16> kSeadDynamicAudibleEntry{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55};
inline constexpr std::array<std::uint8_t, 16> kSeadPointAudibleEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x0F, 0x29, 0x74, 0x24, 0x20, 0x48, 0x8B, 0xD9, 0x0F, 0x57};
inline constexpr std::array<std::uint8_t, 16> kSeadPolygonAudibleEntry{
    0x4C, 0x8B, 0xDC, 0x55, 0x41, 0x56, 0x49, 0x8D, 0x6B, 0xC8, 0x48, 0x81, 0xEC, 0x28, 0x01, 0x00};
inline constexpr platform::FunctionFact kSeadDynamicAudible{L"SEAD_LayDynamic_UpdateAudible", 0x143FE0050,
                                                            kSeadDynamicAudibleEntry};
inline constexpr platform::FunctionFact kSeadPointAudible{L"SEAD_LayPoint_UpdateAudible", 0x143FE0B50,
                                                          kSeadPointAudibleEntry};
inline constexpr platform::FunctionFact kSeadPolygonAudible{L"SEAD_LayPolygon_UpdateAudible", 0x143FE1890,
                                                            kSeadPolygonAudibleEntry};
inline constexpr platform::SlotFact kSeadDynamicAudibleSlot{L"SEAD moving sound: audibility (slot 42)", 0x1441FBCC0,
                                                            kSeadDynamicAudible};
inline constexpr platform::SlotFact kSeadPointAudibleSlot{L"SEAD placed sound: audibility (slot 42)", 0x1441FBE50,
                                                          kSeadPointAudible};
inline constexpr platform::SlotFact kSeadWedgeAudibleSlot{L"SEAD spherical wedge sound: audibility (slot 42)",
                                                          0x1441FC5F8, kSeadPointAudible};
inline constexpr platform::SlotFact kSeadPolygonAudibleSlot{L"SEAD polygon sound: audibility (slot 42)", 0x1441FC2E8,
                                                            kSeadPolygonAudible};
inline constexpr platform::SlotFact kSeadSectorAudibleSlot{L"SEAD cylindrical sector sound: audibility (slot 42)",
                                                           0x1441FC470, kSeadPolygonAudible};
/// The sound classes' updates (int(object, float seconds in xmm1), vt+0x58; SEAD's thread, the frame's second loop,
/// return ignored): the moving sound's doppler, which ends in the base update; the base update runs Calc3D (gain and
/// pan from the listener) and starts or stops the voice. Each reads the listener through the getter.
inline constexpr std::array<std::uint8_t, 16> kSeadDynamicUpdateEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x80, 0xB9, 0xEC, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9};
inline constexpr std::array<std::uint8_t, 16> kSeadBaseUpdateEntry{
    0x40, 0x55, 0x53, 0x56, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xB8, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC};
inline constexpr platform::FunctionFact kSeadDynamicUpdate{L"SEAD_LayDynamic_Update_Doppler", 0x143FE0780,
                                                           kSeadDynamicUpdateEntry};
inline constexpr platform::FunctionFact kSeadBaseUpdate{L"SEAD_LayObj_Update_Base", 0x143FDEEA0, kSeadBaseUpdateEntry};
inline constexpr platform::SlotFact kSeadDynamicUpdateSlot{L"SEAD moving sound: update (slot 11)", 0x1441FBBC8,
                                                           kSeadDynamicUpdate};
inline constexpr platform::SlotFact kSeadPointUpdateSlot{L"SEAD placed sound: update (slot 11)", 0x1441FBD58,
                                                         kSeadBaseUpdate};
inline constexpr platform::SlotFact kSeadWedgeUpdateSlot{L"SEAD spherical wedge sound: update (slot 11)", 0x1441FC500,
                                                         kSeadBaseUpdate};
/// A SEAD sound object's fields: the point the test measured (polygon and sector: the shape's nearest point to the
/// listener), the answer (u8), the audible range; a moving sound's world source and listener-relative flag (u8 1);
/// a placed sound's (and wedge's) place.
inline constexpr std::uint32_t kSeadObjectMeasured  = 0x180;
inline constexpr std::uint32_t kSeadObjectAudible   = 0x190;
inline constexpr std::uint32_t kSeadObjectRange     = 0x194;
inline constexpr std::uint32_t kSeadDynamicSource   = 0x1C0;
inline constexpr std::uint32_t kSeadDynamicRelative = 0x1F9;
inline constexpr std::uint32_t kSeadPointSource     = 0x1B0;

/// The engine sound's SEAD start (bool(source, wave instance), vt+0x18 of vt_SeadSoundSource 0x144F75D18): one per
/// sound played, the emission count the listeners must not change.
inline constexpr std::array<std::uint8_t, 16> kSeadSourceInitEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
inline constexpr platform::SlotFact kSeadSourceInit{L"SEAD sound source: init (slot 3)", 0x144F75D30,
                                                    {L"SEAD_SoundSource_Init", 0x1439E8B80, kSeadSourceInitEntry}};

/// AutoSe (the footstep and cloth sound detection, game thread [ASSUMPTION]), in its own space: metres, (x, z, y) of
/// UE's cm through the converter whose pointer is kAutoSeConverter (void* (out, in), 12-byte vectors). The frames
/// store their points in that space already.
/// Gate A: bool(const vec4* character, float distance) = |character - converted listener|^2 > distance^2, called
/// only from the processor update (the clip; true cuts the character's detection), the character being the frame's
/// +0x35C, +0x360, +0x364 in that order.
inline constexpr std::array<std::uint8_t, 16> kAutoSeBeyondClipEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x0F, 0x29, 0x74, 0x24, 0x50, 0x48, 0x8B, 0x05, 0xC6, 0x0C};
inline constexpr platform::FunctionFact kAutoSeBeyondClip{L"SeadAutoSe_IsBeyondClipDistance", 0x143FCFDF0,
                                                          kAutoSeBeyondClipEntry};
inline constexpr std::uintptr_t kAutoSeBeyondClipCall = 0x143F9C9D1;  // E8, in SeadAutoSeProc_Update_Clip
/// Gate C's source: void(record, analysis) stores record+0x60 = |listener - point|^2, which the rustle and swing
/// detectors compare with their range. The analysis's +0x20 is the frame; the point is the frame's +0x350, +0x354,
/// +0x358 (not the clip's +0x35C, which the analysis only copies to record+0x30); a frame with bit 2 of +0x380 set is
/// not measured (the analysis returns first).
inline constexpr std::array<std::uint8_t, 16> kAutoSeAnalyzeEntry{
    0x4C, 0x8B, 0xDC, 0x41, 0x56, 0x48, 0x81, 0xEC, 0x30, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x05, 0xC5};
inline constexpr platform::FunctionFact kAutoSeAnalyze{L"SeadAutoSeProc_AnalyzeFrame_ListenerDistSq", 0x143F9A4F0,
                                                       kAutoSeAnalyzeEntry};
inline constexpr std::uintptr_t kAutoSeAnalyzeCall   = 0x143F9CB44;  // E8, in SeadAutoSeProc_Update_Clip
inline constexpr std::uint32_t  kAutoSeAnalysisFrame = 0x20;
inline constexpr std::uint32_t  kAutoSeFramePoint     = 0x350;
inline constexpr std::uint32_t  kAutoSeFrameFlags     = 0x380;
inline constexpr std::uint8_t   kAutoSeFrameUnmeasured = 0x2;
inline constexpr std::uintptr_t kAutoSeConverter     = 0x1453A27F0;  // g_SeadAutoSe_PositionConverter
/// The two footstep sources: the animation notify (void(notify, mesh, animation, extra), vt+0x240 of
/// UEndAnimNotifyPlayCharacterFootSound) and AutoSe's event-to-sound callback (bool(callback, out, const event*),
/// vt+0x238 of UEndAutoSeCallback; the event's first int is its type, 1-6 the steps, lands and jumps).
inline constexpr std::array<std::uint8_t, 16> kFootNotifyEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x83, 0x3D, 0xE2, 0xC3, 0xFF, 0x04, 0x00, 0x48, 0x8B};
inline constexpr platform::SlotFact kFootNotify{L"character foot sound notify (slot 72)", 0x144B4E4A0,
                                                {L"EndAnimNotifyPlayCharaFootSound_Notify", 0x1407ED6C0, kFootNotifyEntry}};
inline constexpr std::array<std::uint8_t, 16> kAutoSeSoundForEventEntry{
    0x40, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0xB9, 0x00, 0x01, 0x00};
inline constexpr platform::SlotFact kAutoSeSoundForEvent{
    L"AutoSe callback: sound for an event (slot 71)", 0x144C1C328,
    {L"EndAutoSeCallback_GetSoundForEvent", 0x14167B460, kAutoSeSoundForEventEntry}};
inline constexpr int kAutoSeStepTypeFirst = 1;
inline constexpr int kAutoSeStepTypeLast  = 6;

// --- The Co-op row of the main menu and its page (research/RE-game-widgets.md §9-§10; menu/coop_menu) ----------------
// The main menu's left list (UEndMainTopMenu) is built at its construct from four tables indexed by the row's kind
// (0..12): the text and help (const wchar_t*[13], .rdata, read only by the row's setup, which copies them into a new
// FString), the order (u8[13], .rdata, read only by the build) and the unlock flag (FName[13], .data, written once by a
// static initializer): a kind is kept when its raw flag is 0 or that story flag is set. Kind 12 is the trial's "Buy
// Now" (flag stfCmn_IsTrialMode, never set in the full game); the mod gives it its text, help and
// a 0 flag, and puts it before System (11). A press broadcasts the kind to OpenWindowByKind.
inline constexpr std::uint8_t   kTopRowKind        = 12;
inline constexpr std::uintptr_t kTopRowTextSlot    = 0x1443C4780 + kTopRowKind * 8;  // g_MainTopMenu_ItemTextIds[12]
inline constexpr std::uintptr_t kTopRowTextWas     = 0x1443C8EA0;                    // its pointer, the trial's text id
inline constexpr std::uintptr_t kTopRowHelpSlot    = 0x1443C46E0 + kTopRowKind * 8;  // g_MainTopMenu_ItemHelpIds[12]
inline constexpr std::uintptr_t kTopRowHelpWas     = 0x1443C8970;
inline constexpr std::uintptr_t kTopRowUnlockSlot  = 0x145851F00 + kTopRowKind * 8;  // g_MainTopMenu_ItemUnlockFlag_cand[12]
/// g_MainTopMenu_ItemOrder (0x1443C47E8, 13 bytes 00..0C): its aligned second word holds kinds 8..12 and three bytes
/// past the table's end (the build stops at 0x1443C47F5); swapping 11 and 12 there puts the Co-op row before System.
inline constexpr std::uintptr_t kTopRowOrderWord   = 0x1443C47F0;
inline constexpr std::uint64_t  kTopRowOrderWas    = 0x0000000C0B0A0908ull;
inline constexpr std::uint64_t  kTopRowOrderCoop   = 0x0000000B0C0A0908ull;
/// void(FMainMenuMgr* mgr = EndUISystem[24], u8 kind): the bounds test (kind > 12: return) runs after the menu
/// tutorial's prelude and before the jump table, so kind 0xFF runs only the prelude. First instruction mov [rsp+8], rbx.
inline constexpr std::array<std::uint8_t, 16> kOpenWindowByKindEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x88, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};
inline constexpr platform::FunctionFact kOpenWindowByKind{L"EndUI24_MainMenu_OpenWindowByKind", 0x141463D30,
                                                          kOpenWindowByKindEntry};
inline constexpr std::uint8_t kOpenWindowPreludeOnly = 0xFF;
/// UEndMainTopMenu*(mgr, bool restoreCursor): the Top window shown again (title, focus, Show, cursor), current again;
/// what the game's Back from any main menu window ends in.
inline constexpr std::array<std::uint8_t, 16> kShowTopWindowEntry{
    0x4C, 0x8B, 0xDC, 0x55, 0x56, 0x41, 0x54, 0x41, 0x57, 0x49, 0x8D, 0x6B, 0x88, 0x48, 0x81, 0xEC};
inline constexpr platform::FunctionFact kShowTopWindow{L"EndUI24_MainMenu_ShowTopWindow", 0x141465F90, kShowTopWindowEntry};
inline constexpr std::uint32_t kUiMainMenuSlot     = 24;    // EndUISystem[24]: the main menu manager (a C++ struct)
inline constexpr std::uint32_t kUiMenuTutorialSlot = 57;    // EndUISystem[57]: the menu tutorial
inline constexpr std::uint32_t kMenuTutorialState  = 0x28;  // u8; 15..19: the game opens no window
inline constexpr std::uint8_t  kMenuTutorialBusyFirst = 15;
inline constexpr std::uint8_t  kMenuTutorialBusyLast  = 19;
inline constexpr std::uint32_t kMainMenuMgrMenu    = 0x00;  // weak UEndMainMenu
inline constexpr std::uint32_t kMainMenuMgrCurrent = 0x10;  // weak: the current window
inline constexpr std::uint32_t kMainMenuMgrOpen    = 0x70;  // i32: 1 while open (0 closed, 2 closing)
inline constexpr std::uint32_t kMainMenuMgrTopCurrent = 0x74;  // u8: the Top window is the current one
inline constexpr std::uintptr_t kMainTopMenuVtable = 0x144BD6FD8;  // UEndMainTopMenu's
inline constexpr std::uint32_t kWindowZOrder       = 0x248;  // UEndUserWidget: i32 ZOrder (1990 for the main menu's)
inline constexpr std::uint32_t kWindowHideSlot     = 0x440;  // UEndMainMenuWindow vtable: Hide (pops its layer, reverses Menu_In)
inline constexpr std::uint32_t kWindowVisible      = 0x410;  // u8
inline constexpr std::uint32_t kWindowTitle        = 0x420;  // FString: the title the manager's tick shows (Top's vt+0x458 is 0)
inline constexpr std::uint32_t kTopGuide           = 0x588;  // FString: Top's guide, shown whatever has focus
inline constexpr std::uint32_t kTopRows            = 0x5A0;  // TArray<u8>: the kinds the build kept, in order
inline constexpr std::uint32_t kObjectGetWorldSlot = 0x140;  // UObject vtable: GetWorld
// The main menu's frame (UEndMainMenu, the weak at mgr+0x00): its title, help and guide text blocks, which the manager's
// tick fills every frame from the current window; a text block keeps the text last shown at +0x118.
inline constexpr std::uint32_t kMainMenuTitleText  = 0x3A0;  // UEndTextBlock*
inline constexpr std::uint32_t kMainMenuHelpText   = 0x3B8;
inline constexpr std::uint32_t kMainMenuGuideText  = 0x3C0;
inline constexpr std::uint32_t kTextBlockText      = 0x118;  // UEndTextBlock: FString Text
// A text block's line height (research/RE-options-help-bar.md): a reflected float with no reflected setter, which the
// block's SynchronizeProperties pushes to its Slate text (the game's own path from property to widget). An empty line
// advances one line height; a line with glyphs at least its glyphs' height.
inline constexpr std::uint32_t kTextBlockLineHeight   = 0x110;  // UEndTextBlock: float LineHeightPercentage
inline constexpr float         kTextBlockLineHeightDefault = 1.0f;  // its class default (the constructor's)
inline constexpr std::uint32_t kWidgetSynchronizeSlot = 0x260;  // UWidget vtable: SynchronizeProperties
inline constexpr std::array<std::uint8_t, 16> kTextBlockSynchronizeEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55};
inline constexpr platform::FunctionFact kTextBlockSynchronize{L"UEndTextBlock_SynchronizeProperties_cand", 0x141378790,
                                                               kTextBlockSynchronizeEntry};
// Lifting the help bar (research/RE-options-help-bar.md §10): its slot is a UEndCanvasPanelSlot whose position is the top
// the text grows down from, moved by the slot's reflected SetPosition (kCanvasSlotSetPosition below). The Slate text
// block's desired height is rebuilt by SetText at once: shadow and margins plus every line's advance.
inline constexpr std::uint32_t kTextBlockDesiredHeight = 0x9A4;  // SEndTextBlock: float
inline constexpr float         kHelpBarOneLineHeight   = 29.0f;  // a stock one-line help: shadow 2 + one line of 27
// (UWidget::SetVisibility and its Visibility byte: kWidgetSetVisibilityFn and kWidgetVisibility above.)
// Read only, for the first-open gap (research/RE-game-widgets.md §11.6.5): a text block's drawing state.
inline constexpr std::uint32_t kTextBlockFont      = 0x1D0;  // UEndTextBlock: UEndFont*
inline constexpr std::uint32_t kTextBlockAlpha     = 0x144;  //   ColorAndOpacity's alpha (float)
inline constexpr std::uint32_t kTextBlockSlate     = 0x210;  //   SEndTextBlock*
inline constexpr std::uint32_t kSlateTextText      = 0x340;  // SEndTextBlock: FString, the text it lays out
inline constexpr std::uint32_t kSlateTextFont      = 0x3A0;  //   UEndFont* (main), +0x3A8 the second
inline constexpr std::uint32_t kSlateTextFontTwo   = 0x3A8;
inline constexpr std::uint32_t kSlateTextBatches   = 0x988;  //   i32: glyph batches laid out (0: nothing to draw)
inline constexpr std::uint32_t kSlateTextSize      = 0x9A8;  //   float: the width it was given
inline constexpr std::uint32_t kSlateTextPending   = 0x9B4;  //   u8, u8: layout pending
inline constexpr std::uint32_t kObjectFlags        = 0x08;   // UObject: EObjectFlags (0x20000000: being destroyed)
inline constexpr std::uint32_t kFontPages          = 0x480;  // UEndFont: its pages object (pages +0x18, count +0x20)
inline constexpr std::uint32_t kFontPagesData      = 0x18;
inline constexpr std::uint32_t kFontPagesCount     = 0x20;
inline constexpr std::uint8_t kVisibilityHidden = 2;  // ESlateVisibility::Hidden
// --- The Co-op row of the title menu (research/RE-title-menu.md; menu/title_row) --------------------------------------
// UEndStartMenu (Blueprints Startmenu_00_C, Startmenu_01_C, one native vtable) keeps its rows' kinds in a TArray<i32> its
// construct fills in code order; text and press go by kind, never by place. Kind 6 ("Download Save Data") is built by
// nothing, always enabled, and its handler has one caller, the press's switch.
inline constexpr std::array<std::uint8_t, 16> kStartMenuConstructEntry{
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41};
inline constexpr platform::SlotFact kStartMenuConstruct{L"vtbl_UEndStartMenu +0x2C0 (construct)", 0x144BDFE30 + 0x2C0,
                                                        {L"UEndStartMenu_NativeConstruct_BuildList", 0x14131C930, kStartMenuConstructEntry}};
inline constexpr std::int32_t   kStartMenuKindOptions = 3;
inline constexpr std::int32_t   kStartMenuKindCoop    = 6;
inline constexpr std::uintptr_t kStartMenuRowTextSlot = 0x144BE0290 + kStartMenuKindCoop * 8;  // g_StartMenu_RowTextIds[6]
inline constexpr std::uintptr_t kStartMenuRowTextWas  = 0x1443D7340;                           // $menu_cross_save_download
/// void(start). The redirect replaces the handler for good, so its first two instructions are never replayed.
inline constexpr std::array<std::uint8_t, 16> kStartMenuKindCoopEntry{
    0x40, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x83, 0xB9, 0xA0, 0x03, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF1};
inline constexpr platform::FunctionFact kStartMenuKindCoopPress{L"UEndStartMenu_CrossSaveDownload_Kind6_cand", 0x141320C20,
                                                                kStartMenuKindCoopEntry};
inline constexpr std::size_t kStartMenuKindCoopFirstBytes = 6;  // push rsi; sub rsp, 30h
/// void(start): the frame, the page in +0x460 (made if none), its close function, its Open, the stick as a d-pad.
inline constexpr std::array<std::uint8_t, 16> kStartMenuOpenOptionsEntry{
    0x40, 0x53, 0x56, 0x57, 0x41, 0x54, 0x48, 0x81, 0xEC, 0xD8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05};
inline constexpr platform::FunctionFact kStartMenuOpenOptions{L"UEndStartMenu_OpenOptionsMenu_cand", 0x141320DE0, kStartMenuOpenOptionsEntry};
/// void(start): the frame made once and added to the overlay, its title written, Menu_In played, the filters taken once.
inline constexpr std::array<std::uint8_t, 16> kStartMenuEnsureFrameEntry{
    0x48, 0x8B, 0xC4, 0x53, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC, 0xB8, 0x00, 0x00, 0x00, 0x33, 0xED};
inline constexpr platform::FunctionFact kStartMenuEnsureFrame{L"UEndStartMenu_EnsureMainMenuFrame_cand", 0x14131B120, kStartMenuEnsureFrameEntry};
/// void(capture {start, weak to start}): what the page's Back runs when Options opened from the title.
inline constexpr std::array<std::uint8_t, 16> kStartMenuOptionsClosedEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x44, 0x8B, 0x41, 0x0C, 0x48, 0x8B, 0xD9, 0x45, 0x85, 0xC0};
inline constexpr platform::FunctionFact kStartMenuOptionsClosed{L"UEndStartMenu_OnOptionsClosed_Lambda_cand", 0x141321320, kStartMenuOptionsClosedEntry};
/// void(TArray<i32>*), with Num already raised: grows the array to hold it.
inline constexpr std::array<std::uint8_t, 16> kIntArrayGrowEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48};
inline constexpr platform::FunctionFact kIntArrayGrow{L"TArrayInt32_ResizeGrow_cand", 0x14063D7E0, kIntArrayGrowEntry};
/// UWidget*(UWidgetTree*, const FName*).
inline constexpr std::array<std::uint8_t, 16> kWidgetTreeFindEntry{
    0x4C, 0x8B, 0xDC, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x49, 0x8D, 0x43, 0x08, 0x49, 0xC7, 0x43, 0x08};
inline constexpr platform::FunctionFact kWidgetTreeFind{L"UWidgetTree__FindWidget", 0x1428FD470, kWidgetTreeFindEntry};
using WidgetTreeFindFn = void* (*)(void* tree, const void* name);  // kWidgetTreeFind's signature
/// void(UEndCanvasPanelSlot*, FVector2D by value).
inline constexpr std::array<std::uint8_t, 16> kCanvasSlotSetSizeEntry{
    0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05, 0x3A, 0xA5, 0x04, 0x04, 0x48, 0x33};
inline constexpr platform::FunctionFact kCanvasSlotSetSize{L"UEndCanvasPanelSlot_SetSize_cand", 0x141356580, kCanvasSlotSetSizeEntry};
/// bool(UObject*): it is a UEndCanvasPanelSlot.
inline constexpr std::array<std::uint8_t, 16> kCastCanvasSlotEntry{
    0x4C, 0x8B, 0xDC, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05, 0xA7, 0xC8, 0xB0};
inline constexpr platform::FunctionFact kCastCanvasSlot{L"Cast_UEndCanvasPanelSlot_cand", 0x140E94250, kCastCanvasSlotEntry};
/// bool(UObject*): it is a UEndVirtualScrollBox, the construct's test of its list.
inline constexpr std::array<std::uint8_t, 16> kCastScrollBoxEntry{
    0x4C, 0x8B, 0xDC, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05, 0xE7, 0xC0, 0xAD};
inline constexpr platform::FunctionFact kCastScrollBox{L"Cast_UEndVirtualScrollBox_cand", 0x140ECC1E0, kCastScrollBoxEntry};
/// void(EndUISystem[2], u8 on): the stick moving the lists, as Options sets and clears it.
inline constexpr std::array<std::uint8_t, 16> kSetStickDPadModeEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x0F, 0xB6, 0xF2, 0x48, 0x8B, 0xD9};
inline constexpr platform::FunctionFact kSetStickDPadMode{L"SquareInput_SetStickDPadMode", 0x14143E270, kSetStickDPadModeEntry};
/// void(UEndTextBlock*, const wchar_t*): keeps no pointer to the text.
inline constexpr std::array<std::uint8_t, 16> kTextBlockSetWideEntry{
    0x40, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05, 0xB6, 0x84};
inline constexpr platform::FunctionFact kTextBlockSetWide{L"UEndTextBlock_SetTextIfChanged_WStr_cand", 0x141378600, kTextBlockSetWideEntry};
inline constexpr std::uintptr_t kNameListMenu       = 0x145853768;  // the static FName List_Menu, set at the game's start
inline constexpr std::uint32_t  kUiInputSlot        = 2;      // EndUISystem[2]: the focus stack and the stick's mode
inline constexpr std::uint32_t  kUserWidgetTree     = 0x198;  // UUserWidget::WidgetTree
inline constexpr std::uint32_t  kStartMenuAction    = 0x3A0;  // i32: 0 idle, else a save, load or online flow of its own
inline constexpr std::uint32_t  kStartMenuKinds     = 0x3D8;  // TArray<i32>: the rows' kinds, in order
inline constexpr std::uint32_t  kStartMenuFrame     = 0x458;  // UEndMainMenu*: the frame around Options, a reflected hold
inline constexpr std::uint32_t  kStartMenuPage      = 0x460;  // UEndNewOptionsMenu*: the game's own Options page
inline constexpr std::uint32_t  kStartMenuListWidth = 0x5A4;  // float: the list slot's width
inline constexpr std::uint32_t  kStartMenuListBase  = 0x5A8;  // float: its height at kStartMenuBaseRows rows
inline constexpr std::int32_t   kStartMenuBaseRows  = 5;
inline constexpr float          kStartMenuRowHeight = 62.0f;
inline constexpr std::int32_t   kTitleOptionsZ      = 1990;   // the overlay's order of the frame and page at the title

// The game's Options screen (UEndNewOptionsMenu, Blueprint MainMenu_OptionsTop2_C, resident): the mod's own instance,
// never the one System keeps (g_OptionsMenu_CachedWeak_cand has System's two xrefs only, §10.4).
inline constexpr const wchar_t* kOptionsClassPath =
    L"/Game/GameContents/Menu/Resident/MainMenu/MainMenu_OptionsTop2.MainMenu_OptionsTop2_C";
/// UEndNewOptionsMenu*(UWorld*, UClass*, u8 inGame): CreateWidgetInstance, an IsA check, +0x3C0 = inGame.
inline constexpr std::array<std::uint8_t, 16> kOptionsCreateEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x41, 0x0F, 0xB6, 0xF8, 0x48, 0x8B};
inline constexpr platform::FunctionFact kOptionsCreate{L"UEndNewOptionsMenu_Create_cand", 0x1412C96C0, kOptionsCreateEntry};
/// void(page, u8 startCategory): Menu_In, state 1, its focus layer pushed with focus on the category list.
inline constexpr std::array<std::uint8_t, 16> kOptionsOpenEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x48};
inline constexpr platform::FunctionFact kOptionsOpen{L"UEndNewOptionsMenu_Open_cand", 0x1412D0F30, kOptionsOpenEntry};
/// bool(UEndUserWidget*, i32 ZOrder): TakeWidget (the first one runs the NativeConstruct, which fills the game's data),
/// then the root overlay's AddWidget.
inline constexpr std::array<std::uint8_t, 16> kAddToRootOverlayEntry{
    0x4C, 0x8B, 0xDC, 0x55, 0x41, 0x54, 0x41, 0x56, 0x49, 0x8D, 0xAB, 0xD8, 0xFA, 0xFF, 0xFF, 0x48};
inline constexpr platform::FunctionFact kAddToRootOverlayFn{L"UEndUserWidget__AddToRootOverlay", kAddToRootOverlay,
                                                            kAddToRootOverlayEntry};
/// bool(UEndUserWidget*): its root-overlay canvas still alive (false after an overlay rebuild, a level change).
inline constexpr std::array<std::uint8_t, 16> kIsInRootOverlayEntry{
    0x48, 0x83, 0xB9, 0x30, 0x02, 0x00, 0x00, 0x00, 0x74, 0x12, 0x48, 0x8B, 0x81, 0x38, 0x02, 0x00};
inline constexpr platform::FunctionFact kIsInRootOverlay{L"UEndUserWidget__IsInRootOverlay_CanvasWeakAlive", 0x14134B060,
                                                         kIsInRootOverlayEntry};
/// UEndUserWidget*(widget): removes it from the root overlay (the game's no-animation close).
inline constexpr std::array<std::uint8_t, 16> kRemoveFromRootOverlayEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x80, 0xB9, 0x6A, 0x02, 0x00, 0x00, 0x00, 0x48, 0x89, 0x6C};
inline constexpr platform::FunctionFact kRemoveFromRootOverlay{L"UEndUserWidget__RemoveFromParent_RootOverlay", 0x14134AE90,
                                                               kRemoveFromRootOverlayEntry};
/// TSharedRef<SWidget>* (UWidget*, TSharedRef<SWidget>* out, 16 bytes) and void(EndUISystem array, TSharedRef* taken):
/// the page's focus layer popped the way its own Back does; the pop releases the reference it is given.
inline constexpr std::array<std::uint8_t, 16> kTakeWidgetEntry{
    0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x08, 0x49, 0x89, 0x6B, 0x18, 0x49, 0x89, 0x73, 0x20, 0x57};
inline constexpr platform::FunctionFact kTakeWidget{L"UWidget__TakeWidget_cand", 0x141348DF0, kTakeWidgetEntry};
inline constexpr std::array<std::uint8_t, 16> kPopFocusLayerEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xD1, 0x48, 0x8B, 0x49, 0x10};
inline constexpr platform::FunctionFact kPopFocusLayer{L"EndUISystem_PopFocusLayer", 0x1413AE250, kPopFocusLayerEntry};
/// The page's key handler (vt+0x340, pass 2: Back, Accept, Square, Circle, L3): Back in state 1 closes the page (state 8,
/// its layer popped). Only this vtable holds it.
inline constexpr std::array<std::uint8_t, 16> kOptionsKeyDownEntry{
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x08, 0xFC, 0xFF};
inline constexpr platform::SlotFact kOptionsKeyDown{L"vtbl_UEndNewOptionsMenu +0x340 (key down, pass 2)", 0x144BDC1D8 + 0x340,
                                                    {L"UEndNewOptionsMenu key down (pass 2)", 0x1412D13A0, kOptionsKeyDownEntry}};
/// i32(page, u8 categoryKey, i32 rowIndex): the row's value (a List row: its value's index). First instruction
/// mov [rsp+18h], rbx.
inline constexpr std::array<std::uint8_t, 16> kOptionsGetValueEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8D};
inline constexpr platform::FunctionFact kOptionsGetValue{L"OptionsMenu_GetOptionValue", 0x1412D6260, kOptionsGetValueEntry};
/// void(page, u8 optionCategory, i32 value): commits a value into a copy of the game's option block; for a category of
/// 0x80 or more the copy goes back unchanged. Its first five bytes are four pushes (rbp, rbx, rsi, rdi), no branch lands
/// inside them, and it runs on the game thread only, where the redirect is placed.
inline constexpr std::array<std::uint8_t, 16> kOptionsSetValueEntry{
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x90};
inline constexpr platform::FunctionFact kOptionsSetValue{L"OptionsMenu_SetOptionValue_CopyApplyCommit", 0x1412D6880,
                                                         kOptionsSetValueEntry};
inline constexpr std::size_t kRedirectFirstBytes = 5;  // the five bytes both value redirects copy
inline constexpr std::uint32_t kOptionsInGame      = 0x3C0;  // u8: Create's inGame (1 from the main menu, 0 from the title)
inline constexpr std::uint32_t kOptionsState       = 0x360;  // u8: 1 categories, 2 rows, 4 value list, 5 editing, 8 closing
inline constexpr std::uint8_t  kOptionsStateClosing = 8;     // then its own tick takes it out of the overlay, state 0
/// The rows' heading: a category's press sets the page's Txt_Title from g_OptionsMenu_CategoryHeadingIds[key] (.rdata,
/// const wchar_t*[8], read only there, copied into the text; keys above 7 get none); Back to the categories clears it.
inline constexpr std::uintptr_t kOptionsHeadings     = 0x144BDBCA0;
inline constexpr std::array<std::uintptr_t, 8> kOptionsHeadingsWere{0x1443CDFC8, 0x1443CDF18, 0x1443CE108, 0x1443CE078,
                                                                   0x1443CDD48, 0x1443CDC98, 0x1443CDE68, 0x1443CDDD8};
inline constexpr std::uint32_t kOptionsCategoryList = 0x368;  // UEndVirtualScrollBox*
inline constexpr std::uint32_t kOptionsCategoryIndex = 0x3A8;  // i32
inline constexpr std::uint32_t kOptionsRowIndex     = 0x3AC;  // i32
inline constexpr std::uint32_t kOptionsOrder        = 0x468;  // TArray<u8>: the category keys the left list shows
inline constexpr std::uint32_t kOptionsLabels       = 0x4B8;  // TArray<FString> _CategoryItems, indexed by key
inline constexpr std::uint32_t kOptionsItems        = 0x4C8;  // TMap<u8, FOptionInfos> _OptionItems
inline constexpr std::uint32_t kOptionInfosHelp     = 0x00;   // FOptionInfos: FString HelpText
inline constexpr std::uint32_t kOptionInfosRows     = 0x10;   //   TArray<FOptionInfo> Infos
inline constexpr std::size_t   kOptionInfoSize      = 0x68;   // FOptionInfo: u8 Kind +0 (0 List), u8 OptionCategory +1,
inline constexpr std::uint32_t kOptionInfoCategory  = 0x01;   //   FString Text +8, FString HelpText +0x18,
inline constexpr std::uint32_t kOptionInfoText      = 0x08;   //   TArray<FString> ValueTexts +0x28, ValueHelps +0x38,
inline constexpr std::uint32_t kOptionInfoHelp      = 0x18;   //   TArray<FName> OpenFlags +0x48, i32 Min +0x58, Max +0x5C,
inline constexpr std::uint32_t kOptionInfoValues    = 0x28;   //   FName OpenFlag +0x60
inline constexpr std::uint32_t kOptionInfoValueHelps = 0x38;  // the value list's help per value (read only below its Num)
inline constexpr std::uint32_t kOptionInfoMin       = 0x58;
inline constexpr std::uint32_t kOptionInfoMax       = 0x5C;
/// EOptionSelectionKind (research/RE-options-value-list.md §2): 0 List (the value pull-down, a plain stacking list that
/// cannot scroll), 1 Range (the inline gauge: Left/Right step 1 in [Min, Max], one cell per step, an integer label).
inline constexpr std::uint8_t kOptionKindList  = 0;
inline constexpr std::uint8_t kOptionKindRange = 1;
// The game's camera rows (research/RE-camera-sensitivity-per-player.md). The Options screen's construct
// (vt+0x2C0, void(page)) fills _OptionItems from the Blueprint's defaults, appends Mouse Sensitivity to Camera & Controls
// and re-commits every row's value; the title's screen, the pause menu's and the mod's own page share this slot.
// Camera & Controls (key 1) lists categories 14..28 in order; Responsiveness (EOptionCategory CameraSensitivity, 20) is
// a Range row 1..5 kept in the data singleton's option block at kDataResponsiveness, read every frame by the
// controller's input step through Options_GetCameraSpeedLevel (the turn is level times the controller's rate).
inline constexpr std::array<std::uint8_t, 16> kOptionsConstructEntry{
    0x4C, 0x8B, 0xDC, 0x55, 0x53, 0x56, 0x41, 0x57, 0x49, 0x8D, 0xAB, 0x68, 0xF3, 0xFF, 0xFF, 0x48};
inline constexpr platform::SlotFact kOptionsConstruct{L"vtbl_UEndNewOptionsMenu +0x2C0 (construct)", 0x144BDC1D8 + 0x2C0,
                                                      {L"UEndNewOptionsMenu_BuildOptionItems_cand", 0x1412CA0D0,
                                                       kOptionsConstructEntry}};
inline constexpr std::uint8_t  kOptionsCameraKey             = 1;
inline constexpr std::uint8_t  kOptionCategoryResponsiveness = 20;
inline constexpr std::uint32_t kDataResponsiveness           = 0xB69B5;  // u8 1..5 (0 before the first construct)
inline constexpr std::uint8_t  kResponsivenessMin            = 1;
inline constexpr std::uint8_t  kResponsivenessMax            = 5;
// Reset All's confirmation: a delegate made by the page's key handler (its vtable is set only there), holding the page at
// +0x08. Execute(self, i32 choice) and ExecuteIfSafe(self, i32 choice) -> bool both run
// OptionsMenu_ResetCategoryToDefaults(page) for choice 0 (Yes). The reset writes the Default row into the option block
// for the page's current category (key kOptionsOrder[kOptionsCategoryIndex]), then for each row reads its value and
// commits it again.
inline constexpr std::array<std::uint8_t, 16> kResetConfirmExecuteEntry{
    0x85, 0xD2, 0x75, 0x09, 0x48, 0x8B, 0x49, 0x08, 0xE9, 0x03, 0x4E, 0xFF, 0xFF, 0xC3, 0xCC, 0xCC};
inline constexpr std::array<std::uint8_t, 16> kResetConfirmExecuteIfSafeEntry{
    0x48, 0x83, 0xEC, 0x28, 0x85, 0xD2, 0x75, 0x09, 0x48, 0x8B, 0x49, 0x08, 0xE8, 0x4F, 0x4D, 0xFF};
inline constexpr std::uintptr_t kResetConfirmVtable = 0x144BDB8A0;
inline constexpr platform::SlotFact kResetConfirmExecute{L"Reset All confirm delegate +0x60 (Execute)", kResetConfirmVtable + 0x60,
                                                         {L"OptionsMenu_ResetConfirm_Execute", 0x1412EAFC0,
                                                          kResetConfirmExecuteEntry}};
inline constexpr platform::SlotFact kResetConfirmExecuteIfSafe{
    L"Reset All confirm delegate +0x68 (ExecuteIfSafe)", kResetConfirmVtable + 0x68,
    {L"OptionsMenu_ResetConfirm_ExecuteIfSafe", 0x1412EB070, kResetConfirmExecuteIfSafeEntry}};
inline constexpr std::uint32_t kResetConfirmPage = 0x08;
// The Range row's gauge (research/RE-options-gauge-scroll.md): the page's edited value, the gauge widget (Blueprint
// MainMenu_Options_Gauge2_C) and its cell strip (a UEndVirtualScrollBox: virtualising, clipped to its bounds, scrolled
// by a float offset in slate units, sized by its DesiredCount when above 0, else by all its cells).
inline constexpr std::uint32_t kOptionsEditValue  = 0x3B0;  // i32: the value a Range row edits (Left/Right, then Accept)
inline constexpr std::uint32_t kOptionsGauge      = 0x388;  // MainMenu_Options_Gauge2_C*
inline constexpr std::uint32_t kOptionsGaugeStrip = 0x398;  // UEndVirtualScrollBox*: the gauge's cells
inline constexpr std::uint32_t kGaugeHead         = 0x380;  // gauge: Img_Gauge_Head (its slot a UEndCanvasPanelSlot)
inline constexpr std::uint32_t kGaugeText         = 0x390;  // gauge: Txt_Setting (UEndTextBlock*)
inline constexpr std::uint32_t kWidgetSlot        = 0x28;   // UWidget::Slot
inline constexpr std::uint32_t kWidgetClipping    = 0xB6;   // UWidget::Clipping (u8; 1 ClipToBounds)
inline constexpr std::uint32_t kCanvasSlotLeft    = 0x38;   // UCanvasPanelSlot: LayoutData.Offsets.Left (float)
inline constexpr std::uint32_t kCanvasSlotTop     = 0x3C;   // UCanvasPanelSlot: LayoutData.Offsets.Top (float)
inline constexpr std::uint32_t kScrollBoxDesired  = 0x75C;  // UEndVirtualScrollBox: i32 DesiredCount
inline constexpr std::uint32_t kScrollBoxBar      = 0x721;  //   u8 ScrollBarVisibility; +0x72C AlwaysShowScrollbar
inline constexpr std::uint32_t kScrollBoxAlwaysBar = 0x72C;
inline constexpr std::uint32_t kScrollBoxSlate    = 0x7D8;  //   TSharedPtr<SEndVirtualScrollBox>
inline constexpr std::uint32_t kSlateScrollSize   = 0x5B4;  // SEndVirtualScrollBox: the size it was last given (2 floats)
inline constexpr float         kGaugePitch        = 28.0f;  // units per cell, as the game places the marker
inline constexpr float         kGaugeHeadX        = 80.5f;  // the marker's x at the first cell
/// OptionsMenu_UpdateGaugeMarker(page, gauge, const FOptionInfo*): the strip's count (Max - Min), the marker at
/// (v - Min) * 28 + 80.5, Txt_Setting = "%d". Two calls, both `E8` inside one block: the press (every kind) and each
/// Left/Right step. Its entry is `test rdx, rdx; jz rel32`, so it is redirected at its calls, not at its entry.
inline constexpr std::array<std::uint8_t, 16> kGaugeMarkerEntry{
    0x48, 0x85, 0xD2, 0x0F, 0x84, 0x14, 0x03, 0x00, 0x00, 0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41};
inline constexpr platform::FunctionFact kGaugeMarker{L"OptionsMenu_UpdateGaugeMarker_cand", 0x1412E0450, kGaugeMarkerEntry};
inline constexpr std::uintptr_t kGaugeMarkerOpenCall = 0x1412D9E5B;  // in the press body
inline constexpr std::uintptr_t kGaugeMarkerStepCall = 0x1412E5411;  // in UEndNewOptionsMenu_StepEditValue_cand
/// FString::FromInt(FString* out, i32): a Range row's value text, in OnOptionListSetupItem right after the row's value
/// read (`E8` inside one block; its out is then the row's Txt_AbilityName through UEndTextBlock::SetText).
inline constexpr std::array<std::uint8_t, 16> kFromIntEntry{
    0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x18, 0x55, 0x56, 0x41, 0x56, 0x48, 0x81, 0xEC, 0xB0, 0x00};
inline constexpr platform::FunctionFact kFromInt{L"FString_FromInt", 0x141C0A150, kFromIntEntry};
inline constexpr std::uintptr_t kRowValueTextCall = 0x1412D9788;
/// The page's action handler (vt+0x338, pass 1; research/RE-options-gauge-repeat.md): in state 5 (a Range row edited) a
/// d-pad Left/Right steps the value, but only when the event is not a held button's repeat (`cmp [r9+10h], r13b` at
/// 0x1412D5038), so no gauge of the game's steps on hold. Only this vtable holds it.
inline constexpr std::array<std::uint8_t, 16> kOptionsActionDownEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55};
inline constexpr platform::SlotFact kOptionsActionDown{
    L"vtbl_UEndNewOptionsMenu +0x338 (action down, pass 1)", 0x144BDC1D8 + 0x338,
    {L"UEndNewOptionsMenu action down (pass 1)", 0x1412D4FB0, kOptionsActionDownEntry}};
inline constexpr std::uint8_t   kOptionsStateRangeEdit = 5;      // kOptionsState: a Range row's gauge open
inline constexpr std::uint32_t  kActionEventRepeat     = 0x10;   // SquareInput's action event: u8 1 for a held button's repeat
inline constexpr std::uint32_t  kActionEventKey        = 0x20;   // its key's FName (8 bytes); the d-pad keys: kKeyDPadLeft, kKeyDPadRight
/// The reflected functions the slider drives (ProcessEvent; the same calls the game's shop list makes).
inline constexpr const wchar_t* kScrollBoxSetDesiredCount = L"/Script/EndGame.EndVirtualScrollBox:SetDesiredCount";
inline constexpr const wchar_t* kScrollBoxSetScrollOffset = L"/Script/EndGame.EndVirtualScrollBox:SetScrollOffset";
inline constexpr const wchar_t* kScrollBoxGetScrollOffset = L"/Script/EndGame.EndVirtualScrollBox:GetScrollOffset";
inline constexpr const wchar_t* kScrollBoxGetMaxOffset    = L"/Script/EndGame.EndVirtualScrollBox:GetMaxScrollOffset";
inline constexpr const wchar_t* kCanvasSlotSetPosition    = L"/Script/EndGame.EndCanvasPanelSlot:SetPosition";
inline constexpr const wchar_t* kTextBlockSetText         = L"/Script/EndGame.EndTextBlock:SetText";
inline constexpr const wchar_t* kCanvasSlotClassPath      = L"/Script/EndGame.EndCanvasPanelSlot";
/// The game's own helpers the page's data is written with (its allocator, GMalloc): an FString copy (dst, const
/// FString* src: reads the source's Data and Num, so a view over the mod's string is a valid source), FindOrAdd on
/// _OptionItems (map, const u8* key) -> FOptionInfos*, an FOptionInfo's default constructor, destructor and move
/// constructor (dst, src), the two arrays' growth after Num++ (array: reads Num, sets Max, reallocates), RemoveAt
/// (array, index, count; count > 0), and the scroll box's count and refresh.
inline constexpr std::array<std::uint8_t, 16> kFStringAssignEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x3B, 0xCA, 0x74, 0x59, 0x44, 0x8B};
inline constexpr platform::FunctionFact kFStringAssign{L"FString__AssignCopy_cand", 0x14063E480, kFStringAssignEntry};
inline constexpr std::array<std::uint8_t, 16> kOptionItemsFindOrAddEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0x54, 0x24, 0x38, 0x4C, 0x8D, 0x44, 0x24, 0x38};
inline constexpr platform::FunctionFact kOptionItemsFindOrAdd{L"OptionItemsMap_FindOrAdd_cand", 0x1412E81F0,
                                                              kOptionItemsFindOrAddEntry};
/// The same map's find, which never adds: i32*(map, i32* outId, u8 key), outId -1 when the key is absent; the element
/// (TSetElement of TPair<u8, FOptionInfos>, 0x30 bytes) at the map's data + id * 0x30, its FOptionInfos at +0x08.
inline constexpr std::array<std::uint8_t, 16> kOptionItemsFindIdEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x8B, 0x41, 0x08, 0x48, 0x8B, 0xDA, 0x3B, 0x41, 0x34, 0x74, 0x4A};
inline constexpr platform::FunctionFact kOptionItemsFindId{L"OptionItemsMap_FindId_cand", 0x1412E7DD0, kOptionItemsFindIdEntry};
inline constexpr std::size_t   kOptionItemsElementSize  = 0x30;
inline constexpr std::uint32_t kOptionItemsElementValue = 0x08;
inline constexpr std::uint32_t kOptionItemsElementCount = 0x08;  // i32 at the map: its elements' count (ids below it)
inline constexpr std::array<std::uint8_t, 16> kOptionInfoCtorEntry{
    0x33, 0xC0, 0x48, 0x89, 0x41, 0x08, 0x48, 0x89, 0x41, 0x10, 0x48, 0x89, 0x41, 0x18, 0x48, 0x89};
inline constexpr platform::FunctionFact kOptionInfoCtor{L"FOptionInfo_DefaultCtor_cand", 0x1412D0E90, kOptionInfoCtorEntry};
inline constexpr std::array<std::uint8_t, 16> kOptionInfoDtorEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x49};
inline constexpr platform::FunctionFact kOptionInfoDtor{L"FOptionInfo_Dtor_cand", 0x1412D0ED0, kOptionInfoDtorEntry};
inline constexpr std::array<std::uint8_t, 16> kOptionInfoMoveEntry{
    0x0F, 0xB6, 0x02, 0x45, 0x33, 0xC0, 0x88, 0x01, 0x0F, 0xB6, 0x42, 0x01, 0x88, 0x41, 0x01, 0x4C};
inline constexpr platform::FunctionFact kOptionInfoMove{L"FOptionInfo_MoveCtor_cand", 0x1412E89C0, kOptionInfoMoveEntry};
inline constexpr std::array<std::uint8_t, 16> kArrayGrowEntry{  // both growths open alike
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x83};
inline constexpr platform::FunctionFact kOptionInfosGrow{L"TArrayFOptionInfo_ResizeGrow_cand", 0x140943D00, kArrayGrowEntry};
inline constexpr platform::FunctionFact kStringArrayGrow{L"TArrayFString_ResizeGrow_cand", 0x14063EF30, kArrayGrowEntry};
inline constexpr std::array<std::uint8_t, 16> kOptionInfosRemoveAtEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
inline constexpr platform::FunctionFact kOptionInfosRemoveAt{L"TArrayFOptionInfo_RemoveAt_cand", 0x1412E7D00,
                                                             kOptionInfosRemoveAtEntry};
inline constexpr std::array<std::uint8_t, 16> kScrollBoxSetCountEntry{
    0x89, 0x91, 0x58, 0x07, 0x00, 0x00, 0x48, 0x8B, 0x89, 0xD8, 0x07, 0x00, 0x00, 0x48, 0x85, 0xC9};
inline constexpr platform::FunctionFact kScrollBoxSetCount{L"EndVSB_SetItemCount_cand", 0x14137D490, kScrollBoxSetCountEntry};
inline constexpr std::array<std::uint8_t, 16> kScrollBoxRefreshEntry{
    0x48, 0x8B, 0x89, 0xD8, 0x07, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74, 0x0C, 0x48, 0x8B, 0x89, 0xF8};
inline constexpr platform::FunctionFact kScrollBoxRefresh{L"EndVSB_RefreshItems_cand", 0x14137D8F0, kScrollBoxRefreshEntry};

// ---------------------------------------------------------------------------------------------------------------------
// Player windows: the game's message window (Com_Window_01_C, UEndMessageWindow) in each player's own view
// (hud/view_window; research/RE-game-widgets.md §12). The game's own sequence (EndUI_ShowMessageWindow 0x1413B12A0) with
// its focus layer popped at once: create (world) -> the window or null; AddToRootOverlay (kAddToRootOverlayFn) at the
// ZOrder also written at kEndWidgetZOrder; set the text (window, const FString*: text ID or literal markup); the options
// (window, const TArray<FString>*, leftAligned 1: none collapses the cells); open (window, 0: In, the open sound, a focus
// layer); UEndUserWidget::PopFromFocusStack (that layer, wherever it sits; a no-op once popped); the game's window list
// add (EUI[39], int* id, UObject** window: its reset on a load or a map change closes the window too); close (Out, the
// close sound; its own pop finds nothing). With kMessageWindowRemoveAfterOut set, the window's tick takes it out of the
// overlay when Out ends and sets the state to 0.
inline constexpr std::array<std::uint8_t, 16> kMessageWindowCreateEntry{
    0x48, 0x8B, 0xC4, 0x53, 0x57, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x1D, 0xE5};
inline constexpr platform::FunctionFact kMessageWindowCreate{L"EndMessageWindow_Create", 0x1412BE000, kMessageWindowCreateEntry};
inline constexpr std::array<std::uint8_t, 16> kMessageWindowSetTextEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48, 0x81, 0xEC};
inline constexpr platform::FunctionFact kMessageWindowSetText{L"EndMessageWindow_SetMessageText", 0x1412BE340,
                                                              kMessageWindowSetTextEntry};
inline constexpr std::array<std::uint8_t, 16> kMessageWindowSetOptionsEntry{
    0x44, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x57, 0x41, 0x54, 0x41};
inline constexpr platform::FunctionFact kMessageWindowSetOptions{L"EndMessageWindow_SetOptions", 0x1412BE4A0,
                                                                 kMessageWindowSetOptionsEntry};
inline constexpr std::array<std::uint8_t, 16> kMessageWindowOpenEntry{
    0x4C, 0x8B, 0xDC, 0x55, 0x56, 0x57, 0x49, 0x8D, 0x6B, 0x98, 0x48, 0x81, 0xEC, 0x50, 0x01, 0x00};
inline constexpr platform::FunctionFact kMessageWindowOpen{L"EndMessageWindow_Open_PushFocusLayer", 0x1412BE9E0,
                                                           kMessageWindowOpenEntry};
inline constexpr std::array<std::uint8_t, 16> kMessageWindowCloseEntry{
    0x40, 0x57, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x8B, 0x81, 0x60, 0x03, 0x00, 0x00, 0x48};
inline constexpr platform::FunctionFact kMessageWindowClose{L"EndMessageWindow_Close", 0x1412BF090, kMessageWindowCloseEntry};
inline constexpr std::array<std::uint8_t, 16> kPopFromFocusStackEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8D, 0x54, 0x24, 0x20, 0x48};
inline constexpr platform::FunctionFact kPopFromFocusStack{L"UEndUserWidget__PopFromFocusStack", 0x14134BF50,
                                                           kPopFromFocusStackEntry};
inline constexpr std::array<std::uint8_t, 16> kMessageWindowListAddEntry{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41};
inline constexpr platform::FunctionFact kMessageWindowListAdd{L"EndUI39_MessageWindowList_Add", 0x141485F10,
                                                              kMessageWindowListAddEntry};
inline constexpr std::uint32_t kMessageWindowListSlot      = 39;     // EndUISystem[39]: the game's message windows
inline constexpr std::uint32_t kMessageWindowState         = 0x360;  // int32: 0 none, 1 opening, 2 open, 3 closing
inline constexpr std::uint32_t kMessageWindowRemoveAfterOut = 0x369; // u8: the tick removes it when Out ends
inline constexpr std::int32_t  kPlayerWindowZOrder         = 1000;   // above the HUD (0) and the skip icon (991), below
                                                                     // subtitles (1240) and the pause menu (1990)
/// The game's UI sounds (§12.6): play (const args*, 0x34 bytes: +0 the UI sound source, +8 the cue's FName, +0x10 -1,
/// +0x14 four floats {0, 1, 1, 0}, +0x28 0, +0x30 1.0f) and the source (the object named "UI"; null if none). The cues'
/// FNames, as the game's menus hold them: cursor move, decide, cancel.
inline constexpr std::array<std::uint8_t, 16> kPlayUISoundEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x0D, 0x30, 0xB9, 0x13, 0x04};
inline constexpr platform::FunctionFact kPlayUISound{L"EndAudio_PlayUISound_cand", 0x1416AE170, kPlayUISoundEntry};
/// Its arguments, 0x34 bytes as the message window builds them: the source, the cue's FName, an index (-1), a float4
/// mix {0, 1, 1, 0} and a float the window sets to 1 [UNKNOWN: its meaning].
inline constexpr std::uint32_t kUISoundArgsBytes  = 0x34;
inline constexpr std::uint32_t kUISoundArgsSource = 0x00;
inline constexpr std::uint32_t kUISoundArgsCue    = 0x08;
inline constexpr std::uint32_t kUISoundArgsIndex  = 0x10;
inline constexpr std::uint32_t kUISoundArgsMix    = 0x14;
inline constexpr std::uint32_t kUISoundArgsLast   = 0x30;
inline constexpr std::array<std::uint8_t, 16> kUISoundSourceEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00, 0xBA};
inline constexpr platform::FunctionFact kUISoundSource{L"EndUI_GetUISoundSource_cand", 0x1413B64B0, kUISoundSourceEntry};
inline constexpr std::uintptr_t kSoundCueCursor = 0x145852C80;  // FName UI000_Cursor
inline constexpr std::uintptr_t kSoundCueDecide = 0x145852A90;  // FName UI002_OK
inline constexpr std::uintptr_t kSoundCueCancel = 0x145852A98;  // FName UI003_Cancel
/// The focus context's layers (SquareInput owner, EndUISystem[2]): TArray of 40-byte layers, count at +0x48. Read only,
/// for the log line that shows a window's open left them as they were.
inline constexpr std::uint32_t kFocusContextSlot = 2;
inline constexpr std::uint32_t kFocusLayerCount  = 0x48;


// --- The battle's rates: enemy HP and stagger, enemy damage, EXP and AP (research/RE-enemy-stat-scaling.md,
// RE-streamlined-progression.md; reviewed against the binary) ----------------------------------------
/// EnemyParam_ComputeStats_FromSpecAndLevel(estimateRow, paramRow, int* hp, int* bp, int* pa, int* ma, int* pd, int* md):
/// the HP and stagger gauge in percent of the EnemyParameter row in, the values out, then the Easy/Hard rate. Called by
/// an enemy's battle start, the spec override (a phase row) and a guest ally's construction; the first two are
/// redirected (each `E8` inside one block; the result in rax is never read).
inline constexpr std::array<std::uint8_t, 16> kEnemyComputeStatsEntry{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x4C, 0x89, 0x48, 0x20, 0x55, 0x56, 0x57, 0x41, 0x55};
inline constexpr platform::FunctionFact kEnemyComputeStats{L"EnemyParam_ComputeStats_FromSpecAndLevel", 0x140A01020,
                                                           kEnemyComputeStatsEntry};
inline constexpr std::uintptr_t kEnemyComputeStatsStartCall    = 0x140893594;  // in the enemy component's battle start
inline constexpr std::uintptr_t kEnemyComputeStatsOverrideCall = 0x14096D0A2;  // in the spec override
/// BattleUnit_SetOverrideBattleCharaSpec_KeepRatios(component, FName row): the game's own phase change (AI exec,
/// four callers, one a tail jump). Stores the row's name and the row, then, in battle, recomputes the stats and sets
/// max HP and the stagger gauge keeping their shares. Its first instruction is a home-area store (5 bytes, one block).
inline constexpr std::array<std::uint8_t, 16> kSpecOverrideEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56};
inline constexpr platform::FunctionFact kSpecOverride{L"BattleUnit_SetOverrideBattleCharaSpec_KeepRatios", 0x14096CBC0,
                                                      kSpecOverrideEntry};
inline constexpr std::size_t   kSpecOverrideFirstBytes = 5;
inline constexpr std::uint32_t kBattleCompBaseSpec     = 0x5B8;  // the component's own BattleCharaSpec row
inline constexpr std::uint32_t kCharaSpecKind          = 0x130;  // u8 CharacterKindID: 1 the fight's leader (a boss)
inline constexpr std::uint8_t  kCharaSpecKindLeader    = 1;
/// BattleDamage_ClampAndDebugInvincible(int* damage, attackerActor, victimActor, damageSourceRow, u8 heal), after the
/// game's per-hit difficulty step, at the end of both hit functions; the first call crosses a block.
inline constexpr std::array<std::uint8_t, 16> kDamageClampEntry{
    0x40, 0x53, 0x56, 0x41, 0x54, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x45, 0x0F, 0xB6, 0x61, 0x08};
inline constexpr platform::FunctionFact kDamageClamp{L"BattleDamage_ClampAndDebugInvincible", 0x1409E7E20, kDamageClampEntry};
inline constexpr std::uintptr_t kDamageClampCall  = 0x1409EC02C;  // in BattleDamage_ApplyHitToVictim (crosses)
inline constexpr std::uintptr_t kDamageClampCallB = 0x1409F2011;  // in BattleDamage_ApplyHitToVictim_B
inline constexpr std::uint32_t  kDamageSourceType      = 0x08;  // u8 DamageType (FEndDataTableBattleDamageSource)
inline constexpr std::uint32_t  kDamageSourcePowerType = 0x14;  // u8 PowerType
/// Difficulty_GetEffective(): 0 Easy, 1 Normal, 2 Hard (the save's Hard flag first). Reads the save data.
inline constexpr std::array<std::uint8_t, 16> kDifficultyEntry{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xE8, 0x25, 0xBA, 0xE3, 0xFF, 0x48, 0x8B, 0xD8, 0xE8, 0x1D};
inline constexpr platform::FunctionFact kDifficulty{L"Difficulty_GetEffective", 0x140B11D80, kDifficultyEntry};
inline constexpr std::int32_t kDifficultyNormal = 1;
/// BattleResult_AwardExpApGil(result): the battle's EXP and AP times the bonus rows when the story is cleared or the
/// save's 2x EXP & 3x AP byte is on, then each member's share. One caller (game thread), `E8` inside one block.
inline constexpr std::array<std::uint8_t, 16> kAwardEntry{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55};
inline constexpr platform::FunctionFact kAward{L"BattleResult_AwardExpApGil", 0x1409D9590, kAwardEntry};
inline constexpr std::uintptr_t kAwardCall = 0x1409B79E2;  // in BattleSystem_Tick_CallsTargetingTwice
/// Story_IsFF7RemakeCleared(): the bonus gate's first test; its call in the award lies inside one block.
inline constexpr std::array<std::uint8_t, 16> kStoryClearedEntry{
    0x48, 0x83, 0xEC, 0x38, 0xC7, 0x44, 0x24, 0x28, 0x17, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x0D, 0x2D};
inline constexpr platform::FunctionFact kStoryCleared{L"Story_IsFF7RemakeCleared", 0x140B06CF0, kStoryClearedEntry};
inline constexpr std::uintptr_t kAwardStoryClearedCall = 0x1409D9890;
inline constexpr std::uint32_t  kSaveDataBoosterExp    = 0x399;  // u8 in the active save data: 2x EXP & 3x AP
/// The ResidentParameter tables (three slots: area, resident, debug; DataTableSet3_FindRowByName over them, the first
/// hit wins, every read) and the rows' FName globals. A row: ParamInt +0x08, ParamFloat +0x0C.
inline constexpr std::uintptr_t kResidentParameterTables = 0x145843650;
inline constexpr std::uintptr_t kNameExpRateGameClear    = 0x145836E18;  // ParamInt 200
inline constexpr std::uintptr_t kNameApRateGameClear     = 0x145836DF8;  // ParamInt 300
inline constexpr std::uintptr_t kNameEasyTakeDamage      = 0x1458370E8;  // ParamFloat 0.4
inline constexpr std::uintptr_t kNameHardTakeDamage      = 0x1458370F0;  // ParamFloat 1.4
inline constexpr std::uint32_t  kParamRowInt   = 0x08;
inline constexpr std::uint32_t  kParamRowFloat = 0x0C;
inline constexpr std::size_t    kTableSlots    = 3;
inline constexpr std::size_t    kTableSlotResident = 1;

// --- The save-data notice, the title screen's one notice (RE-title-notice.md) ----------------------------------------
/// EndUI31_ShowSaveIcon_Static(UWorld*, u8 isSave): 0 shows the "Checking saved data" look (no timer, no text swap; a
/// hide ends it at once), anything else "Saving"; a show while shown changes nothing on the widget.
inline constexpr std::array<std::uint8_t, 16> kShowSaveIconEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x0F};
inline constexpr platform::FunctionFact kShowSaveIcon{L"EndUI31_ShowSaveIcon_Static", 0x141396DE0, kShowSaveIconEntry};
inline constexpr std::uint32_t kSaveIconSlot      = 31;     // EndUISystem[31]: the save icon's manager
inline constexpr std::uint32_t kSaveIconWidget    = 0x00;   // the manager: the SaveIcon_C widget (an FWeakObjectPtr)
inline constexpr std::uint32_t kSaveIconErrorOnce = 0x09;   //   set once the network notice was shown: a hide shows it again
inline constexpr std::uint32_t kSaveIconShown     = 0x10;   //   the notice is up
inline constexpr std::uint32_t kSaveIconText      = 0x3B0;  // SaveIcon_C.Txt_Info, the widget's one text block
/// The save system (the data singleton): the running job's type (u8, 0 for none) and the queued jobs' count (i32). The
/// title's press queues a job that shows the notice and hides it at its end.
inline constexpr std::uint32_t kSaveSystemRunningJob = 0x1F0;
inline constexpr std::uint32_t kSaveSystemQueuedJobs = 0x210;
/// The game's own hide with no next notice (UEndMenuAPI::BPHideDataAccessIcon, no parameters, called on the class's
/// default object).
inline constexpr const wchar_t* kEndMenuApiClassPath    = L"/Script/EndGame.EndMenuAPI";
inline constexpr const wchar_t* kHideDataAccessIconPath = L"/Script/EndGame.EndMenuAPI:BPHideDataAccessIcon";

// --- The engine's own frame times: the numbers `stat unit` shows (research/RE-frame-stats.md) ------------------------
/// Stock UE 4.18 globals, each a u32 count of QueryPerformanceCounter ticks for the last frame: the game thread's work
/// (GGameThreadTime, written by FViewport::Draw on the game thread), the render thread's work less its waits and its
/// present (GRenderThreadTime, the Slate window draw on the render thread), the render thread's present (the swap
/// buffer time, the same draw) and the GPU's frame (GGPUFrameTime, D3D12's gap recorder at the end of the RHI frame,
/// which keeps its last value while the recorder is off). render/frame_watch checks each through an instruction that
/// refers to it RIP-relatively: its writer's store, the GPU's through RHIGetGPUFrameCycles' load. [CONFIRMED code]
inline constexpr std::uintptr_t kGameThreadTime   = 0x14582C4CC;
inline constexpr std::uintptr_t kRenderThreadTime = 0x14582C4D0;
inline constexpr std::uintptr_t kSwapBufferTime   = 0x14582C4BC;
inline constexpr std::uintptr_t kGpuFrameTime     = 0x145831140;
/// GRenderingThread and GRHIThread_InternalUseOnly (FRunnableThread*; the second null without a dedicated RHI thread),
/// each thread's id at +0x44; set when the render thread starts, set again at a restart. [CONFIRMED code]
inline constexpr std::uintptr_t kRenderingThread  = 0x1457FACF8;
inline constexpr std::uintptr_t kRhiThread        = 0x1457FA4E0;
inline constexpr std::uint32_t  kRunnableThreadId = 0x44;

}  // namespace lss::facts
