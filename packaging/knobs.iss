; SPDX-License-Identifier: GPL-2.0-or-later
;
; The installer, built with Inno Setup 6 by packaging\package.ps1, which
; passes the defines below (and Sign, with a signtool command, to sign the
; uninstaller and the installer). It installs for the current user only, with no
; admin rights, into %LocalAppData%\Programs\knobs: knobs's Run entry and its
; data are per user too. Uninstalling closes knobs, removes its Run entry, and
; its RunOnce entry if one waits for a restart, and %LocalAppData%\knobs (the
; copy of OBS's files and the logs), and asks before removing %AppData%\knobs
; (the settings). A portable copy's entries are left alone.
;
; With no virtual cable on the PC, it offers to install VB-CABLE: VB-Audio's
; driver pack, which it carries as VB-Audio ships it (VbCablePack), and whose
; own setup it runs unattended, which asks for admin rights. Uninstalling knobs
; leaves VB-CABLE, which other apps may use.
;
; knobs runs it to update itself (src/main.cpp, StartInstaller), very silent
; with /update=1: then it starts knobs again as it ends, with --updated,
; whether the update worked or not, and --paused if knobs passed /paused=1.

#ifndef AppName
  #error Run packaging\package.ps1, which passes AppName, AppVersion, StageDir, OutputDir and VbCablePack.
#endif
#ifndef VbCablePack
  #error Run packaging\package.ps1, which passes VbCablePack, VB-Audio's driver pack.
#endif
#ifndef WizardGround
  #error Run packaging\package.ps1, which passes WizardGround, the color the wizard image was rendered on.
#endif
; This script is UTF-8 with no BOM, which Inno Setup reads as UTF-8 from 6.3.
#if Ver < EncodeVer(6, 3, 0)
  #error Inno Setup 6.3 or later is needed: this script is UTF-8 with no BOM.
#endif
#define VbCablePackName ExtractFileName(VbCablePack)

; Windows knows an install by its AppId. package.ps1 passes KnobsAppId for
; knobs's own, which every release has. Any other build, such as one with
; another AppName to test an uninstall, gets an AppId of its own, so it can't
; install over knobs or uninstall it.
#ifdef KnobsAppId
  #define AppId "{{1FE50EC5-3715-4EE5-BD51-381BF3374BA2}"
#else
  #define AppId AppName
#endif

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Daniel Alyoshin
AppPublisherURL=https://github.com/danielalyoshin/knobs
AppSupportURL=https://github.com/danielalyoshin/knobs/issues
AppUpdatesURL=https://github.com/danielalyoshin/knobs/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
DisableReadyPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1803, the first with tar, which unzips VB-Cable's pack.
MinVersion=10.0.17134
OutputDir={#OutputDir}
OutputBaseFilename={#AppName}-{#AppVersion}-setup
SetupIconFile=..\assets\knobs.ico
UninstallDisplayIcon={app}\{#AppName}.exe
UninstallDisplayName={#AppName}
WizardStyle=modern
; The stacked lockup, from tools\render-installer-images.ps1, at each display
; scale's size. Setup picks the one that fits, and centers it on its own color,
; the brand's light ground it was rendered on, rather than stretching it. The
; header's lockup is InitializeWizard's, so there's no small image.
WizardImageFile=..\assets\installer\wizard-202.png,..\assets\installer\wizard-269.png,..\assets\installer\wizard-336.png,..\assets\installer\wizard-403.png,..\assets\installer\wizard-430.png,..\assets\installer\wizard-498.png,..\assets\installer\wizard-534.png
WizardImageStretch=no
WizardImageBackColor={#WizardGround}
WizardSmallImageFile=
Compression=lzma2/max
SolidCompression=yes
; Setup closes a running knobs itself (PrepareToInstall).
CloseApplications=no
#ifdef Sign
; package.ps1 -Sign defines the signtool command.
SignTool=signtool
SignedUninstaller=yes
#endif

[Messages]
; The tasks page, which only VB-CABLE's task shows. The last two sentences
; are VB-Audio's own words, which its licence asks bundlers to show.
SelectTasksDesc=There’s no virtual cable on this PC yet.
SelectTasksLabel2={#AppName} sends your mic to a virtual cable, a sound device that exists only in software, which your other apps choose as their mic. Setup can install VB-CABLE, VB-Audio’s virtual cable, by running VB-Audio’s own installer in the background. Windows asks you to let it install its driver.%n%nThe origin of VB-CABLE: www.vb-cable.com. VB-CABLE is a donationware, all participations are welcome.
; Only VB-CABLE asks for a restart (NeedRestart), not knobs.
FinishedRestartLabel=To finish installing VB-CABLE, Windows has to restart. [name] opens again after the restart, to carry on setting up. Would you like to restart now?
FinishedRestartMessage=To finish installing VB-CABLE, Windows has to restart. [name] opens again after the restart, to carry on setting up.%n%nWould you like to restart now?

[Tasks]
Name: vbcable; Description: "Install VB-CABLE, from VB-Audio"; Check: OfferVbCable

[Files]
; The header's lockup, for InitializeWizard. First, so it unpacks without the
; rest.
Source: "..\assets\installer\header-*.png"; Flags: dontcopy noencryption
; VB-Audio's driver pack, as it ships, for InstallVbCable.
Source: "{#VbCablePack}"; Flags: dontcopy noencryption
Source: "{#StageDir}\{#AppName}.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\NOTICE.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppName}.exe"

[Run]
Filename: "{app}\{#AppName}.exe"; Parameters: "{code:StartParameters}"; Description: "Start {#AppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; The copy of OBS's files, the logs, and the dev tools' copies.
Type: filesandordirs; Name: "{localappdata}\{#AppName}"

[Code]
const
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  RunOnceKey = 'Software\Microsoft\Windows\CurrentVersion\RunOnce';
  ApprovedKey = 'Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run';
  // Windows' playback devices, and its installed apps, which every user can
  // read.
  RenderKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render';
  UninstallKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall';
  // A device's name, and its driver's: PKEY_Device_DeviceDesc and
  // PKEY_DeviceInterface_FriendlyName.
  DeviceNameValue = '{a45c254e-df1c-4efd-8020-67d146a850e0},2';
  DriverNameValue = '{b3f8fa53-0004-438e-9003-51a46e139bfc},6';
  // How long a cable gets to appear after VB-CABLE's setup closes.
  CableWaitMs = 15000;
  // The tray's window, which quits on WM_CLOSE (src/main.cpp).
  TrayWindowClass = '{#AppName}.tray';
  WM_CLOSE = $0010;
  // The header images' heights: header-<height>.png is for a header that
  // tall, one for each display scale (tools\render-installer-images.ps1).
  HeaderHeights = '58,77,97,116,124,143,159';

  // Opening HKLM's keys for reading, in the 64-bit registry.
  HklmRoot = $80000002;
  KeyRead64 = $20119;
  ErrorCancelled = 1223;

type
  TFileTimeRec = record
    Low: Cardinal;
    High: Cardinal;
  end;

var
  // Read once, as Setup starts: a virtual cable, and VB-CABLE's driver
  // waiting for a restart.
  HadCable, VbCableWaited: Boolean;
  // VB-CABLE's setup ran, as the task asked.
  VbCableRan: Boolean;
  // VB-CABLE's driver was installed, but its devices wait for Windows to
  // restart.
  VbCableNeedsRestart: Boolean;
  // The RunOnce entry that opens knobs after that restart couldn't be written.
  NoResume: Boolean;

function RegOpenKeyEx(Key: Cardinal; SubKey: String; Options, Sam: Cardinal; var Handle: Cardinal): Integer;
  external 'RegOpenKeyExW@advapi32.dll stdcall';
function RegQueryInfoKey(Key: Cardinal; ClassName, ClassLength, Reserved, SubKeys, MaxSubKey, MaxClass, Values,
  MaxValueName, MaxValue, Security: Cardinal; var LastWrite: TFileTimeRec): Integer;
  external 'RegQueryInfoKeyW@advapi32.dll stdcall';
function RegCloseKey(Key: Cardinal): Integer;
  external 'RegCloseKey@advapi32.dll stdcall';
procedure GetSystemTimeAsFileTime(var Time: TFileTimeRec);
  external 'GetSystemTimeAsFileTime@kernel32.dll stdcall';
function GetTickCount64(): Int64;
  external 'GetTickCount64@kernel32.dll stdcall';

// A FILETIME, in 100 ns since 1601.
function Ticks(const Time: TFileTimeRec): Int64;
begin
  Result := Int64(Time.High) * 4294967296 + Int64(Time.Low);
end;

// An update knobs started itself (StartInstaller in src/main.cpp).
function IsUpdate(): Boolean;
begin
  Result := ExpandConstant('{param:update|0}') = '1';
end;

// Whether a playback device is a virtual cable, as the tray recognizes one
// (IsVirtualCable in src/audio/audio_devices.cpp): VB-Audio's cables, and
// Virtual Audio Cable. Its device's name and its driver's, as Windows shows
// them together.
function IsVirtualCable(const Name: String): Boolean;
begin
  Result := (Pos('vb-audio', Lowercase(Name)) > 0) or (Pos('virtual audio cable', Lowercase(Name)) > 0);
end;

// Whether a virtual cable is installed: one that's on, or turned off in
// Windows' Sound settings, which the first run says how to turn on.
function CableInstalled(): Boolean;
var
  Devices: TArrayOfString;
  I: Integer;
  State: Cardinal;
  Name, Driver: String;
begin
  Result := False;
  if not RegGetSubkeyNames(HKLM64, RenderKey, Devices) then
    Exit;
  for I := 0 to GetArrayLength(Devices) - 1 do
  begin
    // DEVICE_STATE_ACTIVE or DEVICE_STATE_DISABLED, in the low bits.
    if RegQueryDWordValue(HKLM64, RenderKey + '\' + Devices[I], 'DeviceState', State) and
       (((State and $F) = 1) or ((State and $F) = 2)) then
    begin
      Name := '';
      Driver := '';
      RegQueryStringValue(HKLM64, RenderKey + '\' + Devices[I] + '\Properties', DeviceNameValue, Name);
      RegQueryStringValue(HKLM64, RenderKey + '\' + Devices[I] + '\Properties', DriverNameValue, Driver);
      if IsVirtualCable(Name + ' (' + Driver + ')') then
      begin
        Result := True;
        Exit;
      end;
    end;
  end;
end;

// Whether VB-CABLE's setup installed its driver since Windows last started,
// when its devices may wait for a restart. It adds itself to the installed
// apps as "VBCABLE, The Virtual Audio Cable", and its key's time says when.
// Installed before, with no devices, it's as none: a restart won't bring
// them back, and installing it again does (VbCablePending in
// src/audio/device_watch.cpp).
function VbCablePending(): Boolean;
var
  Apps: TArrayOfString;
  I: Integer;
  Name: String;
  Key: Cardinal;
  Written, Now: TFileTimeRec;
  Started: Int64;
begin
  Result := False;
  if not RegGetSubkeyNames(HKLM64, UninstallKey, Apps) then
    Exit;
  GetSystemTimeAsFileTime(Now);
  Started := Ticks(Now) - GetTickCount64() * 10000;
  for I := 0 to GetArrayLength(Apps) - 1 do
    if RegQueryStringValue(HKLM64, UninstallKey + '\' + Apps[I], 'DisplayName', Name) and
       (Pos('vbcable', Lowercase(Name)) = 1) and
       (RegOpenKeyEx(HklmRoot, UninstallKey + '\' + Apps[I], 0, KeyRead64, Key) = 0) then
    begin
      if RegQueryInfoKey(Key, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, Written) = 0 then
        Result := Ticks(Written) > Started;
      RegCloseKey(Key);
      if Result then
        Exit;
    end;
end;

function InitializeSetup(): Boolean;
begin
  HadCable := CableInstalled();
  VbCableWaited := VbCablePending();
  Result := True;
end;

// VB-CABLE's task, which an update or any other silent install never runs:
// its setup asks for admin rights, and VB-Audio's licensing wants users to see
// VB-CABLE named on the tasks page. Not with a cable, nor with VB-CABLE
// installed but waiting for a restart, which knobs's first run says.
function OfferVbCable(): Boolean;
begin
  Result := not WizardSilent() and not IsUpdate() and not HadCable and not VbCableWaited;
end;

// After VB-CABLE's setup ran, knobs opens its first run at the cable, even if
// it was finished, to choose the new one (--resume-setup).
function StartParameters(Param: String): String;
begin
  Result := '';
  if VbCableRan then
    Result := '--resume-setup';
end;

// Opens knobs again at the next sign-in, to carry on with its first run
// (knobs.exe --resume-setup, which knobs's first run writes too). If the entry
// can't be written, the user hears to open knobs themselves.
procedure ResumeAfterRestart();
begin
  if not RegWriteStringValue(HKCU, RunOnceKey, '{#AppName}',
                             '"' + ExpandConstant('{app}\{#AppName}.exe') + '" --resume-setup') then
  begin
    Log('Couldn''t write the RunOnce entry that opens {#AppName} after the restart.');
    NoResume := True;
    SuppressibleMsgBox('{#AppName} can’t open by itself after the restart, so open it once Windows has ' +
                       'restarted, to finish setting up VB-CABLE.', mbInformation, MB_OK, IDOK);
  end;
end;

// VB-CABLE wasn't installed, and `Why`: says so, and that knobs shows how to
// install it by hand.
procedure VbCableNotInstalled(const Why: String);
begin
  Log('VB-CABLE wasn''t installed: ' + Why);
  SuppressibleMsgBox('VB-CABLE wasn’t installed: ' + Why + #13#10#13#10 + 'When {#AppName} starts, it shows how ' +
                     'to install it, step by step.', mbInformation, MB_OK, IDOK);
end;

// Unzips VB-Audio's driver pack and runs its setup, which asks for admin
// rights, unattended: -i installs and -h hides its window, so there's no
// Install Driver to click, no restart message and no website (VB-Audio's
// forum gives the switches; its licensing allows a silent install that the
// user can tell is VB-CABLE, which the tasks page says). Then waits for the
// cable.
// If VB-CABLE's driver was installed but its devices don't appear, Windows
// has to restart, which Setup then asks for. If it wasn't, as when Windows'
// prompt was declined, knobs's first run shows how to install it by hand.
procedure InstallVbCable();
var
  Folder, Pack: String;
  Code, Waited: Integer;
begin
  Folder := ExpandConstant('{tmp}\vb-cable');
  ForceDirectories(Folder);
  ExtractTemporaryFile('{#VbCablePackName}');
  Pack := ExpandConstant('{tmp}\{#VbCablePackName}');
  WizardForm.StatusLabel.Caption := 'Installing VB-CABLE, VB-Audio’s virtual cable.';
  WizardForm.FilenameLabel.Caption := 'Windows asks you to let it install its driver.';
  VbCableRan := True;
  // tar, which ships with Windows, unzips it.
  if not Exec(ExpandConstant('{sys}\tar.exe'), '-xf "' + Pack + '" -C "' + Folder + '"', Folder, SW_HIDE,
              ewWaitUntilTerminated, Code) then
  begin
    VbCableNotInstalled('its driver pack couldn’t be unpacked (' + SysErrorMessage(Code) + ').');
    Exit;
  end;
  if (Code <> 0) or not FileExists(Folder + '\VBCABLE_Setup_x64.exe') then
  begin
    VbCableNotInstalled(Format('its driver pack couldn’t be unpacked (error %d).', [Code]));
    Exit;
  end;
  if not ShellExec('runas', Folder + '\VBCABLE_Setup_x64.exe', '-i -h', Folder, SW_SHOWNORMAL, ewWaitUntilTerminated,
                   Code) then
  begin
    if Code = ErrorCancelled then
      VbCableNotInstalled('Windows asked to let it install, and the answer was No.')
    else
      VbCableNotInstalled('its installer couldn’t start (' + SysErrorMessage(Code) + ').');
    Exit;
  end;
  // Hidden, its setup says nothing of how it went: its exit code is logged.
  Log(Format('VB-CABLE''s setup ended with exit code %d.', [Code]));
  // Its devices take a moment to appear.
  Waited := 0;
  while not CableInstalled() and VbCablePending() and (Waited < CableWaitMs) do
  begin
    Sleep(500);
    Waited := Waited + 500;
  end;
  if CableInstalled() then
    Exit;
  // knobs opens after the restart once its files are in (ssPostInstall).
  // A restart helps only a driver that installed, which its setup says with
  // 0, or with Windows' codes for a restart needed or started.
  if VbCablePending() and ((Code = 0) or (Code = 3010) or (Code = 1641)) then
    VbCableNeedsRestart := True
  else if Code <> 0 then
    VbCableNotInstalled(Format('its installer ended with error %d.', [Code]))
  else
    VbCableNotInstalled('its installer ended without installing its driver.');
end;

function NeedRestart(): Boolean;
begin
  Result := VbCableNeedsRestart;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  // As installing starts: Setup asks NeedRestart once the files are in,
  // before ssPostInstall.
  if (CurStep = ssInstall) and WizardIsTaskSelected('vbcable') then
    InstallVbCable();
  // Not before: if Setup fails or is cancelled, nothing would open.
  if (CurStep = ssPostInstall) and VbCableNeedsRestart then
    ResumeAfterRestart();
  // A restart that waits for later skips the Start check box, so knobs starts
  // now, at its first run's page that says to restart, and again after it.
  if (CurStep = ssDone) and VbCableNeedsRestart and not WizardForm.YesRadio.Checked then
    ExecAsOriginalUser(ExpandConstant('{app}\{#AppName}.exe'), '--resume-setup', ExpandConstant('{app}'),
                       SW_SHOWNORMAL, ewNoWait, Code);
end;

// The finish page says knobs opens again after the restart (FinishedRestartLabel),
// unless its RunOnce entry couldn't be written.
procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and NoResume then
    WizardForm.FinishedLabel.Caption := 'To finish installing VB-CABLE, Windows has to restart. Open {#AppName} ' +
                                       'after the restart, to carry on setting up. Would you like to restart now?';
end;

// After an update, knobs starts again: the new copy, or this one if the
// update failed, which says so. Paused if the copy that started the update
// was (/paused=1).
procedure DeinitializeSetup();
var
  App, Parameters: String;
  Code: Integer;
begin
  if not IsUpdate() then
    Exit;
  try
    App := ExpandConstant('{app}');
  except
    Exit;
  end;
  Parameters := '--updated';
  if ExpandConstant('{param:paused|0}') = '1' then
    Parameters := Parameters + ' --paused';
  if FileExists(App + '\{#AppName}.exe') then
    Exec(App + '\{#AppName}.exe', Parameters, App, SW_SHOWNORMAL, ewNoWait, Code);
end;

// Shows the lockup in the header, where Setup's small image goes. Setup keeps
// that square, and would pick a WizardSmallImageFile by its area, the wrong
// one for a wide image, so it's put there here: the smallest header image at
// least as tall as the header (the tallest, scaled, if none is), with the
// square widened leftward to fit it and the page's name and description
// narrowed to match.
procedure InitializeWizard();
var
  Image: TBitmapImage;
  Heights: TArrayOfString;
  Name: String;
  I, Grow: Integer;
begin
  Image := WizardForm.WizardSmallBitmapImage;
  Heights := StringSplit(HeaderHeights, [','], stExcludeEmpty);
  I := 0;
  while (I < GetArrayLength(Heights) - 1) and (StrToInt(Heights[I]) < Image.Height) do
    I := I + 1;
  Name := 'header-' + Heights[I] + '.png';
  ExtractTemporaryFile(Name);
  Image.PngImage.LoadFromFile(ExpandConstant('{tmp}\') + Name);
  Image.Stretch := True;
  Grow := Image.PngImage.Width * Image.Height div Image.PngImage.Height - Image.Width;
  Image.Left := Image.Left - Grow;
  Image.Width := Image.Width + Grow;
  WizardForm.PageNameLabel.Width := WizardForm.PageNameLabel.Width - Grow;
  WizardForm.PageDescriptionLabel.Width := WizardForm.PageDescriptionLabel.Width - Grow;
end;

// The uninstaller shows the Windows icon in its header, as tall as the lockup is
// in the installer's. This moves it left to the lockup's margin, 24/58 of the
// header's height (tools\render-installer-images.ps1), so their right edges
// line up.
procedure InitializeUninstallProgressForm();
var
  Image: TBitmapImage;
  Shift: Integer;
begin
  Image := UninstallProgressForm.WizardSmallBitmapImage;
  Shift := Image.Left + Image.Width - (UninstallProgressForm.MainPanel.ClientWidth -
           MulDiv(UninstallProgressForm.MainPanel.Height, 24, 58));
  if Shift > 0 then
  begin
    Image.Left := Image.Left - Shift;
    UninstallProgressForm.PageNameLabel.Width := UninstallProgressForm.PageNameLabel.Width - Shift;
    UninstallProgressForm.PageDescriptionLabel.Width := UninstallProgressForm.PageDescriptionLabel.Width - Shift;
  end;
end;

// Asks a running knobs to quit, and waits up to 15 s for it. False if it's
// still running.
function CloseRunningApp(): Boolean;
var
  Window: HWND;
  Waited: Integer;
begin
  Window := FindWindowByClassName(TrayWindowClass);
  if Window <> 0 then
    PostMessage(Window, WM_CLOSE, 0, 0);
  Waited := 0;
  while (FindWindowByClassName(TrayWindowClass) <> 0) and (Waited < 15000) do
  begin
    Sleep(250);
    Waited := Waited + 250;
  end;
  Result := FindWindowByClassName(TrayWindowClass) = 0;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not CloseRunningApp() then
    Result := '{#AppName} is still running. Quit it from its tray menu, then run Setup again.';
end;

function InitializeUninstall(): Boolean;
begin
  Result := CloseRunningApp();
  if not Result then
    MsgBox('{#AppName} is still running. Quit it from its tray menu, then uninstall again.', mbError, MB_OK);
end;

// Whether a value under Key starts this install's knobs.exe, not a portable
// copy's.
function StartsThisApp(const Key: String): Boolean;
var
  Command: String;
begin
  Result := RegQueryStringValue(HKCU, Key, '{#AppName}', Command) and
            (Pos(Lowercase(ExpandConstant('{app}\{#AppName}.exe')), Lowercase(Command)) > 0);
end;

// Removes the Run entry, and the RunOnce entry that opens knobs after a
// restart, if they're this install's.
procedure RemoveRunEntries();
begin
  if StartsThisApp(RunKey) then
  begin
    RegDeleteValue(HKCU, RunKey, '{#AppName}');
    RegDeleteValue(HKCU, ApprovedKey, '{#AppName}');
  end;
  if StartsThisApp(RunOnceKey) then
    RegDeleteValue(HKCU, RunOnceKey, '{#AppName}');
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Settings: String;
begin
  if CurUninstallStep = usUninstall then
    RemoveRunEntries();
  if CurUninstallStep = usPostUninstall then
  begin
    Settings := ExpandConstant('{userappdata}\{#AppName}');
    if DirExists(Settings) and not UninstallSilent() and
       (MsgBox('Also delete your {#AppName} settings?' + #13#10#13#10 +
               'They’re your choice of mic and cable, whether {#AppName} pauses while OBS is open, and ' +
               'whether it checks for new versions. ' +
               'Keep them if you might install {#AppName} again.',
               mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES) then
      DelTree(Settings, True, True, True);
  end;
end;
