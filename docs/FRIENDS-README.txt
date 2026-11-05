STATLET (working name) - early test build
=========================================

A small "island" at the top-center of your screen. It shows the time, CPU and RAM,
and pops open for volume, battery, network, Focus timer and what music is playing.
Hover it to see more. Right-click its tray icon for Focus, Settings and Quit.

RUN IT
1. Right-click the zip > Extract All. Put the Statlet folder anywhere (Desktop is fine).
   Don't run it from inside the zip.
2. Open the Statlet folder, double-click statlet.exe.
3. Windows will say "Windows protected your PC" (the app is unsigned).
   Click "More info" > "Run anyway". Normal for a test build.
Nothing to install. Needs Windows 11, 64-bit (Intel/AMD).

SETTINGS
Tray icon (bottom-right, maybe under ^) > Settings. Or run settings\StatletSettings.exe.

QUIT / UNINSTALL
- Quit: tray icon > Quit Statlet.
- Uninstall: quit, delete the Statlet folder, then delete %LOCALAPPDATA%\Statlet
  (paste that into File Explorer's address bar). No other files or registry entries (autostart, if on, is one Run value).

KNOWN LIMITS
- Windows 11 only, x64 only.
- Autostart is off by default: turn it on in Settings > General, or launch it yourself.
- Unsigned, so SmartScreen warns.
- Tested on one laptop with one external monitor, so other setups may misbehave.
- Early build: rough edges expected.

FEEDBACK
Open an issue at https://github.com/ejaysobara/statlet/issues saying what you saw
and what you did. Helpful extras: Windows version, monitor setup,
and the log file: %LOCALAPPDATA%\Statlet\island.log (small text file, only holds app events
like "slid"/"hidden" - no personal data). Crash notes, if any, are in the same folder
(settings-crash.txt).

PRIVACY
It reads CPU/RAM, volume, battery, network status and the current media title/artist locally.
Nothing is sent anywhere. Your settings are saved in %LOCALAPPDATA%\Statlet\settings.json.

Font: Manrope (SIL Open Font License, see OFL.txt).
