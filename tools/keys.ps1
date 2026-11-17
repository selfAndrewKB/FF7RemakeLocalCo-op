param(
    [Parameter(Mandatory)] [string[]]$Keys,   # e.g. Enter, Escape, Up, W, F1, Ctrl+J, Shift+Tab
    [int]$HoldMs = 120,
    [int]$GapMs = 600,
    [string]$Title = "FINAL FANTASY VII REMAKE"  # a window whose title starts with this
)
# Brings the game window to the front and presses keys by scan code (games reading raw input ignore
# virtual-key-only events). Refuses if the game window cannot be made the foreground window.
# Any letter, digit and F-key, with Ctrl/Shift/Alt.
$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class RmkKeys {
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)] struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; [FieldOffset(8)] public long pad0; [FieldOffset(16)] public long pad1; [FieldOffset(24)] public long pad2; [FieldOffset(32)] public long pad3; }
    [DllImport("user32.dll", SetLastError=true)] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int cmd);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    // Windows lets a process take the foreground right after it has sent input; an Alt tap is the
    // least disruptive input to send.
    public static bool Focus(IntPtr hwnd) {
        keybd_event(0x12, 0, 0, UIntPtr.Zero);
        keybd_event(0x12, 0, 2, UIntPtr.Zero);
        return SetForegroundWindow(hwnd);
    }
    public static uint Key(ushort scan, bool extended, bool up) {
        var input = new INPUT[1];
        input[0].type = 1;
        input[0].ki.wScan = scan;
        input[0].ki.dwFlags = 0x0008u | (extended ? 0x0001u : 0u) | (up ? 0x0002u : 0u);
        return SendInput(1, input, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@
$scan = @{ Enter = @(0x1C, $false); Escape = @(0x01, $false); Space = @(0x39, $false); Tab = @(0x0F, $false)
           Backspace = @(0x0E, $false); Tilde = @(0x29, $false)
           Up = @(0x48, $true); Down = @(0x50, $true); Left = @(0x4B, $true); Right = @(0x4D, $true) }
$letters = @{ Q=0x10; W=0x11; E=0x12; R=0x13; T=0x14; Y=0x15; U=0x16; I=0x17; O=0x18; P=0x19
              A=0x1E; S=0x1F; D=0x20; F=0x21; G=0x22; H=0x23; J=0x24; K=0x25; L=0x26
              Z=0x2C; X=0x2D; C=0x2E; V=0x2F; B=0x30; N=0x31; M=0x32 }
foreach ($l in $letters.Keys) { $scan[$l] = @($letters[$l], $false) }
$digits = @(0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A)
for ($d = 0; $d -le 9; $d++) { $scan["D$d"] = @($digits[$d], $false) }
$fkeys = @(0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58)
for ($f = 1; $f -le 12; $f++) { $scan["F$f"] = @($fkeys[$f - 1], $false) }
$mods = @{ Ctrl = @(0x1D, $false); Shift = @(0x2A, $false); Alt = @(0x38, $false) }

$game = Get-Process | Where-Object { $_.MainWindowHandle -ne 0 -and $_.MainWindowTitle.StartsWith($Title) } | Select-Object -First 1
if (-not $game) { throw "no window titled '$Title...'" }
$hwnd = $game.MainWindowHandle
[void][RmkKeys]::ShowWindow($hwnd, 9)
[void][RmkKeys]::Focus($hwnd)
Start-Sleep -Milliseconds 300
if ([RmkKeys]::GetForegroundWindow() -ne $hwnd) { throw "the game window is not in the foreground; no key sent" }
foreach ($k in $Keys) {
    $parts = $k.Split('+')
    $name = $parts[-1]
    $held = @($parts | Select-Object -SkipLast 1)
    if (-not $scan.ContainsKey($name)) { throw "unknown key $k" }
    foreach ($m in $held) { if (-not $mods.ContainsKey($m)) { throw "unknown modifier $m in $k" } }
    foreach ($m in $held) { [void][RmkKeys]::Key($mods[$m][0], $mods[$m][1], $false); Start-Sleep -Milliseconds 50 }
    $code, $ext = $scan[$name]
    $sent = [RmkKeys]::Key($code, $ext, $false)
    Start-Sleep -Milliseconds $HoldMs
    [void][RmkKeys]::Key($code, $ext, $true)
    foreach ($m in $held) { Start-Sleep -Milliseconds 50; [void][RmkKeys]::Key($mods[$m][0], $mods[$m][1], $true) }
    if ($sent -ne 1) { throw "SendInput failed for $k" }
    "pressed $k"
    Start-Sleep -Milliseconds $GapMs
}
