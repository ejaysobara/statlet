# Builds a portable zip for friends: the island (statlet.exe) + the settings app, no installer, no dev tools needed to run.
#   .\package.ps1            -> dist\Statlet-friends-<date>.zip (x64 Windows 11)
# Needs (to BUILD only): Visual Studio C++ x64 tools + .NET 8 SDK. Runs build steps in this checkout, never touches %LOCALAPPDATA%.
# Runtime deps on the friend's PC: none to install. The island is built with /MT (C++ runtime inside the exe);
# the settings app is self-contained (.NET + Windows App SDK copied into settings\).
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$stage = Join-Path $root 'dist\Statlet'
$zip = Join-Path $root ("dist\Statlet-friends-{0}.zip" -f (Get-Date -Format 'yyyyMMdd'))
if (Test-Path "$root\dist") { Remove-Item "$root\dist" -Recurse -Force }
New-Item -ItemType Directory "$stage\settings" -Force | Out-Null

cmd /c "`"$root\island\build.cmd`""
if ($LASTEXITCODE -ne 0) { throw 'island build failed' }
dotnet publish "$root\settings\StatletSettings.csproj" -c Release -r win-x64 -p:DebugType=none -p:DebugSymbols=false "-p:PathMap=$root=src" -o "$stage\settings"
if ($LASTEXITCODE -ne 0) { throw 'settings publish failed' }

Copy-Item "$root\island\out\statlet.exe", "$root\island\out\Manrope.ttf" $stage
Copy-Item "$root\island\assets\OFL.txt" $stage
Copy-Item "$root\docs\FRIENDS-README.txt" "$stage\README.txt"
Get-ChildItem $stage -Recurse -Filter *.pdb | Remove-Item   # debug symbols embed the build machine's paths

Compress-Archive -Path $stage -DestinationPath $zip
"{0}  {1:N1} MB" -f $zip, ((Get-Item $zip).Length / 1MB)
