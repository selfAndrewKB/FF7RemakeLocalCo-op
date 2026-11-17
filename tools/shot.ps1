param(
    [string]$Out = "$env:TEMP\rmk-shot.png",
    [int]$Width = 1280,
    [string]$Title = "FINAL FANTASY VII REMAKE",   # a window whose title starts with this; "" captures the primary monitor
    [string]$Display = "",                         # or a whole monitor, e.g. \\.\DISPLAY1
    [switch]$Focus,                                # bring the game to the front and copy the screen instead (old way)
    [int]$SettleMs = 400
)
# Screenshot of the game window (by title), a monitor, or the primary monitor, downscaled to $Width.
# The game window's own content is read through PrintWindow(PW_RENDERFULLCONTENT): the composed window as DWM
# holds it, whatever lies over it and whoever has the focus, so the capture is the game and nothing else, and the
# player's foreground window is never taken. -Focus brings the game to the front and copies the screen instead, for
# a window PrintWindow cannot read. A capture that comes back black is refused with a message.
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class RmkWindow {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    // Windows lets a process take the foreground right after it has sent input; an Alt tap is the least
    // disruptive input to send (as keys.ps1 does).
    public static bool FocusWindow(IntPtr h) {
        keybd_event(0x12, 0, 0, UIntPtr.Zero);
        keybd_event(0x12, 0, 2, UIntPtr.Zero);
        return SetForegroundWindow(h);
    }
}
"@
function Save([System.Drawing.Bitmap]$full, [string]$where) {
    $hh = [int]($full.Height * $Width / $full.Width)
    $small = New-Object System.Drawing.Bitmap $full, $Width, $hh
    $small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $small.Dispose()
    "$Out ($($full.Width)x$($full.Height) $where -> ${Width}x$hh)"
}
if ($Display -or -not $Title) {
    $bounds = if ($Display) { ([System.Windows.Forms.Screen]::AllScreens | Where-Object DeviceName -eq $Display).Bounds }
              else { [System.Windows.Forms.Screen]::PrimaryScreen.Bounds }
    $full = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
    $g = [System.Drawing.Graphics]::FromImage($full)
    $g.CopyFromScreen($bounds.X, $bounds.Y, 0, 0, $bounds.Size)
    Save $full "at $($bounds.X),$($bounds.Y)"
    $g.Dispose(); $full.Dispose()
    return
}
$game = Get-Process | Where-Object { $_.MainWindowHandle -ne 0 -and $_.MainWindowTitle.StartsWith($Title) } | Select-Object -First 1
if (-not $game) { throw "no window titled '$Title'" }
$h = $game.MainWindowHandle
$rect = New-Object RmkWindow+RECT; $origin = New-Object RmkWindow+POINT
[void][RmkWindow]::GetClientRect($h, [ref]$rect)
[void][RmkWindow]::ClientToScreen($h, [ref]$origin)
if ($Focus) {
    if ([RmkWindow]::IsIconic($h)) { [void][RmkWindow]::ShowWindow($h, 9) }
    if ([RmkWindow]::GetForegroundWindow() -ne $h) {
        [void][RmkWindow]::FocusWindow($h)
        Start-Sleep -Milliseconds $SettleMs
        if ([RmkWindow]::GetForegroundWindow() -ne $h) { throw "the game window could not be brought to the front; nothing captured" }
    }
    $full = New-Object System.Drawing.Bitmap $rect.Right, $rect.Bottom
    $g = [System.Drawing.Graphics]::FromImage($full)
    $g.CopyFromScreen($origin.X, $origin.Y, 0, 0, $full.Size)
    Save $full "at $($origin.X),$($origin.Y) (screen copy)"
    $g.Dispose(); $full.Dispose()
    return
}
# PrintWindow draws the whole window (frame included) at the window rect's size; the client area is cut out of it.
$win = New-Object RmkWindow+RECT
[void][RmkWindow]::GetWindowRect($h, [ref]$win)
$whole = New-Object System.Drawing.Bitmap ($win.Right - $win.Left), ($win.Bottom - $win.Top)
$g = [System.Drawing.Graphics]::FromImage($whole)
$hdc = $g.GetHdc()
$ok = [RmkWindow]::PrintWindow($h, $hdc, 2)   # PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc); $g.Dispose()
if (-not $ok) { $whole.Dispose(); throw "PrintWindow failed; try -Focus" }
$cut = New-Object System.Drawing.Rectangle ($origin.X - $win.Left), ($origin.Y - $win.Top), $rect.Right, $rect.Bottom
$full = $whole.Clone($cut, $whole.PixelFormat)
$whole.Dispose()
# a black capture means the window's content was not composed (minimised, or a renderer PrintWindow cannot read)
$dark = 0
foreach ($x in 0..15) { $p = $full.GetPixel([int]($x * ($full.Width - 1) / 15), [int]($full.Height / 2)); if ($p.R + $p.G + $p.B -lt 12) { $dark++ } }
if ($dark -eq 16) { $full.Dispose(); throw "the capture is black (the window's content is not composed); try -Focus" }
Save $full "at $($origin.X),$($origin.Y) (window content)"
$full.Dispose()
