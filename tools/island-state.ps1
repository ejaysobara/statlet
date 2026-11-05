# Prints the Statlet island's window state and the foreground window's, plus the end of island.log.
# Run it the moment the island goes blank:  powershell -NoProfile -ExecutionPolicy Bypass -File tools\island-state.ps1
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class W {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; public override string ToString() { return L + "," + T + "-" + R + "," + B + " (" + (R - L) + "x" + (B - T) + ")"; } }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int v, int size);
    [DllImport("shell32.dll")] public static extern int SHQueryUserNotificationState(out int state);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    public static string Class(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
    public static RECT Rect(IntPtr h) { RECT r; GetWindowRect(h, out r); return r; }
    public static int Cloaked(IntPtr h) { int v; return DwmGetWindowAttribute(h, 14, out v, 4) == 0 ? v : -1; }
}
'@
[void][W]::SetProcessDPIAware()  # physical pixels, as island.log reports them

$proc = Get-Process statlet -ErrorAction SilentlyContinue
"island process : " + $(if ($proc) { "running, pid $($proc.Id), $([math]::Round($proc.PrivateMemorySize64 / 1MB)) MB private" } else { "NOT RUNNING" })

$island = [W]::FindWindow('StatletIsland', [NullString]::Value)  # the title changes with the readings; a plain $null arrives as ""
if ($island -eq [IntPtr]::Zero) { "island window  : not found" }
else {
    "island window  : visible $([W]::IsWindowVisible($island)), rect $([W]::Rect($island)), cloaked $([W]::Cloaked($island))"
    "island exstyle : 0x{0:X8}" -f [W]::GetWindowLong($island, -20)
}

$fg = [W]::GetForegroundWindow()
$fgPid = 0; [void][W]::GetWindowThreadProcessId($fg, [ref]$fgPid)
$fgName = (Get-Process -Id $fgPid -ErrorAction SilentlyContinue).ProcessName
"foreground     : '$([W]::Class($fg))' ($fgName), rect $([W]::Rect($fg)), zoomed $([W]::IsZoomed($fg)), cloaked $([W]::Cloaked($fg))"
"fg style       : 0x{0:X8}  ex 0x{1:X8}" -f [W]::GetWindowLong($fg, -16), [W]::GetWindowLong($fg, -20)

$state = 0; [void][W]::SHQueryUserNotificationState([ref]$state)
$names = @{1 = 'not present'; 2 = 'busy (fullscreen)'; 3 = 'D3D fullscreen'; 4 = 'presentation mode'; 5 = 'accepts notifications'; 6 = 'quiet time'; 7 = 'app (Store) fullscreen'}
"notification   : $state $($names[$state])"

Add-Type -AssemblyName System.Windows.Forms
foreach ($s in [System.Windows.Forms.Screen]::AllScreens) { "screen         : $($s.DeviceName) primary $($s.Primary) bounds $($s.Bounds) work $($s.WorkingArea)" }

$log = Join-Path $env:LOCALAPPDATA 'Statlet\island.log'
if (Test-Path $log) { ''; 'island.log (last 15 lines):'; Get-Content $log -Tail 15 }
