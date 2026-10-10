; Easnap per-user installer (Inno Setup 6).
;
; Built by scripts/package-release.ps1, which passes the version and the
; already verified package directory:
;   ISCC /DAppVersion=1.0.1 /DSourceDir=<package> /DOutputDir=<dir> packaging\easnap.iss
;
; The installer needs no administrator rights: it installs for the current
; user only, into %LOCALAPPDATA%\Programs\Easnap. Settings and temporary
; recordings live separately in %LOCALAPPDATA%\Easnap and are left in place
; on uninstall so a recoverable recording is never deleted silently.

#ifndef AppVersion
  #error AppVersion must be defined (for example /DAppVersion=1.0.1)
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
VersionInfoVersion={#AppVersion}.0
DefaultDirName={localappdata}\Programs\Easnap
DefaultGroupName=Easnap
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
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

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; The application and its preview helper must come from the same build.
Source: "{#SourceDir}\bin\Easnap.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\bin\EasnapPreviewHost.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\Easnap"; Filename: "{app}\Easnap.exe"
Name: "{userdesktop}\Easnap"; Filename: "{app}\Easnap.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Easnap.exe"; Description: "{cm:LaunchProgram,Easnap}"; Flags: nowait postinstall skipifsilent

[Messages]
english.FinishedLabel=Easnap is installed. It runs in the notification area; press Ctrl+Shift+A to start a capture.
