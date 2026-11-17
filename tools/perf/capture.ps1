# ETW capture of FF7 Remake for CPU and render profiling: CPU sampling, context switches and ready-thread events
# (WPR's CPU profile) with GPU events (GPU.light), plus a snapshot of every game thread's name, group affinity and
# ideal processor (the check that found the Ys IX affinity-mask overflow). Run from an Administrator PowerShell.
#
#   .\capture.ps1 -Label solo
#   .\capture.ps1 -Label split -Seconds 30
#   from a normal window (one UAC prompt):
#   Start-Process pwsh -Verb RunAs -ArgumentList '-NoProfile','-File','<this file>','-Label','split'
#
# Captures go to <LSS_CAPTURES_DIR, else the repository's captures\>\<label>-<time>\: trace.etl, threads.csv, capture.json, and at the end done.txt or
# error.txt (so a caller that started the elevated window can tell when it has finished).
param(
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Label,
    [ValidateRange(5, 60)][int]$Seconds = 30
)
$ErrorActionPreference = 'Stop'
$root = if ($env:LSS_CAPTURES_DIR) { $env:LSS_CAPTURES_DIR } else { Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'captures' }
$capture = Join-Path $root ($Label + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $capture | Out-Null
trap { Set-Content (Join-Path $capture 'error.txt') ($_ | Out-String); break }

$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an Administrator PowerShell window; Windows requires profiling privileges.'
}
$game = Get-Process -Name ff7remake_ -ErrorAction Stop | Select-Object -First 1
$status = (& wpr -status 2>&1 | Out-String)
if ($status -notmatch 'WPR is not recording') {
    throw 'Another WPR capture may be active. Leave it intact and finish it before using this script.'
}

Add-Type -Namespace Lss -Name Threads -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct GroupAffinity { public UIntPtr Mask; public ushort Group; ushort r0, r1, r2; }
[StructLayout(LayoutKind.Sequential)] public struct ProcessorNumber { public ushort Group; public byte Number; public byte Reserved; }
[DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr OpenThread(uint access, bool inherit, uint id);
[DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
[DllImport("kernel32.dll", SetLastError = true)] public static extern bool GetThreadGroupAffinity(IntPtr thread, out GroupAffinity affinity);
[DllImport("kernel32.dll", SetLastError = true)] public static extern bool GetThreadIdealProcessorEx(IntPtr thread, out ProcessorNumber ideal);
[DllImport("kernel32.dll")] public static extern int GetThreadDescription(IntPtr thread, out IntPtr description);
[DllImport("kernel32.dll")] public static extern IntPtr LocalFree(IntPtr memory);
'@

# THREAD_QUERY_INFORMATION (GetThreadGroupAffinity needs it; limited access is not enough).
function Get-GameThreads([System.Diagnostics.Process]$process) {
    $process.Refresh()
    foreach ($thread in $process.Threads) {
        $row = [ordered]@{ id = $thread.Id; name = ''; group = ''; mask = ''; ideal = ''; priority = $thread.PriorityLevel
                           cpuSeconds = [math]::Round($thread.TotalProcessorTime.TotalSeconds, 2) }
        $handle = [Lss.Threads]::OpenThread(0x0040, $false, [uint32]$thread.Id)
        if ($handle -ne [IntPtr]::Zero) {
            try {
                $text = [IntPtr]::Zero
                if ([Lss.Threads]::GetThreadDescription($handle, [ref]$text) -ge 0 -and $text -ne [IntPtr]::Zero) {
                    $row.name = [Runtime.InteropServices.Marshal]::PtrToStringUni($text)
                    [void][Lss.Threads]::LocalFree($text)
                }
                $affinity = New-Object Lss.Threads+GroupAffinity
                if ([Lss.Threads]::GetThreadGroupAffinity($handle, [ref]$affinity)) {
                    $row.group = $affinity.Group
                    $row.mask = '0x{0:X}' -f $affinity.Mask.ToUInt64()
                }
                $ideal = New-Object Lss.Threads+ProcessorNumber
                if ([Lss.Threads]::GetThreadIdealProcessorEx($handle, [ref]$ideal)) {
                    $row.ideal = '{0}:{1}' -f $ideal.Group, $ideal.Number
                }
            } finally { [void][Lss.Threads]::CloseHandle($handle) }
        }
        [pscustomobject]$row
    }
}

$gameDir = Split-Path $game.Path
$modDll = Join-Path $gameDir 'ue4ss\Mods\LocalSplitScreen\dlls\main.dll'
$log = Join-Path $gameDir 'ue4ss\UE4SS.log'
$version = ''
if (Test-Path $log) {
    $line = Select-String -Path $log -Pattern '\[LocalSplitScreen\] (v\S+) starting' | Select-Object -Last 1
    if ($line) { $version = $line.Matches[0].Groups[1].Value }
}

$logical = (Get-CimInstance Win32_Processor | Measure-Object -Property NumberOfLogicalProcessors -Sum).Sum
[ordered]@{ label = $Label; pid = $game.Id; executable = $game.Path; seconds = $Seconds; modVersion = $version
            modDllHash = if (Test-Path $modDll) { (Get-FileHash $modDll).Hash } else { '' }
            logicalProcessors = $logical; processAffinity = '0x{0:X}' -f $game.ProcessorAffinity.ToInt64()
            time = (Get-Date -Format 'o') } |
    ConvertTo-Json | Set-Content (Join-Path $capture 'capture.json')

Write-Host "Return to FF7 Remake now and keep the scene to measure on screen. Recording begins in ten seconds."
Start-Sleep -Seconds 10
Get-GameThreads $game | Export-Csv (Join-Path $capture 'threads.csv') -NoTypeInformation
$recording = $false
try {
    & wpr -start CPU -start GPU.light -filemode
    if ($LASTEXITCODE) { throw 'Windows could not start the CPU/GPU recording.' }
    $recording = $true
    Start-Sleep -Seconds $Seconds
    & wpr -stop (Join-Path $capture 'trace.etl')
    if ($LASTEXITCODE) { throw 'Windows could not save the recording.' }
    $recording = $false
    Set-Content (Join-Path $capture 'done.txt') (Get-Date -Format 'o')
    Write-Host "Saved: $capture"
} finally {
    if ($recording) { & wpr -cancel }
}
