; Easnap per-user installer (Inno Setup 6).
;
; This file is UTF-8 with a byte order mark. Inno Setup reads a script without
; one in the system code page, which would break the Turkish text below.
;
; Built by scripts/package-release.ps1, which passes the versions and the
; already verified package directory:
;   ISCC /DAppVersion=1.1.0-beta.1 /DAppNumericVersion=1.1.0 /DSourceDir=<package>
;        /DOutputDir=<dir> packaging\easnap.iss
;
; The installer needs no administrator rights: it installs for the current
; user only, into %LOCALAPPDATA%\Programs\Easnap. Settings, the log, crash
; reports and temporary recordings live separately in %LOCALAPPDATA%\Easnap.
; Uninstalling asks whether settings, log and reports should go too. A
; recording that was not saved is never deleted by the uninstaller.

#ifndef AppVersion
  #error AppVersion must be defined (for example /DAppVersion=1.1.0-beta.1)
#endif
#ifndef AppNumericVersion
  #error AppNumericVersion must be defined (for example /DAppNumericVersion=1.1.0)
#endif
#ifndef SourceDir
  #error SourceDir must point at the verified package directory
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={{6F2B7C4E-0D53-4E0B-9C2A-7A7E5B1D4F31}
AppName=Easnap
AppVersion={#AppVersion}
AppVerName=Easnap {#AppVersion}
AppPublisher=Easnap contributors
AppPublisherURL=https://github.com/vseprr/easnap
AppSupportURL=https://github.com/vseprr/easnap/issues
AppUpdatesURL=https://github.com/vseprr/easnap/releases
VersionInfoVersion={#AppNumericVersion}.0
DefaultDirName={localappdata}\Programs\Easnap
DefaultGroupName=Easnap
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 version 2004. Earlier builds cannot keep Easnap's own windows out
; of a recording or a scrolling screenshot.
MinVersion=10.0.19041
LicenseFile={#SourceDir}\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename=Easnap-v{#AppVersion}-windows-x64-setup
SetupIconFile=..\apps\easnap-desktop\resources\easnap.ico
UninstallDisplayIcon={app}\Easnap.exe
UninstallDisplayName=Easnap
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Easnap is single-instance and holds this mutex while it runs. Setup and
; uninstall ask the user to exit it instead of replacing files in use.
AppMutex=Local\Easnap.SingleInstance
CloseApplications=no
; The "Open with" entries below: Explorer is told when setup and uninstall
; change them, so its list is current without signing out.
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "turkish"; MessagesFile: "compiler:Languages\Turkish.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; The application and its preview helper must come from the same build.
Source: "{#SourceDir}\bin\Easnap.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\bin\EasnapPreviewHost.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Registry]
; "Start Easnap when I sign in to Windows" in Settings writes this value. The
; installer never creates it; the uninstaller removes it.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "Easnap"; Flags: uninsdeletevalue dontcreatekey

; "Open with" for the videos Easnap opens (ADR-040), for the current user
; only. Easnap is offered in Explorer's "Open with" list and never made the
; default: no extension's default value is written and UserChoice is never
; touched, so installing changes no existing association. Uninstalling removes
; Easnap's own keys and its values under the extensions. The extensions are
; the support table's (src/media/video_support.cpp); the Fast test
; installer_open_with_list fails when the two lists differ. The portable ZIP
; writes none of this.
Root: HKCU; Subkey: "Software\Classes\Easnap.Video"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\Easnap.Video\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\Easnap.exe"",0"
Root: HKCU; Subkey: "Software\Classes\Easnap.Video\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "Easnap"
Root: HKCU; Subkey: "Software\Classes\Easnap.Video\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\Easnap.exe"" --open ""%1"""
Root: HKCU; Subkey: "Software\Classes\Applications\Easnap.exe"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "Easnap"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\Applications\Easnap.exe\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\Easnap.exe"",0"
Root: HKCU; Subkey: "Software\Classes\Applications\Easnap.exe\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\Easnap.exe"" --open ""%1"""
Root: HKCU; Subkey: "Software\Classes\Applications\Easnap.exe\SupportedTypes"; ValueType: string; ValueName: ".mp4"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\Applications\Easnap.exe\SupportedTypes"; ValueType: string; ValueName: ".m4v"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\Applications\Easnap.exe\SupportedTypes"; ValueType: string; ValueName: ".mov"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\Applications\Easnap.exe\SupportedTypes"; ValueType: string; ValueName: ".wmv"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\Applications\Easnap.exe\SupportedTypes"; ValueType: string; ValueName: ".asf"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\Applications\Easnap.exe\SupportedTypes"; ValueType: string; ValueName: ".avi"; ValueData: ""
; An extension key Easnap created is removed when nothing else is left in it.
Root: HKCU; Subkey: "Software\Classes\.mp4"; Flags: uninsdeletekeyifempty
Root: HKCU; Subkey: "Software\Classes\.mp4\OpenWithProgids"; ValueType: string; ValueName: "Easnap.Video"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKCU; Subkey: "Software\Classes\.m4v"; Flags: uninsdeletekeyifempty
Root: HKCU; Subkey: "Software\Classes\.m4v\OpenWithProgids"; ValueType: string; ValueName: "Easnap.Video"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKCU; Subkey: "Software\Classes\.mov"; Flags: uninsdeletekeyifempty
Root: HKCU; Subkey: "Software\Classes\.mov\OpenWithProgids"; ValueType: string; ValueName: "Easnap.Video"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKCU; Subkey: "Software\Classes\.wmv"; Flags: uninsdeletekeyifempty
Root: HKCU; Subkey: "Software\Classes\.wmv\OpenWithProgids"; ValueType: string; ValueName: "Easnap.Video"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKCU; Subkey: "Software\Classes\.asf"; Flags: uninsdeletekeyifempty
Root: HKCU; Subkey: "Software\Classes\.asf\OpenWithProgids"; ValueType: string; ValueName: "Easnap.Video"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKCU; Subkey: "Software\Classes\.avi"; Flags: uninsdeletekeyifempty
Root: HKCU; Subkey: "Software\Classes\.avi\OpenWithProgids"; ValueType: string; ValueName: "Easnap.Video"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty

[Icons]
Name: "{group}\Easnap"; Filename: "{app}\Easnap.exe"
Name: "{userdesktop}\Easnap"; Filename: "{app}\Easnap.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Easnap.exe"; Description: "{cm:LaunchProgram,Easnap}"; Flags: nowait postinstall skipifsilent

[Messages]
english.FinishedLabel=Easnap is installed. It runs in the notification area; press Ctrl+Shift+A to start a capture.
turkish.FinishedLabel=Easnap kuruldu. Bildirim alanında çalışır; yakalama başlatmak için Ctrl+Shift+A tuşlarına basın.

[CustomMessages]
english.RemoveData=Do you also want to remove Easnap's settings, log and crash reports from this computer?%n%nRecordings you have not saved are kept in any case.
turkish.RemoveData=Easnap'in ayarları, günlüğü ve hata raporları da bu bilgisayardan kaldırılsın mı?%n%nKaydetmediğiniz kayıtlar her durumda korunur.

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Data: String;
begin
  if CurUninstallStep <> usPostUninstall then
    exit;
  Data := ExpandConstant('{localappdata}\Easnap');
  if not DirExists(Data) then
    exit;
  { Asked only when a person is there to answer: a silent uninstall keeps everything. }
  if SuppressibleMsgBox(CustomMessage('RemoveData'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) = IDYES then
  begin
    DeleteFile(Data + '\settings.ini');
    DelTree(Data + '\logs', True, True, True);
    DelTree(Data + '\crashes', True, True, True);
  end;
  { RemoveDir removes empty directories only. The temporary folder stays as
    long as it holds a recording that was not saved. }
  RemoveDir(Data + '\temp');
  RemoveDir(Data);
end;
