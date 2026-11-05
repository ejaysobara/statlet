# Builds the installer: dist\Statlet-Setup-<version>.exe and its .sha256. Needs Visual Studio C++ x64 tools, the .NET 10 SDK
# and Inno Setup 7 (ISCC.exe). Never touches %LOCALAPPDATA% or a running island (it builds into island\out\release).
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$version = (Get-Content "$root\VERSION" -TotalCount 1).Trim()
$stage = Join-Path $root 'dist\stage'
$iscc = @("$env:ProgramFiles\Inno Setup 7\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 7\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 7\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 7 (ISCC.exe) not found' }
if (Test-Path "$root\dist") { Remove-Item "$root\dist" -Recurse -Force }
New-Item -ItemType Directory "$stage\settings" -Force | Out-Null

$env:STATLET_OUT = 'out\release'
cmd /c "`"$root\island\build.cmd`""
if ($LASTEXITCODE -ne 0) { throw 'island build failed' }
dotnet publish "$root\settings\StatletSettings.csproj" -c Release -r win-x64 -p:DebugType=none -p:DebugSymbols=false "-p:PathMap=$root=src" -o "$stage\settings"
if ($LASTEXITCODE -ne 0) { throw 'settings publish failed' }

Copy-Item "$root\island\out\release\statlet.exe", "$root\island\out\release\Manrope.ttf", "$root\island\assets\OFL.txt" $stage
Get-ChildItem $stage -Recurse -Filter *.pdb | Remove-Item   # debug symbols embed the build machine's paths

& $iscc /Qp "/DAppVersion=$version" "/DStage=$stage" "$root\installer\statlet.iss"
if ($LASTEXITCODE -ne 0) { throw 'installer build failed' }
$setup = "$root\dist\Statlet-Setup-$version.exe"
$hash = (Get-FileHash $setup -Algorithm SHA256).Hash.ToLower()
"$hash  $(Split-Path $setup -Leaf)" | Set-Content "$setup.sha256" -Encoding ascii
"{0}  {1:N1} MB`nSHA256 {2}" -f $setup, ((Get-Item $setup).Length / 1MB), $hash
