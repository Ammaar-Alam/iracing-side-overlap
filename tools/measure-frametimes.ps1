# A/B frame time measurement of iRacing with and without the overlay, on identical replay footage
# Needs an elevated shell for PresentMon's ETW session. Open a replay in iRacing first.
#   measure-frametimes.ps1 -SessionNum 2 -StartTime 300 [-Seconds 60] [-Rounds 2]
param(
  [Parameter(Mandatory)][int]$SessionNum,
  [Parameter(Mandatory)][double]$StartTime,
  [int]$Seconds = 60,
  [int]$Rounds = 2,
  [string]$Overlay = "$env:LOCALAPPDATA\Programs\iRacingSideOverlap\side-overlap.exe",
  [string]$PresentMon = (Join-Path $PSScriptRoot '..\.tools\presentmon\PresentMon-2.6.0-x64.exe'),
  [string]$Out = (Join-Path $PSScriptRoot '..\captures\frametimes')
)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Sdk {
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern uint RegisterWindowMessage(string name);
  [DllImport("user32.dll")] public static extern bool SendNotifyMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  static uint id = RegisterWindowMessage("IRSDK_BROADCASTMSG");
  public static void Send(int cmd, int var1, int var2) {
    SendNotifyMessage((IntPtr)0xffff, id, (IntPtr)((cmd & 0xffff) | (var1 << 16)), (IntPtr)var2);
  }
}
'@
New-Item -ItemType Directory -Force $Out | Out-Null

function Stop-Overlay {
  Get-Process side-overlap -ErrorAction SilentlyContinue | ForEach-Object {
    taskkill /PID $_.Id | Out-Null
    $_.WaitForExit(5000) | Out-Null
  }
}

function Measure($label, $round) {
  $setPlaySpeed = 3; $searchSessionTime = 12
  [Sdk]::Send($setPlaySpeed, 0, 0)
  [Sdk]::Send($searchSessionTime, $SessionNum, [int]($StartTime * 1000))
  Start-Sleep -Seconds 3
  if ($label -eq 'overlay') {
    Start-Process $Overlay -ArgumentList '--demo', '--stats' | Out-Null
    Start-Sleep -Seconds 1
  }
  [Sdk]::Send($setPlaySpeed, 1, 0)
  Start-Sleep -Seconds 2
  $csv = Join-Path $Out "$label-$round.csv"
  & $PresentMon --process_name iRacingSim64DX11.exe --output_file $csv --timed $Seconds --terminate_after_timed `
    --stop_existing_session --no_console_stats | Out-Null
  Stop-Overlay
  $csv
}

function Summarize($files) {
  $times = foreach ($f in $files) {
    $rows = Import-Csv $f
    $col = if ($rows[0].PSObject.Properties.Name -contains 'FrameTime') { 'FrameTime' } else { 'MsBetweenPresents' }
    $rows | ForEach-Object { [double]$_.$col }
  }
  $s = $times | Sort-Object
  $n = $s.Count
  $avg = ($s | Measure-Object -Average).Average
  $worst = $s[[int]($n * 0.99)..($n - 1)] | Measure-Object -Average
  [pscustomobject]@{
    frames = $n
    avg_fps = [math]::Round(1000 / $avg, 1)
    median_ms = [math]::Round($s[[int]($n * 0.5)], 2)
    p99_ms = [math]::Round($s[[int]($n * 0.99)], 2)
    p999_ms = [math]::Round($s[[int]($n * 0.999)], 2)
    low1pct_fps = [math]::Round(1000 / $worst.Average, 1)
  }
}

Stop-Overlay
$results = @{ baseline = @(); overlay = @() }
for ($r = 1; $r -le $Rounds; $r++) {
  foreach ($label in 'baseline', 'overlay') { $results[$label] += Measure $label $r }
}
[Sdk]::Send(3, 0, 0)
foreach ($label in 'baseline', 'overlay') { "$label"; Summarize $results[$label] | Format-List }
