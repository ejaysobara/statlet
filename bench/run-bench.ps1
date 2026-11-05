# Benchmarks the island prototypes. Run from any folder: powershell -File bench\run-bench.ps1
# Each target is a process launched with the ISLAND_BENCH env vars that bench/native/island.cpp and
# bench/winui/IslandBar/Bench.cs understand. Results go to bench/results/<target>.json.
param(
    [int]$StartupRuns = 10,
    [int]$IdleRuns = 3,
    [int]$IdleSeconds = 60,
    [int]$CycleRuns = 3,
    [int]$CycleSeconds = 20,
    [string[]]$Only
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$targets = [ordered]@{
    # The native Statlet island (island/): compositor-driven spring, acrylic, click-through.
    'statlet'   = "$root\..\island\out\statlet.exe"
    'native'    = "$root\native\island.exe"
    'winui-r2r' = "$root\winui\out-r2r\IslandBar.exe"
    'winui-jit' = "$root\winui\IslandBar\bin\x64\Release\net8.0-windows10.0.19041.0\win-x64\IslandBar.exe"
    # Current shell design (acrylic, rounded region) at HEAD vs with the composition-driven animation; both untrimmed ReadyToRun.
    'winui-before' = "$root\winui-before\out-r2r\IslandBar.exe"
    'winui-after'  = "$root\winui-after\out-r2r\IslandBar.exe"
    'slint'        = "$root\slint\target\release\island-slint.exe"
}
# Keeps Statlet clear of Dynamic Edge at the top center; the other prototypes already sit 300 epx down.
$env:STATLET_TOP_OFFSET = '300'
$resultDir = "$root\results"
New-Item -ItemType Directory -Force $resultDir | Out-Null
$tmp = Join-Path $env:TEMP "island-bench"
New-Item -ItemType Directory -Force $tmp | Out-Null

function Launch($exe, $mode, $out) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo $exe
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = Split-Path $exe
    foreach ($k in 'ISLAND_BENCH', 'ISLAND_BENCH_OUT', 'ISLAND_BENCH_TOGGLE_MS', 'ISLAND_BENCH_SECONDS') { [void]$psi.EnvironmentVariables.Remove($k) }
    if ($mode) {
        $psi.EnvironmentVariables['ISLAND_BENCH'] = $mode
        $psi.EnvironmentVariables['ISLAND_BENCH_OUT'] = $out
        $psi.EnvironmentVariables['ISLAND_BENCH_TOGGLE_MS'] = '1000'
        # Outlasts the 2 s warm-up, the measured window and Get-Counter's own start-up; the harness kills it after.
        $psi.EnvironmentVariables['ISLAND_BENCH_SECONDS'] = [string]($CycleSeconds + 15)
    }
    [System.Diagnostics.Process]::Start($psi)
}

# Once a second for $seconds (doubles as the wait): GPU engine utilization of the process and of DWM
# (summed over engines), and DWM CPU. DWM does the compositing for both stacks, so its share counts too.
function Sample($procId, $seconds) {
    $dwmId = (Get-Process dwm | Select-Object -First 1).Id
    $paths = "\GPU Engine(pid_$($procId)_*)\Utilization Percentage", "\GPU Engine(pid_$($dwmId)_*)\Utilization Percentage", "\Process(dwm)\% Processor Time"
    $samples = Get-Counter $paths -SampleInterval 1 -MaxSamples $seconds -ErrorAction SilentlyContinue
    function Avg($filter) {
        $sums = $samples | ForEach-Object { ($_.CounterSamples | Where-Object { $_.Path -like $filter } | Measure-Object CookedValue -Sum).Sum }
        [math]::Round(($sums | Measure-Object -Average).Average, 3)
    }
    [ordered]@{
        gpuPercent           = Avg "*pid_$($procId)_*"
        dwmGpuPercent        = Avg "*pid_$($dwmId)_*"
        dwmCpuPercentOneCore = Avg "*process(dwm)*"
    }
}

function PrivateWorkingSetMB($procId) {
    $p = Get-CimInstance Win32_PerfFormattedData_PerfProc_Process -Filter "IDProcess=$procId"
    [math]::Round($p.WorkingSetPrivate / 1MB, 2)
}

# TotalProcessorTime is charged per 15.6 ms clock tick, which aliases with 144 Hz frame work and swings
# 1-20% between identical runs. Cycle counts are exact, so CPU is reported from them.
Add-Type -Namespace Bench -Name Cycles -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern bool QueryProcessCycleTime(IntPtr process, out ulong cycles);
[DllImport("kernel32.dll")] public static extern bool QueryThreadCycleTime(IntPtr thread, out ulong cycles);
[DllImport("kernel32.dll")] public static extern IntPtr GetCurrentThread();
public static ulong Of(IntPtr process) { ulong c; QueryProcessCycleTime(process, out c); return c; }
public static double CyclesPerSecond() {
    ulong a, b; var sw = System.Diagnostics.Stopwatch.StartNew();
    QueryThreadCycleTime(GetCurrentThread(), out a);
    while (sw.ElapsedMilliseconds < 500) { }
    QueryThreadCycleTime(GetCurrentThread(), out b);
    return (b - a) / sw.Elapsed.TotalSeconds;
}
'@
# A busy thread's cycles per second: the cycle counter's rate, so cycles convert to "ms of one core".
$cyclesPerMs = [Bench.Cycles]::CyclesPerSecond() / 1000
Write-Host "cycle counter: $([math]::Round($cyclesPerMs / 1000)) MHz"

