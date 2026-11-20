; mtdrop setup program (Inno Setup 6). Built by installer\build-release.ps1, which passes
;   /DAppVersion=x.y.z /DStageDir=<out\release\stage>
; The machine-side steps (driver backup, local driver signature, service) are in setup.ps1 next to this file.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #error StageDir not defined (run installer\build-release.ps1)
#endif

; MT7961 firmware, downloaded at install time from openwrt/mt76 at a pinned commit (docs/FIRMWARE.md):
; the exact files every FINDINGS entry since F-007 used.
#define FwBase "https://raw.githubusercontent.com/openwrt/mt76/0936617dc529ff9da0385221d3fa6de732231177/firmware/"
#define FwPatch "WIFI_MT7961_patch_mcu_1_2_hdr.bin"
#define FwPatchSha "7085fc6ef8cd054e7ec29c9b4a4bf0ce34f43d479f8e7565c867cc5bb420ae5f"
#define FwRam "WIFI_RAM_CODE_MT7961_1.bin"
#define FwRamSha "91f2a24c3238668a6e6d5b6c7ce69c1c1ed0e20238ffe594be95279d0fe8792c"

[Setup]
AppId={{8D6B2F4A-3C1E-4F5B-9A7D-2E61C0B5F9A3}
AppName=mtdrop
AppVersion={#AppVersion}
AppVerName=mtdrop {#AppVersion} (alpha)
AppPublisher=mtdrop contributors
AppPublisherURL=https://github.com/jp10b/mtdrop
AppSupportURL=https://github.com/jp10b/mtdrop/issues
DefaultDirName={autopf}\mtdrop
DisableDirPage=yes
DefaultGroupName=mtdrop
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
InfoBeforeFile=before-install.txt
OutputBaseFilename=mtdrop-setup-{#AppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\app\mtdrop.exe
UninstallDisplayName=mtdrop
CloseApplications=no
UsedUserAreasWarning=no
VersionInfoVersion={#AppVersion}
VersionInfoDescription=mtdrop setup

[Tasks]
Name: autostart; Description: "Start mtdrop when I sign in (tray icon)"
Name: sendto; Description: "Add ""Send to > iPhone (mtdrop)"" to the Explorer menu"

[InstallDelete]
; files of an older install (also of a developer install made with scripts\install-service.ps1)
Type: filesandordirs; Name: "{app}\app"
Type: filesandordirs; Name: "{app}\driver"
Type: files; Name: "{app}\*.dll"
Type: files; Name: "{app}\*.json"
Type: files; Name: "{app}\*.exe"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Excludes: "\setup\*"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\setup\setup.ps1"; DestDir: "{app}\setup"; Flags: ignoreversion
Source: "{tmp}\{#FwPatch}"; DestDir: "{app}\firmware"; Flags: external ignoreversion
Source: "{tmp}\{#FwRam}"; DestDir: "{app}\firmware"; Flags: external ignoreversion

[Icons]
Name: "{autoprograms}\mtdrop"; Filename: "{app}\app\mtdrop.exe"; WorkingDir: "{app}\app"; Comment: "AirDrop-compatible receive and send"
Name: "{usersendto}\iPhone (mtdrop)"; Filename: "{app}\app\mtdrop.exe"; Parameters: "--send"; WorkingDir: "{app}\app"; Tasks: sendto

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "mtdrop"; ValueData: """{app}\app\mtdrop.exe"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\app\mtdrop.exe"; Description: "Start mtdrop now"; Flags: postinstall nowait skipifsilent runasoriginaluser

[Code]
var
  DownloadPage: TDownloadWizardPage;

function RunSetupScript(const Script, Mode: String; var Code: Integer): Boolean;
var
  Args: String;
begin
  Args := '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + Script + '" ' + Mode;
  { The app folder is not known yet when the check runs at start; setup.ps1 then uses its default. }
  if Mode <> '-Check' then
    Args := Args + ' -InstallDir "' + ExpandConstant('{app}') + '"';
  Result := Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Args, '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

function CheckMessage(Code: Integer): String;
begin
  case Code of
    0: Result := '';
    10: Result := 'No MediaTek MT7921 Wi-Fi card was found. mtdrop works only with this card for now.';
    11: Result := 'The Wi-Fi card is not on the MediaTek driver and no backup of that driver exists. Make sure Wi-Fi works normally, then run the setup again.';
    12: Result := 'This PC uses a MediaTek driver package mtdrop does not know yet. Please open an issue at https://github.com/jp10b/mtdrop/issues.';
  else
    Result := 'The setup check failed (code ' + IntToStr(Code) + '). Details: ' + ExpandConstant('{%TEMP}') + '\mtdrop-setup.log';
  end;
end;

function InitializeSetup(): Boolean;
var
  Code: Integer;
  Msg: String;
begin
  Result := True;
  ExtractTemporaryFile('setup.ps1');
  if not RunSetupScript(ExpandConstant('{tmp}\setup.ps1'), '-Check', Code) then
    Code := 1;
  Msg := CheckMessage(Code);
  if Msg <> '' then
  begin
    SuppressibleMsgBox(Msg, mbCriticalError, MB_OK, IDOK);
    Result := False;
  end;
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage('Downloading the Wi-Fi firmware',
    'mtdrop loads the MT7961 firmware from the openwrt/mt76 project; its SHA-256 is checked.', nil);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpReady then
  begin
    DownloadPage.Clear;
    DownloadPage.Add('{#FwBase}{#FwPatch}', '{#FwPatch}', '{#FwPatchSha}');
    DownloadPage.Add('{#FwBase}{#FwRam}', '{#FwRam}', '{#FwRamSha}');
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
      except
        if not DownloadPage.AbortedByUser then
          SuppressibleMsgBox('The firmware could not be downloaded: ' + GetExceptionMessage + #13#10#13#10 +
            'The setup needs internet access once. Nothing has been changed.', mbCriticalError, MB_OK, IDOK);
        Result := False;
      end;
    finally
      DownloadPage.Hide;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  { Stops a running mtdrop (the service gives the Wi-Fi card back first) before files are replaced. }
  if not RunSetupScript(ExpandConstant('{tmp}\setup.ps1'), '-Prepare', Code) then
    Code := 1;
  Result := CheckMessage(Code);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    WizardForm.StatusLabel.Caption := 'Backing up the MediaTek driver, signing the radio driver, starting the service...';
    if not RunSetupScript(ExpandConstant('{app}\setup\setup.ps1'), '-Install', Code) or (Code <> 0) then
      SuppressibleMsgBox('The last setup step failed (code ' + IntToStr(Code) + '). Details: ' +
        ExpandConstant('{%TEMP}') + '\mtdrop-setup.log' + #13#10#13#10 +
        'Your Wi-Fi was not changed. Uninstall mtdrop from Settings > Apps, or run the setup again.', mbCriticalError, MB_OK, IDOK);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Code: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    if not RunSetupScript(ExpandConstant('{app}\setup\setup.ps1'), '-Uninstall', Code) or (Code <> 0) then
      SuppressibleMsgBox('mtdrop could not confirm that the Wi-Fi card is back on the MediaTek driver (code ' + IntToStr(Code) +
        '). Details: ' + ExpandConstant('{%TEMP}') + '\mtdrop-setup.log' + #13#10 +
        'The driver backup in C:\ProgramData\mtdrop was kept. Restart the PC; if Wi-Fi is still missing, open an issue.',
        mbError, MB_OK, IDOK);
  end;
end;
