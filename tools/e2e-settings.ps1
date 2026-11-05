# Opens the settings app twice and checks that only one copy runs (single-instance guard in settings\Program.cs).
# Needs a desktop, so it is not part of build.ps1 -Ci. It publishes to a temp folder and never touches your installed
# copy or %LOCALAPPDATA%\Statlet\settings.json (nothing is clicked, and the app only writes the file on a change).
#   .\tools\e2e-settings.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$dir = Join-Path ([IO.Path]::GetTempPath()) "statlet-e2e-$PID"
dotnet publish "$root\settings\StatletSettings.csproj" -c Release -r win-x64 -o "$dir\settings" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'publish failed' }
$exe = "$dir\settings\StatletSettings.exe"
function Mine { @(Get-Process StatletSettings -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }) }
try {
    1..2 | ForEach-Object { Start-Process explorer.exe "`"$exe`""; Start-Sleep 6 }  # via explorer, as the island's tray menu would
    $n = (Mine).Count
    if ($n -ne 1) { Write-Host "FAILED: $n copies of Settings running, expected 1" -ForegroundColor Red; exit 1 }
    Write-Host 'single instance ok' -ForegroundColor Green
} finally {
    Mine | Stop-Process -Force
    Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
}
