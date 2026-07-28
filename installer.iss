; Inno Setup script for Replay Slider.
; Version/config are normally passed in via ISCC /D command-line defines
; from .github/scripts/Package-Windows.ps1; the values below are just
; fallbacks for a manual local run.
#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#ifndef ConfigDir
  #define ConfigDir "RelWithDebInfo"
#endif
#define MyAppName "Replay Slider"
#define MyAppPublisher "ilyambr"

[Setup]
; NOTE: The value of AppId uniquely identifies this application.
; Do not use the same AppId value in installers for other applications.
AppId={{CA1D94AF-4931-4719-9192-E307B75887E9}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppMutex={#MyAppName}
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} Setup
Compression=lzma2/ultra64
SolidCompression=yes
LZMAAlgorithm=1
DefaultDirName={code:GetDirName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir=release
OutputBaseFilename=replay-slider-{#MyAppVersion}-windows-x64
DirExistsWarning=no
WizardStyle=modern
WizardResizable=yes
SetupIconFile=media\icon.ico

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; The CMake build produces the modern self-contained plugin layout
; (replay-slider\bin\64bit\replay-slider.dll, replay-slider\data\*), but a
; system-wide "Program Files\obs-studio" install (like real OBS's own
; obs-plugins/data layout) expects the classic flat layout instead, so remap
; it here rather than dumping the nested folder as-is.
Source: "release\{#ConfigDir}\replay-slider\bin\64bit\replay-slider.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "release\{#ConfigDir}\replay-slider\bin\64bit\replay-slider.pdb"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion skipifsourcedoesntexist
Source: "release\{#ConfigDir}\replay-slider\data\*"; DestDir: "{app}\data\obs-plugins\replay-slider"; Flags: ignoreversion recursesubdirs createallsubdirs
; NOTE: Don't use "Flags: ignoreversion" on any shared system files

[Icons]
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"

[Code]
function GetDirName(Value: string): string;
var
  InstallPath: string;
begin
  // initialize default path, which will be returned when the following registry
  // key queries fail due to missing keys or for some different reason
  Result := ExpandConstant('{pf}\obs-studio');
  // query the first registry value; if this succeeds, return the obtained value
  if RegQueryStringValue(HKLM32, 'SOFTWARE\OBS Studio', '', InstallPath) then
    Result := InstallPath;
  if RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', InstallPath) then
    Result := InstallPath;
end;

/////////////////////////////////////////////////////////////////////
function GetUninstallString(): String;
var
  sUnInstPath: String;
  sUnInstallString: String;
begin
  sUnInstPath := ExpandConstant('Software\Microsoft\Windows\CurrentVersion\Uninstall\{#emit SetupSetting("AppId")}_is1');
  sUnInstallString := '';
  if not RegQueryStringValue(HKLM, sUnInstPath, 'UninstallString', sUnInstallString) then
    RegQueryStringValue(HKCU, sUnInstPath, 'UninstallString', sUnInstallString);
  Result := sUnInstallString;
end;


/////////////////////////////////////////////////////////////////////
function IsUpgrade(): Boolean;
begin
  Result := (GetUninstallString() <> '');
end;


/////////////////////////////////////////////////////////////////////
function UnInstallOldVersion(): Integer;
var
  sUnInstallString: String;
  iResultCode: Integer;
begin
// Return Values:
// 1 - uninstall string is empty
// 2 - error executing the UnInstallString
// 3 - successfully executed the UnInstallString

  // default return value
  Result := 0;

  // get the uninstall string of the old app
  sUnInstallString := GetUninstallString();
  if sUnInstallString <> '' then begin
    sUnInstallString := RemoveQuotes(sUnInstallString);
    if Exec(sUnInstallString, '/VERYSILENT /NORESTART /SUPPRESSMSGBOXES','', SW_HIDE, ewWaitUntilTerminated, iResultCode) then
      Result := 3
    else
      Result := 2;
  end else
    Result := 1;
end;

/////////////////////////////////////////////////////////////////////
function NextButtonClick(PageId: Integer): Boolean;
begin
    Result := True;
    if (PageId = wpSelectDir) and not FileExists(ExpandConstant('{app}\bin\64bit\obs64.exe')) then begin
        MsgBox('OBS Studio (bin\64bit\obs64.exe) does not seem to be installed in that folder.  Please select the correct folder.', mbError, MB_OK);
        Result := False;
        exit;
    end;
end;
