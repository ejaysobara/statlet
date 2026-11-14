# Break test for the widget registry: drives a separate test island (STATLET_INSTANCE, own data dir; never touches your
# live island or settings) with lock/unlock bursts, a fullscreen window shown and hidden, rapid settings writes and a
# display-change burst, then checks the log, the window bounds, handle counts and wake-ups. Needs a desktop; the
# screen blinks black under a second per fullscreen toggle. Exit code = number of FAILed checks.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\qa-registry.ps1 [-Exe island\out\statlet.exe] [-IdleSeconds 20]
param([string]$Exe = "$PSScriptRoot\..\island\out\statlet.exe", [int]$IdleSeconds = 20, [int]$Rapid = 25)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing @'
using System; using System.Runtime.InteropServices; using System.Text; using System.Windows.Forms;
public static class Q {
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr proc, uint flags);
    public struct RECT { public int L, T, R, B; }
    delegate bool EnumProc(IntPtr h, IntPtr l);
    public static IntPtr IslandOf(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var s = new StringBuilder(64); GetClassNameW(h, s, 64);
            if (p == pid && s.ToString() == "StatletIsland") { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
}
public class NoActForm : Form {
    protected override bool ShowWithoutActivation { get { return true; } }
    protected override CreateParams CreateParams { get { var cp = base.CreateParams; cp.ExStyle |= 0x08000000 | 0x8; return cp; } }  // NOACTIVATE | TOPMOST
}
'@
$script:fails = 0
function Check($name, $ok, $detail = '') {
    if ($ok) { "PASS  $name" } else { "FAIL  $name  $detail"; $script:fails++ }
}
$tmp = Join-Path $env:TEMP "statlet-qa-$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
$log = "$tmp\island.log"
$settings = "$tmp\settings.json"
function Log { if (Test-Path $log) { Get-Content $log -Raw } else { '' } }
function Count($text, $pat) { ([regex]::Matches($text, $pat)).Count }
function Pump($ms) { $end = [DateTime]::Now.AddMilliseconds($ms); while ([DateTime]::Now -lt $end) { [Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 20 } }
function Post($m, $w, $l = 0) { [void][Q]::PostMessageW($script:isl, $m, [IntPtr]$w, [IntPtr]$l) }
function Full { $f = New-Object NoActForm; $f.FormBorderStyle = 'None'; $f.StartPosition = 'Manual'; $f.BackColor = 'Black'; $f.ShowInTaskbar = $false; $f.Bounds = [Windows.Forms.Screen]::PrimaryScreen.Bounds; $f.Show(); $f }
function Switches($text) { [double](($text -split ', ')[-1] -replace '[^\d.].*', '') }

$psi = New-Object Diagnostics.ProcessStartInfo (Resolve-Path $Exe)
$psi.UseShellExecute = $false
$psi.WorkingDirectory = Split-Path (Resolve-Path $Exe)
$psi.EnvironmentVariables['STATLET_INSTANCE'] = 'qa'
$psi.EnvironmentVariables['STATLET_DATA_DIR'] = $tmp
$psi.EnvironmentVariables['STATLET_TOP_OFFSET'] = '300'
$p = [Diagnostics.Process]::Start($psi)
try {
    for ($i = 0; $i -lt 100 -and ($script:isl = [Q]::IslandOf($p.Id)) -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 100 }
    Check 'island window appears' ($script:isl -ne [IntPtr]::Zero)
    Pump 1500
    Check 'starts with timers on' ((Log) -match 'timers on')
    $p.Refresh(); $h0 = $p.HandleCount; $g0 = [Q]::GetGuiResources($p.Handle, 0); $u0 = [Q]::GetGuiResources($p.Handle, 1)

    # 1. shown: idle cost
    $shown = & "$PSScriptRoot\island-idle.ps1" -Id $p.Id -Seconds $IdleSeconds
    "      shown   : $shown"

    # 2. locked: every polled widget must stop (v1 rule), wake-ups drop
    Post 0x2B1 7   # WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK
    Pump 500
    Check 'lock turns timers off' ((Log) -match 'timers off')
    $lk = & "$PSScriptRoot\island-idle.ps1" -Id $p.Id -Seconds $IdleSeconds
    "      locked  : $lk"
    Check 'locked wake-ups below shown' ((Switches $lk) -lt (Switches $shown)) "shown $(Switches $shown), locked $(Switches $lk)"
    Post 0x2B1 8   # unlock
    Pump 600
    $t = Log
    Check 'unlock turns timers back on' ($t.LastIndexOf('timers on') -gt $t.LastIndexOf('timers off'))

    # 3. lock/unlock burst: every lock logs one off, ends on
    $before = Count (Log) 'timers off'
    1..$Rapid | % { Post 0x2B1 7; Start-Sleep -Milliseconds 15; Post 0x2B1 8; Start-Sleep -Milliseconds 15 }
    Pump 800
    $t = Log
    Check "lock/unlock x$Rapid ends with timers on" ($t.LastIndexOf('timers on') -gt $t.LastIndexOf('timers off'))
    Check "lock/unlock x$Rapid logs $Rapid 'off' lines" ((Count $t 'timers off') - $before -eq $Rapid) "got $((Count $t 'timers off') - $before)"

    # 4. fullscreen app: hide, wake-ups drop, closing brings the island back
    $mark = (Log).Length
    $f = Full; Pump 700
    $t = (Log).Substring($mark)
    Check 'fullscreen window hides the island' ($t -match '-> hidden') $t
    $hid = & "$PSScriptRoot\island-idle.ps1" -Id $p.Id -Seconds $IdleSeconds
    "      hidden  : $hid"
    Check 'hidden wake-ups below shown' ((Switches $hid) -lt (Switches $shown)) "shown $(Switches $shown), hidden $(Switches $hid)"
    $mark = (Log).Length
    $f.Close(); Pump 1500
    Check 'closing it brings the island back' ((Log).Substring($mark) -match 'away hidden -> ')

    # 5. rapid fullscreen toggling
    1..10 | % { $f = Full; Pump 250; $f.Close(); Pump 250 }
    Pump 1500
    $t = Log
    Check 'fullscreen toggled x10: island alive' (-not $p.HasExited)
    Check 'fullscreen toggled x10: ends with timers on' ($t.LastIndexOf('timers on') -gt $t.LastIndexOf('timers off'))

    # 6. rapid settings writes (the island watches the folder), then a malformed file
    1..$Rapid | % { Set-Content $settings ('{"topOffset":' + ($_ % 40) + ',"hoverPollMs":' + (50 + $_) + '}') -Encoding ascii; Start-Sleep -Milliseconds 20 }
    Set-Content $settings '{"topOffset": 300 garbage' -Encoding ascii
    Pump 1000
    Check "settings written x$Rapid + malformed: alive" (-not $p.HasExited)

    # 7. display change + resume burst
    $mark = (Log).Length
    1..5 | % { Post 0x7E 32 0; Post 0x218 0x12 0 }   # WM_DISPLAYCHANGE, WM_POWERBROADCAST PBT_APMRESUMEAUTOMATIC
    Pump 1200
    $n = Count (Log).Substring($mark) 'place \('
    Check 'display/resume burst coalesced to 1-2 placements' ($n -ge 1 -and $n -le 2) "place lines: $n"

    # 8. final state
    $r = New-Object 'Q+RECT'; [void][Q]::GetWindowRect($script:isl, [ref]$r)
    $sc = [Windows.Forms.Screen]::PrimaryScreen.Bounds
    Check 'island window inside the primary screen' ($r.L -ge $sc.Left -and $r.R -le $sc.Right -and $r.T -ge $sc.Top -and $r.B -le $sc.Bottom) "$($r.L),$($r.T)-$($r.R),$($r.B)"
    Check 'island window visible' ([Q]::IsWindowVisible($script:isl))
    $p.Refresh()
    Check 'handle count stable (<= +25)' ($p.HandleCount - $h0 -le 25) "start $h0, end $($p.HandleCount)"
    Check 'GDI/USER objects stable (<= +10)' (([Q]::GetGuiResources($p.Handle, 0) - $g0) -le 10 -and ([Q]::GetGuiResources($p.Handle, 1) - $u0) -le 10)
    Check 'no FAILED line in the log' ((Log) -notmatch 'FAILED')
    Check 'process still alive' (-not $p.HasExited)
    "      working set $([int]($p.WorkingSet64 / 1MB)) MB, private $([int]($p.PrivateMemorySize64 / 1MB)) MB"
} finally {
    if (-not $p.HasExited) { [void][Q]::PostMessageW($script:isl, 0x10, [IntPtr]::Zero, [IntPtr]::Zero); if (-not $p.WaitForExit(5000)) { $p.Kill() } }
}
"$script:fails check(s) FAILED"
exit $script:fails