function Percentile($sorted, $q) { $sorted[[math]::Min($sorted.Count - 1, [int][math]::Floor($q * $sorted.Count))] }

# DWM baseline with no island running (other apps, including Dynamic Edge, keep DWM busy); subtract it from DWM figures.
$baseline = @(1..3 | ForEach-Object { Sample 0 $CycleSeconds })
$baseline | ConvertTo-Json -Depth 3 | Set-Content -Encoding utf8 "$resultDir\dwm-baseline.json"
Write-Host "dwm baseline: $($baseline | ConvertTo-Json -Compress)"

foreach ($name in $targets.Keys) {
    # -File passes "a,b" as one string, so split it here.
    if ($Only -and $name -notin ($Only -split ",")) { continue }
    $exe = $targets[$name]
    Write-Host "== $name"
    $result = [ordered]@{ target = $name; exe = $exe; startupMs = @(); idle = @(); cycle = @() }

    # 1. Startup: process launch to first presented frame. First run is a discarded warm-up (file cache).
    for ($i = 0; $i -le $StartupRuns; $i++) {
        $out = "$tmp\$name-startup.txt"; Remove-Item $out -ErrorAction SilentlyContinue
        $t0 = [DateTime]::UtcNow.ToFileTimeUtc()
        $p = Launch $exe 'startup' $out
        if (-not $p.WaitForExit(30000)) { $p.Kill(); throw "$name startup run timed out" }
        $ms = ([double](Get-Content $out | Select-Object -First 1) - $t0) / 10000
        if ($i -gt 0) { $result.startupMs += [math]::Round($ms, 1) }
        Start-Sleep -Milliseconds 500
    }
    Write-Host "  startup ms: $($result.startupMs -join ', ')"

    # 2. Idle: plain launch, settle 10 s, then CPU and GPU over the idle window, then memory.
    for ($i = 0; $i -lt $IdleRuns; $i++) {
        $p = Launch $exe $null $null
        Start-Sleep -Seconds 10
        $p.Refresh()
        $cpu0 = [Bench.Cycles]::Of($p.Handle)
        $gpu = Sample $p.Id $IdleSeconds
        $p.Refresh()
        $cpuMs = ([Bench.Cycles]::Of($p.Handle) - $cpu0) / $cyclesPerMs
        $idle = [ordered]@{
            privateWorkingSetMB = PrivateWorkingSetMB $p.Id
            commitMB            = [math]::Round($p.PrivateMemorySize64 / 1MB, 2)
            workingSetMB        = [math]::Round($p.WorkingSet64 / 1MB, 2)
            threads             = $p.Threads.Count
            handles             = $p.HandleCount
            cpuMsPerMin         = [math]::Round($cpuMs * 60 / $IdleSeconds, 1)
            cpuPercentOneCore   = [math]::Round($cpuMs / ($IdleSeconds * 1000) * 100, 3)
        }
        $p.Kill(); $p.WaitForExit()
        foreach ($k in $gpu.Keys) { $idle[$k] = $gpu[$k] }
        $result.idle += $idle
        Write-Host "  idle: $($idle | ConvertTo-Json -Compress)"
        Start-Sleep -Seconds 2
    }

    # 3. Expand/collapse: toggle every 1 s; measure CPU, DWM CPU and GPU over a window after a 2 s warm-up.
    for ($i = 0; $i -lt $CycleRuns; $i++) {
        $out = "$tmp\$name-cycle.txt"; Remove-Item $out -ErrorAction SilentlyContinue
        $p = Launch $exe 'cycle' $out
        Start-Sleep -Seconds 2
        $p.Refresh()
        $cpu0 = [Bench.Cycles]::Of($p.Handle)
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $gpu = Sample $p.Id $CycleSeconds
        $p.Refresh()
        $secs = $sw.Elapsed.TotalSeconds
        $cpuMs = ([Bench.Cycles]::Of($p.Handle) - $cpu0) / $cyclesPerMs
        $peakWs = [math]::Round($p.PeakWorkingSet64 / 1MB, 2)
        if (-not $p.WaitForExit(30000)) { $p.Kill(); throw "$name cycle run timed out" }
        $frames = @(Get-Content $out | ForEach-Object { [double]$_ } | Sort-Object)
        $cycle = [ordered]@{
            cpuPercentOneCore    = [math]::Round($cpuMs / ($secs * 1000) * 100, 2)
            peakWorkingSetMB     = $peakWs
            frames               = $frames.Count
            frameMsMean          = [math]::Round(($frames | Measure-Object -Average).Average, 3)
            frameMsP50           = Percentile $frames 0.50
            frameMsP95           = Percentile $frames 0.95
            frameMsP99           = Percentile $frames 0.99
            frameMsMax           = $frames[-1]
            # 144 Hz panel: a frame is 6.94 ms; anything over 1.5x that is a missed refresh.
            longFramesPct        = [math]::Round(@($frames | Where-Object { $_ -gt 10.4 }).Count / [math]::Max(1, $frames.Count) * 100, 2)
        }
        foreach ($k in $gpu.Keys) { $cycle[$k] = $gpu[$k] }
        $result.cycle += $cycle
        Write-Host "  cycle: $($cycle | ConvertTo-Json -Compress)"
        Start-Sleep -Seconds 2
    }

    $result | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 "$resultDir\$name.json"
}
Write-Host "done"
