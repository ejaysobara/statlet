## Statlet 1.0.0

A Dynamic Island-style floating status pill for Windows 11. Shows time, CPU and RAM; opens for volume, battery,
network, a Focus timer and now-playing music; includes a settings app.

**New in 1.0.0**
- A real installer (`Statlet-Setup-1.0.0.exe`): per-user, no administrator needed, updates in place and keeps your settings.
- Starting Statlet again (Start menu) opens Settings instead of doing nothing.
- Start with Windows is off by default, with a note in Settings and a one-time question the first time you close Settings.
  Statlet also notices when Windows has switched it off in Startup Apps.
- Settings > About: version, install date, and a Releases page button.
- The settings app now runs on .NET 10.

**Install:** run the setup. Windows shows "Windows protected your PC" because the setup is unsigned: More info > Run anyway.
Check the download with `Get-FileHash` against the SHA256 below.

**Updates:** Statlet never goes online. On GitHub choose Watch > Custom > Releases to be told about new versions.

**Known limits:** Windows 11 x64 only. Unsigned. Tested on one laptop plus one external monitor.

**Feedback:** open an issue. Attach `%LOCALAPPDATA%\Statlet\island.log` if something went wrong.

SHA256 of `Statlet-Setup-1.0.0.exe`: d28fe566a1b384873b9647a8ab717f45e0181078ce33cfa7b74c239658ec3401
