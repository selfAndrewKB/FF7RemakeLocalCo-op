# Nsight Graphics GPU Trace of FF7 Remake with players 2 and 3 in. Run ELEVATED: the NVIDIA driver gives
# the GPU performance counters to administrators only (unless "Allow access to all users" is set in the NVIDIA Control
# Panel, Developer > Manage GPU Performance Counters), and an elevated game takes keys only from an elevated sender, so
# the whole sequence runs here: launch under ngfx, load the save, both joins, settle, F11 (the trace hotkey), wait.
#   Start-Process pwsh -Verb RunAs -ArgumentList '-NoProfile','-File','<this file>'          (one UAC prompt)
# Output: <LSS_CAPTURES_DIR, else the repository's captures\>\gputrace-<label>-<time>\ : ngfx's trace (.ngfx-gputrace) and export, run.log, done.txt/error.txt.
param(
    [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Label = '3p',
    [int]$SettleSeconds = 15,
    [int]$Frames = 5
)
$ErrorActionPreference = 'Stop'
$root = if ($env:LSS_CAPTURES_DIR) { $env:LSS_CAPTURES_DIR } else { Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'captures' }
$out = Join-Path $root ('gputrace-' + $Label + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $out | Out-Null
$logFile = Join-Path $out 'run.log'
function Note([string]$text) { Add-Content $logFile ((Get-Date -Format 'HH:mm:ss') + ' ' + $text); Write-Host $text }
trap { Note ("error: " + ($_ | Out-String)); Set-Content (Join-Path $out 'error.txt') ($_ | Out-String); break }

# NGFX names ngfx.exe; else the newest Nsight Graphics install. The game's folder is the one tools\game.py finds.
$ngfx = if ($env:NGFX) { $env:NGFX } else {
    Get-ChildItem 'C:\Program Files\NVIDIA Corporation\Nsight Graphics *\host\windows-desktop-nomad-x64\ngfx.exe' -ErrorAction SilentlyContinue |
        Sort-Object { [version](($_.FullName -replace '.*Nsight Graphics ([\d.]+)\\.*', '$1')) } | Select-Object -Last 1 -ExpandProperty FullName }
if (-not $ngfx) { throw 'Nsight Graphics not found; set NGFX to its ngfx.exe.' }
$tools = Split-Path -Parent $PSScriptRoot
$game = Join-Path $tools 'game.py'
$win64 = (& python -I -c "import sys; sys.path.insert(0, sys.argv[1]); import game; print(game.GAME_DIR)" $tools).Trim()
if (-not (Test-Path (Join-Path $win64 'ff7remake_.exe'))) { throw "The game was not found in $win64; set FF7R_GAME_DIR to its End\Binaries\Win64 folder." }
if (Get-Process ff7remake_ -ErrorAction SilentlyContinue) { throw 'The game is already running; quit it first.' }

# SteamAppId keeps the Steam API from relaunching the game through Steam; the redirect picks the RTX's DXGI entry the
# game's duplicate adapter LUID hides from GPU Trace (its own TARGET WARNING names this variable).
$env_ = 'SteamAppId=1462040; SteamGameId=1462040; GPUTRACE_D3D12_REDIRECT_DUPLICATE_DXGI_ADAPTER=1;'
$ngfxArgs = @('--activity', '"GPU Trace Profiler"', '--exe', "`"$win64\ff7remake_.exe`"", '--dir', "`"$win64`"",
              '--args', 'End', '--env', "`"$env_`"", '--no-timeout', '--output-dir', "`"$out`"", '--start-after-hotkey',
              '--limit-to-frames', "$Frames", '--max-duration-ms', '1000', '--auto-export', '--verbose')
$proc = Start-Process $ngfx -ArgumentList $ngfxArgs -RedirectStandardOutput (Join-Path $out 'ngfx.out.txt') `
    -RedirectStandardError (Join-Path $out 'ngfx.err.txt') -PassThru
Note "ngfx started (pid $($proc.Id))"

function Console([string[]]$lines) {
    $a = @('-I', $game); foreach ($l in $lines) { $a += @('-c', $l) }
    $r = & python @a 2>&1 | Out-String; Note ("console: " + ($lines -join ' | ') + ' -> ' + $r.Trim()); return $r
}
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt 180) {
    if ($proc.HasExited) { throw 'ngfx exited before the game came up (see ngfx.out.txt).' }
    $r = & python -I $game -c 'get log-level' 2>&1 | Out-String
    if ($r -match '^value') { break }
    Start-Sleep -Seconds 2
}
Note "console up after $([int]((Get-Date) - $t0).TotalSeconds) s"
Start-Sleep -Seconds 8
$r = & python -I $game load-save 2>&1 | Out-String; Note ("load-save -> " + $r.Trim())
Console @('join', 'wait-until p2.in == on 30000', 'p3.join', 'wait-until p3.in == on 30000') | Out-Null
# Three columns across a row of monitors (the span, with the mirror in an empty column) is the setup measured; a launch
# can come up in one window without it, so ask for it and log the state the trace runs in.
Console @('get window-span.state', 'window-span on') | Out-Null
Start-Sleep -Seconds 8
Console @('get window-span.state', 'get column-layout.state') | Out-Null
Note "settling $SettleSeconds s"
Start-Sleep -Seconds $SettleSeconds
# "LSS view 1..n" GPU events around each player's view family (render/view_marks.h) and a named event per render pass inside each (render/pass_marks.h), on for the trace only.
Console @('view-marks on', 'pass-marks on') | Out-Null
Start-Sleep -Seconds 3
Console @('get view-marks', 'get pass-marks') | Out-Null
$r = & python -I $game keys F11 2>&1 | Out-String; Note ("F11 -> " + $r.Trim())
Start-Sleep -Seconds 3
Console @('get view-marks', 'get pass-marks', 'pass-marks off', 'view-marks off') | Out-Null
$t1 = Get-Date
while (-not $proc.HasExited -and ((Get-Date) - $t1).TotalSeconds -lt 300) {
    if (Get-ChildItem $out -Recurse -Filter '*.ngfx-gputrace' -ErrorAction SilentlyContinue) { Start-Sleep -Seconds 20; break }
    Start-Sleep -Seconds 2
}
Note ("files: " + ((Get-ChildItem $out -Recurse | ForEach-Object { $_.Name }) -join ', '))
Set-Content (Join-Path $out 'done.txt') (Get-Date -Format 'o')
