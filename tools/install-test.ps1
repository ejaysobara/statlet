# Installs, upgrades and uninstalls the built setup on THIS PC and checks the plan's install rows.
# Needs a desktop. It backs up your settings.json and your Start-with-Windows value first and puts both back at the end,
# even on failure. Close the island yourself first (it refuses to run while one is up). Run:  .\package.ps1  then  .\tools\install-test.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$version = (Get-Content "$root\VERSION" -TotalCount 1).Trim()
$setup = "$root\dist\Statlet-Setup-$version.exe"
if (-not (Test-Path $setup)) { throw "Run .\package.ps1 first ($setup is missing)" }
if (Get-Process statlet -ErrorAction SilentlyContinue) { throw 'Close the island first (tray icon > Quit Statlet)' }

$dir = Join-Path $env:LOCALAPPDATA "Programs\StatletInstallTest-$PID"   # not %TEMP%: unsigned exes run from there draw heuristic detections
$data = Join-Path $env:LOCALAPPDATA 'Statlet'
$run = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$backup = Join-Path $env:TEMP "statlet-itest-backup-$PID"
New-Item -ItemType Directory $backup | Out-Null
if (Test-Path $data) { Copy-Item $data "$backup\data" -Recurse }
$oldRun = (Get-ItemProperty $run -Name Statlet -ErrorAction SilentlyContinue).Statlet
$failures = @()
function Check($ok, $what) { if ($ok) { Write-Host "ok   $what" -ForegroundColor Green } else { Write-Host "FAIL $what" -ForegroundColor Red; $script:failures += $what } }
# WaitForExit, not Start-Process -Wait: that one also waits for the island the setup starts
function Run($exe, $argText) { $p = [Diagnostics.Process]::Start($exe, $argText); $p.WaitForExit() }
function Install { Run $setup "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR=`"$dir`"" }
function Hash($f) { if (Test-Path $f) { (Get-FileHash $f).Hash } else { 'none' } }
function WaitFor($test, $secs) { for ($i = 0; $i -lt $secs * 4; $i++) { if (& $test) { return $true }; Start-Sleep -Milliseconds 250 }; $false }
function Island { @(Get-Process statlet -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq "$dir\statlet.exe" }) }
function Settings { @(Get-Process StatletSettings -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dir*" }) }

try {
    # A pretend old zip install: a Run value pointing at another folder, and a settings file to protect.
    New-Item -ItemType Directory $data -Force | Out-Null
    '{"topOffset":14,"fromTheFuture":1}' | Set-Content "$data\settings.json" -Encoding ascii -NoNewline
    Set-ItemProperty $run -Name Statlet -Value '"D:\old zip\statlet.exe"'
    $settingsHash = Hash "$data\settings.json"

    Install
    Check ((Test-Path "$dir\statlet.exe") -and (Test-Path "$dir\settings\StatletSettings.exe")) 'fresh install: files in place, no admin prompt'
    Check (WaitFor { (Island).Count -eq 1 } 8) 'fresh install: island started'
    Check ((Get-Item "$dir\statlet.exe").VersionInfo.ProductVersion -eq $version) "island reports version $version"
    Check ((Get-ItemProperty $run -Name Statlet).Statlet -eq "`"$dir\statlet.exe`"") 'old Run value rewritten to the installed path'
    Check ((Hash "$data\settings.json") -eq $settingsHash) 'settings.json untouched by install'

    Start-Process explorer.exe "`"$dir\settings\StatletSettings.exe`""
    Check (WaitFor { (Settings).Count -ge 1 } 15) 'Settings opens'
    $before = @(Island)[0].Id
    Start-Process explorer.exe "`"$dir\statlet.exe`""   # as the Start menu entry would, while the island runs
    Start-Sleep 3
    Check ((Island).Count -eq 1 -and (Island)[0].Id -eq $before) 'second start leaves one island running'

    Install   # upgrade with island and Settings open
    Check (WaitFor { (Island).Count -eq 1 } 10) 'upgrade: island is back'
    Check ((Hash "$data\settings.json") -eq $settingsHash) 'upgrade: settings.json byte-identical'
    Check ((Settings).Count -eq 0) 'upgrade: Settings was closed, no files in use'

    $unins = Get-ChildItem $dir -Filter 'unins*.exe' | Select-Object -First 1
    Run $unins.FullName '/VERYSILENT /SUPPRESSMSGBOXES'
    Start-Sleep 2
    Check (-not (Test-Path "$dir\statlet.exe")) 'uninstall: program files gone'
    Check ($null -eq (Get-ItemProperty $run -Name Statlet -ErrorAction SilentlyContinue).Statlet) 'uninstall: Run value gone'
    Check (-not (Test-Path $data)) 'uninstall: settings folder deleted (D3)'
}
finally {
    Get-Process statlet, StatletSettings -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dir*" } | Stop-Process -Force
    Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $data -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path "$backup\data") { New-Item -ItemType Directory $data -Force | Out-Null; Copy-Item "$backup\data\*" $data -Recurse -Force }
    if ($oldRun) { Set-ItemProperty $run -Name Statlet -Value $oldRun } else { Remove-ItemProperty $run -Name Statlet -ErrorAction SilentlyContinue }
    Remove-Item $backup -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failures) { Write-Host "`n$($failures.Count) check(s) failed" -ForegroundColor Red; exit 1 }
Write-Host "`nAll install checks passed. Your settings and autostart value are back as they were." -ForegroundColor Green
