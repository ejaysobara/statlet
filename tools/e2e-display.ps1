# Cycles the display mode (external only, internal only, extend) with DisplaySwitch.exe, then restores the starting mode,
# and checks that a test island stays inside the primary work area and logs no FAILED line. Not part of the gate.
# The screen goes black for about 3 s per switch: run it with the lid OPEN and hands off the machine.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\e2e-display.ps1 [-Rounds 3]
param([int]$Rounds = 3, [string]$Exe = "$PSScriptRoot\..\island\out\gate\statlet.exe", [int]$SettleSeconds = 6)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class D {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint action, uint param, out RECT r, uint flags);
    public static RECT PrimaryWork() { RECT r; SystemParametersInfo(0x30, 0, out r, 0); return r; }  // SPI_GETWORKAREA: the primary's work area, now
    public static IntPtr IslandOf(uint pid) {  // the real island may be running too
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => { uint q; GetWindowThreadProcessId(h, out q); var s = new System.Text.StringBuilder(32); GetClassName(h, s, 32);
            if (q == pid && s.ToString() == "StatletIsland") { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
}
'@
[void][D]::SetProcessDPIAware()  # physical pixels, like the island
Add-Type -AssemblyName System.Windows.Forms

function Screens { [System.Windows.Forms.Screen]::AllScreens.Count }
function Switch-To($mode) { & DisplaySwitch.exe "/$mode"; Start-Sleep -Seconds $SettleSeconds }

# Windows cannot tell us the current mode reliably (WMI calls a closed lid's panel active), so the script starts only in
# extend mode, with both screens on, and restores extend at the end.
$start = 'extend'
if ((Screens) -lt 2) { Write-Host 'Needs the lid open and both screens extended (Win+P > Extend). With the lid closed, internal-only mode would leave no screen.' -ForegroundColor Red; exit 2 }

$data = Join-Path (Split-Path $Exe) 'display-test'
New-Item -ItemType Directory -Force $data | Out-Null
$log = Join-Path $data 'island.log'
$from = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
$env:STATLET_INSTANCE = 'display'; $env:STATLET_DATA_DIR = $data
$island = Start-Process $Exe -PassThru
Remove-Item Env:STATLET_INSTANCE, Env:STATLET_DATA_DIR
$failures = @()
try {
    Start-Sleep -Seconds 2
    $hwnd = [D]::IslandOf($island.Id)
    if ($hwnd -eq [IntPtr]::Zero) { throw 'the test island did not start' }
    Write-Host "start mode $start; $Rounds round(s)"
    for ($i = 1; $i -le $Rounds; $i++) {
        foreach ($mode in 'external', 'internal', 'extend') {
            Switch-To $mode
            $r = New-Object D+RECT; [void][D]::GetWindowRect($hwnd, [ref]$r)
            $work = [D]::PrimaryWork()  # not Screen.PrimaryScreen: WinForms caches it across display changes
            $ok = $work.R -gt $work.L -and $r.L -ge $work.L -and $r.T -ge $work.T -and $r.R -le $work.R -and -not $island.HasExited
            $line = "round $i $mode : island $($r.L),$($r.T)-$($r.R),$($r.B) primary work $($work.L),$($work.T)-$($work.R),$($work.B)"
            Write-Host $line -ForegroundColor $(if ($ok) { 'Gray' } else { 'Red' })
            if (-not $ok) { $failures += $line }
        }
    }
} finally {
    Switch-To $start
    if (-not $island.HasExited) { [void][D]::PostMessage([D]::IslandOf($island.Id), 0x10, [IntPtr]::Zero, [IntPtr]::Zero) }  # WM_CLOSE
    if (-not $island.WaitForExit(5000)) { Stop-Process -Id $island.Id }
    Write-Host "restored $start ($(Screens) screens)"
}
$lines = (Get-Content $log -Raw).Substring([Math]::Min($from, (Get-Item $log).Length)) -split "`n"
$failures += @($lines | Where-Object { $_ -match 'FAILED' })
$places = @($lines | Where-Object { $_ -match 'place \(' }).Count
Write-Host "$places place lines; log $log"
if ($failures) { Write-Host "FAILED: $($failures.Count) problem(s)" -ForegroundColor Red; $failures | ForEach-Object { Write-Host "  $_" }; exit 1 }
Write-Host 'display cycling passed' -ForegroundColor Green
exit 0
