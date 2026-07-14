; TcpRedirector v1.1.0 — Inno Setup Installer
; Run with: iscc TcpRedirectorSetup.iss

#define AppName "TcpRedirector"
#define AppVersion "1.1.0"
#define AppPublisher "TcpRedirector"
#define AppURL "https://github.com/tcpredirector"
#define AppExeName "TcpRedirectorGUI.exe"
#define ServiceExeName "TcpRedirectorService.exe"
#define AuthAgentExeName "TcpRedirectorAuthAgent.exe"

[Setup]
AppId={{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
; AppURL not supported in [Setup] — removed
DefaultDirName={pf}\{#AppName}
DefaultGroupName={#AppName}
OutputDir=..\release\{#AppVersion}\installer
OutputBaseFilename=TcpRedirectorSetup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#AppName} {#AppVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Types]
Name: "full"; Description: "Full installation"
Name: "compact"; Description: "Compact installation"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "service"; Description: "TcpRedirector Service"; Types: full compact custom; Flags: fixed
Name: "gui"; Description: "TcpRedirector GUI"; Types: full compact custom
Name: "authagent"; Description: "AuthAgent (Kerberos)"; Types: full custom
Name: "windivert"; Description: "WinDivert Driver"; Types: full custom

[Files]
; Service
Source: "..\release\{#AppVersion}\bin\{#ServiceExeName}"; DestDir: "{app}"; Components: service; Flags: ignoreversion
Source: "..\release\{#AppVersion}\bin\*.dll"; DestDir: "{app}"; Components: service; Flags: ignoreversion

; GUI (full self-contained directory)
Source: "..\release\{#AppVersion}\bin\gui\*"; DestDir: "{app}\gui"; Components: gui; Flags: ignoreversion recursesubdirs createallsubdirs

; AuthAgent (optional — build separately with CMake)
; Source: "..\release\{#AppVersion}\bin\{#AuthAgentExeName}"; DestDir: "{app}"; Components: authagent; Flags: ignoreversion

; WinDivert
Source: "..\release\{#AppVersion}\bin\WinDivert.dll"; DestDir: "{app}"; Components: windivert; Flags: ignoreversion
Source: "..\release\{#AppVersion}\bin\WinDivert64.sys"; DestDir: "{app}"; Components: windivert; Flags: ignoreversion
; WinDivertInstall.exe — install separately from https://www.reqrypt.org/windivert.html
; Source: "..\release\{#AppVersion}\bin\WinDivertInstall.exe"; DestDir: "{app}"; Components: windivert; Flags: ignoreversion
Source: "..\release\{#AppVersion}\bin\install_windivert.bat"; DestDir: "{app}"; Components: windivert; Flags: ignoreversion

; Task Scheduler definition
Source: "TcpRedirectorAuthAgent.xml"; DestDir: "{app}"; Components: authagent; Flags: ignoreversion

[Dirs]
Name: "{commonappdata}\{#AppName}"
Name: "{commonappdata}\{#AppName}\logs"

[Run]
; Install WinDivert driver
Filename: "{app}\install_windivert.bat"; WorkingDir: "{app}"; Components: windivert; Flags: runhidden

; Install and start the service
Filename: "{app}\{#ServiceExeName}"; Parameters: "--install"; WorkingDir: "{app}"; Components: service; Flags: runhidden
Filename: "net"; Parameters: "start TcpRedirectorService"; Components: service; Flags: runhidden

; Register AuthAgent in Task Scheduler (when built)
; Filename: "schtasks"; Parameters: "/create /tn ""TcpRedirectorAuthAgent"" /xml ""{app}\TcpRedirectorAuthAgent.xml"" /f"; Components: authagent; Flags: runhidden

[UninstallRun]
; Stop and uninstall the service
Filename: "net"; Parameters: "stop TcpRedirectorService"; Flags: runhidden
Filename: "{app}\{#ServiceExeName}"; Parameters: "--uninstall"; WorkingDir: "{app}"; Flags: runhidden

; Remove AuthAgent from Task Scheduler (when present)
; Filename: "schtasks"; Parameters: "/delete /tn ""TcpRedirectorAuthAgent"" /f"; Flags: runhidden

[Icons]
Name: "{group}\{#AppName} GUI"; Filename: "{app}\{#AppExeName}"; Components: gui
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{commondesktop}\{#AppName} GUI"; Filename: "{app}\{#AppExeName}"; Components: gui; Flags: createonlyiffileexists

[Code]
function InitializeSetup: Boolean;
begin
  Result := True;
  if not IsAdmin then
  begin
    MsgBox('This installer requires administrator privileges.' + #13#10 +
           'Right-click the installer and select "Run as administrator".',
      mbError, MB_OK);
    Result := False;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
  begin
    if FileExists(ExpandConstant('{commonappdata}\{#AppName}\config.json')) then
      FileCopy(
        ExpandConstant('{commonappdata}\{#AppName}\config.json'),
        ExpandConstant('{commonappdata}\{#AppName}\config.json.bak'),
        False);
  end;
end;
