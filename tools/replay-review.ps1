# Pauses an open iRacing replay at chosen session times and screenshots each moment
# Run irinspect capture at the same time so every screenshot has matching telemetry
#   replay-review.ps1 -SessionNum 2 -Times 812.4,915.0 -CarNumber 64 -Group 19 -Out shots
param(
  [Parameter(Mandatory)][int]$SessionNum,
  [Parameter(Mandatory)][double[]]$Times,
  [Parameter(Mandatory)][int]$CarNumber,
  [int]$Group = 19,
  [string]$Out = 'shots',
  [int]$SettleMs = 2500
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Sdk {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern uint RegisterWindowMessage(string name);
  [DllImport("user32.dll")] public static extern bool SendNotifyMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  static uint id = RegisterWindowMessage("IRSDK_BROADCASTMSG");
  // the documented SDK broadcast: wParam = MAKELONG(command, var1), lParam = var2
  public static void Send(int cmd, int var1, int var2) {
    SendNotifyMessage((IntPtr)0xffff, id, (IntPtr)((cmd & 0xffff) | (var1 << 16)), (IntPtr)var2);
  }
}
'@
[Sdk]::SetProcessDPIAware() | Out-Null
New-Item -ItemType Directory -Force $Out | Out-Null
$camSwitchNum = 1; $setPlaySpeed = 3; $searchSessionTime = 12
$w = [Sdk]::GetSystemMetrics(0); $h = [Sdk]::GetSystemMetrics(1)
$log = Join-Path $Out 'shots.csv'
'file,session_num,session_time' | Set-Content $log
foreach ($t in $Times) {
  [Sdk]::Send($setPlaySpeed, 0, 0)
  [Sdk]::Send($searchSessionTime, $SessionNum, [int]($t * 1000))
  [Sdk]::Send($camSwitchNum, $CarNumber, $Group)
  Start-Sleep -Milliseconds $SettleMs
  $file = Join-Path $Out ('t{0:000000.000}.png' -f $t)
  $bmp = New-Object System.Drawing.Bitmap $w, $h
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen(0, 0, 0, 0, $bmp.Size)
  $bmp.Save($file)
  $g.Dispose(); $bmp.Dispose()
  "$file,$SessionNum,$t" | Add-Content $log
  "captured $file"
}
