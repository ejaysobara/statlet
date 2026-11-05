# One command for the whole gate: build the island and the settings app, run their tests, fail on any warning.
#   .\build.ps1                 everything
#   .\build.ps1 -Ci             skip tests that need a desktop (screen, e2e)
#   .\build.ps1 -Publish        also publish the settings app to island\out\settings
param([switch]$Ci, [switch]$Publish)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$summary = @()

function Step($name, [scriptblock]$run) {
    Write-Host "== $name" -ForegroundColor Cyan
    & $run
    if ($LASTEXITCODE -ne 0) { Write-Host "FAILED: $name (exit $LASTEXITCODE)" -ForegroundColor Red; exit 1 }
}

# Island: build.cmd uses /WX, so a warning is already a failed build; a clean build means 0 warnings.
$out = Join-Path $root 'island\out'
if (Get-Process statlet -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $out 'statlet.exe') }) {
    $env:STATLET_OUT = 'out\gate'   # out\statlet.exe is running; build beside it
    $out = Join-Path $root 'island\out\gate'
}
Step 'island build' { cmd /c "`"$root\island\build.cmd`"" }
$summary += 'island 0 warnings'

Step 'island tests' { Push-Location "$root\island"; try { & "$out\statlet-tests.exe" @(if ($Ci) { '--ci' }) } finally { Pop-Location } }

# Settings app: count warnings from the compiler output, any warning fails the gate.
Write-Host '== settings build' -ForegroundColor Cyan
$log = dotnet build "$root\settings\StatletSettings.csproj" -c Release -r win-x64 --no-incremental 2>&1
$log | Where-Object { $_ -match ': (warning|error) ' } | Sort-Object -Unique | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { Write-Host 'FAILED: settings build' -ForegroundColor Red; exit 1 }
$warnings = @($log | Where-Object { $_ -match ': warning ' } | Sort-Object -Unique).Count
if ($warnings -gt 0) { Write-Host "FAILED: settings has $warnings warning(s)" -ForegroundColor Red; exit 1 }
$summary += 'settings 0 warnings'

Step 'settings tests' { dotnet test "$root\settings.tests" -c Release -warnaserror }
$summary += 'settings tests pass'

if ($Publish) {
    Step 'settings publish' { dotnet publish "$root\settings\StatletSettings.csproj" -c Release -r win-x64 -o "$root\island\out\settings" }
}

Write-Host ($summary -join ', ') -ForegroundColor Green
exit 0
