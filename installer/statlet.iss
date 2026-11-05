; Statlet installer (Inno Setup 7). Built by package.ps1, which passes /DAppVersion (from VERSION) and /DStage (the files).
; Per-user: no admin prompt, installs to %LOCALAPPDATA%\Programs\Statlet. The fixed AppId makes a newer setup install over an older one.
#ifndef AppVersion
  #error Build with package.ps1 (it passes /DAppVersion and /DStage).
#endif

[Setup]
AppId={{6E1B7C52-3A0D-4F0B-9B83-5D6C1E2A7F41}
AppName=Statlet
AppVersion={#AppVersion}
AppPublisher=Statlet
DefaultDirName={autopf}\Statlet
DefaultGroupName=Statlet
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.22000
SetupIconFile=..\island\src\statlet.ico
UninstallDisplayIcon={app}\statlet.exe
OutputDir=..\dist
OutputBaseFilename=Statlet-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; Restart Manager can miss the island (hidden window, tray icon), so CloseRunning below asks both apps to close first.
CloseApplications=yes
RestartApplications=no

[Files]
Source: "{#Stage}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\Statlet"; Filename: "{app}\statlet.exe"

[Tasks]
; Unticked on purpose. The one line says why you would tick it.
Name: "autostart"; Description: "Start Statlet when I sign in. It only appears while it is running, so tick this to have it back after a restart."; Flags: unchecked

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Statlet"; ValueData: """{app}\statlet.exe"""; Tasks: autostart; Flags: uninsdeletevalue

[Run]
; Not skipifsilent: a silent upgrade closed the island, so it must come back.
Filename: "{app}\statlet.exe"; Description: "Start Statlet"; Flags: nowait runasoriginaluser

[UninstallDelete]
; Only an explicit uninstall deletes settings and the log; installing over an older version never does.
Type: filesandordirs; Name: "{localappdata}\Statlet"

[Code]
const WM_CLOSE = $0010;
function FindWindowW(cls, title: String): LongInt; external 'FindWindowW@user32.dll stdcall';
function PostMessageW(h: LongInt; msg, wp, lp: LongInt): Boolean; external 'PostMessageW@user32.dll stdcall';

function IslandWindow: LongInt; begin Result := FindWindowW('StatletIsland', ''); end;
function SettingsWindow: LongInt; begin Result := FindWindowW('', 'Statlet Settings'); end;

// Asks the island to close and ends Settings (a polite close would show its one-time question), then waits up to 8 s.
procedure CloseRunning;
var i, code: Integer;
begin
  if SettingsWindow <> 0 then Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM StatletSettings.exe', '', SW_HIDE, ewWaitUntilTerminated, code);
  if IslandWindow <> 0 then PostMessageW(IslandWindow, WM_CLOSE, 0, 0);
  for i := 1 to 40 do
  begin
    if (IslandWindow = 0) and (SettingsWindow = 0) then Break;
    Sleep(200);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  CloseRunning;
  Result := '';
end;

// A Run value from the old zip build points at the old folder: aim it at the installed copy.
procedure CurStepChanged(CurStep: TSetupStep);
var v: String;
begin
  if (CurStep = ssPostInstall) and
     RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Statlet', v) then
    RegWriteStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Statlet', '"' + ExpandConstant('{app}\statlet.exe') + '"');
end;

function InitializeUninstall: Boolean;
begin
  CloseRunning;
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Statlet');
end;
