@echo off
rem Builds statlet.exe and statlet-tests.exe with MSVC (found by vswhere) and the Windows SDK's C++/WinRT.
rem Output goes to out\, or to the folder in STATLET_OUT (relative to this folder), e.g. while out\statlet.exe is running.
setlocal
cd /d "%~dp0"
if not defined VCVARS for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set "VCVARS=%%i"
if not defined VCVARS echo No Visual Studio with the C++ x64 tools found (vswhere). & exit /b 1
rem vcvars64.bat runs vswhere itself and prints an error when the installer folder is not on PATH.
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VCVARS%" >nul || exit /b 1
set OUT=out
if defined STATLET_OUT set "OUT=%STATLET_OUT%"
if not exist "%OUT%" mkdir "%OUT%"
rem /external:* keeps Windows SDK and C++/WinRT headers out of /W4 /WX; our own code gets the full warning level.
set FLAGS=/nologo /std:c++20 /EHsc /O2 /W4 /WX /external:anglebrackets /external:W0 /permissive- /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS
set LIBS=d3d11.lib dxgi.lib d2d1.lib dwrite.lib dwmapi.lib dxguid.lib user32.lib gdi32.lib shell32.lib shcore.lib ole32.lib windowscodecs.lib windowsapp.lib CoreMessaging.lib iphlpapi.lib advapi32.lib wtsapi32.lib
rc /nologo /fo "%OUT%\statlet.res" src\statlet.rc || exit /b 1
cl %FLAGS% /Fo"%OUT%\\" src\gfx.cpp src\pieces.cpp src\events.cpp src\media.cpp src\main.cpp /Fe"%OUT%\statlet.exe" /link /WX "%OUT%\statlet.res" /SUBSYSTEM:WINDOWS %LIBS% || exit /b 1
cl %FLAGS% /Fo"%OUT%\\" tests\tests.cpp "%OUT%\gfx.obj" "%OUT%\pieces.obj" /Fe"%OUT%\statlet-tests.exe" /link /WX /SUBSYSTEM:CONSOLE %LIBS% || exit /b 1
copy /y assets\Manrope.ttf "%OUT%\" >nul
