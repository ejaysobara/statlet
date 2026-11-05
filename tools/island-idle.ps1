# Measures an island's idle cost: CPU time used and context switches (wake-ups) per second over a period. Read-only.
# For W6, start it and then lock the screen (Win+L) or let the display turn off: the new build should show ~0 wake-ups.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\island-idle.ps1 [-Seconds 300] [-Id <pid>]
param([int]$Seconds = 300, [int]$Id = 0)
$ErrorActionPreference = 'Stop'
# Performance counters cannot tell two statlet.exe apart (both are "statlet"), so this reads each thread's context-switch
# count from NtQuerySystemInformation(SystemProcessInformation) instead. Offsets are for 64-bit Windows.
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Cs {
    [DllImport("ntdll.dll")] static extern int NtQuerySystemInformation(int cls, IntPtr buf, int len, out int needed);
    public static long Of(int pid) {
        int len = 1 << 20, needed;
        for (;;) {
            IntPtr buf = Marshal.AllocHGlobal(len);
            try {
                int status = NtQuerySystemInformation(5, buf, len, out needed);
                if (status == unchecked((int)0xC0000004)) { len = needed + (1 << 16); continue; }  // STATUS_INFO_LENGTH_MISMATCH
                if (status != 0) throw new Exception("NtQuerySystemInformation " + status.ToString("X"));
                for (IntPtr p = buf; ; ) {
                    int next = Marshal.ReadInt32(p), threads = Marshal.ReadInt32(p, 4);
                    if ((long)Marshal.ReadIntPtr(p, 0x50) == pid) {
                        long sum = 0;
                        for (int i = 0; i < threads; i++) sum += (uint)Marshal.ReadInt32(p, 0x100 + i * 0x50 + 0x40);
                        return sum;
                    }
                    if (next == 0) return -1;
                    p = IntPtr.Add(p, next);
                }
            } finally { Marshal.FreeHGlobal(buf); }
        }
    }
}
'@
$p = if ($Id) { Get-Process -Id $Id } else { Get-Process statlet | Select-Object -First 1 }
$cpu = $p.TotalProcessorTime
$switches = [Cs]::Of($p.Id)
Start-Sleep -Seconds $Seconds
$p.Refresh()
$rate = ([Cs]::Of($p.Id) - $switches) / $Seconds
$ms = ($p.TotalProcessorTime - $cpu).TotalMilliseconds
"pid {0} ({1}): CPU {2:N0} ms over {3} s ({4:N3}% of one core), {5:N1} context switches/s" -f $p.Id, $p.Path, $ms, $Seconds, ($ms / ($Seconds * 10)), $rate
