#define MyAppName "WebMiere D3D11VA Community Fork"
#define MyAppVersion "1.3.0"
#define MyAppPublisher "moesuito"
#define MyAppURL "https://github.com/moesuito/webmiere-d3d11va"
#define MyOutputName "WebMiere-D3D11VA-Setup-1.3.0"

#ifndef PayloadDir
  #define PayloadDir "payload"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

[Setup]
AppId={{7C9A18D2-6132-47DA-985F-EBFA83B09F41}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases/latest
DefaultDirName={commonpf64}\Adobe\Common\Plug-ins\7.0\MediaCore\WebMiere
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename={#MyOutputName}
Compression=bzip
SolidCompression=no
WizardStyle=modern
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
LicenseFile={#PayloadDir}\assets\licenses\WebMiere-MPL-2.0.txt
InfoBeforeFile=FORK_NOTICE.txt
UninstallDisplayName={#MyAppName}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription=Installer for the unofficial WebMiere D3D11VA community fork
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoVersion=1.3.0.1

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Code]
function PremiereIsRunning: Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(
    ExpandConstant('{cmd}'),
    '/C tasklist /FI "IMAGENAME eq Adobe Premiere Pro.exe" /NH | find /I "Adobe Premiere Pro.exe" >NUL',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

function InitializeSetup: Boolean;
begin
  Result := not PremiereIsRunning;
  if not Result then
    MsgBox('Close Adobe Premiere Pro before installing WebMiere.', mbError, MB_OK);
end;

function InitializeUninstall: Boolean;
begin
  Result := not PremiereIsRunning;
  if not Result then
    MsgBox('Close Adobe Premiere Pro before uninstalling WebMiere.', mbError, MB_OK);
end;
