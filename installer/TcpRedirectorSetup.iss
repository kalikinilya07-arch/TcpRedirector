; TcpRedirector v1.1.1 — Inno Setup Installer
; Run with: iscc TcpRedirectorSetup.iss

#define AppName "TcpRedirector"
#define AppVersion "1.1.1"
#define AppPublisher "TcpRedirector"
#define AppExeName "TcpRedirectorGUI.exe"
#define ServiceExeName "TcpRedirectorService.exe"

[Setup]
AppId={{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
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
Name: "windivert"; Description: "WinDivert Driver"; Types: full compact custom; Flags: fixed

[Files]
; Service
Source: "..\release\{#AppVersion}\bin\{#ServiceExeName}"; DestDir: "{app}"; Components: service; Flags: ignoreversion

; AuthAgent (Kerberos authentication helper, runs in user session)
Source: "..\release\{#AppVersion}\bin\TcpRedirectorAuthAgent.exe"; DestDir: "{app}"; Components: service; Flags: ignoreversion

; GUI (self-contained, ~400 files)
Source: "..\release\{#AppVersion}\bin\gui\*"; DestDir: "{app}\gui"; Components: gui; Flags: ignoreversion recursesubdirs createallsubdirs

; WinDivert driver + install script
Source: "..\release\{#AppVersion}\bin\WinDivert.dll"; DestDir: "{app}"; Components: windivert; Flags: ignoreversion
Source: "..\release\{#AppVersion}\bin\WinDivert64.sys"; DestDir: "{app}"; Components: windivert; Flags: ignoreversion
Source: "..\release\{#AppVersion}\bin\install_windivert.bat"; DestDir: "{app}"; Components: windivert; Flags: ignoreversion

[Dirs]
Name: "{commonappdata}\{#AppName}"
Name: "{commonappdata}\{#AppName}\logs"

[Run]
; 1. Install WinDivert driver
Filename: "{app}\install_windivert.bat"; WorkingDir: "{app}"; Components: windivert; Flags: runhidden waituntilterminated

; 2. Install service
Filename: "{app}\{#ServiceExeName}"; Parameters: "--install"; WorkingDir: "{app}"; Components: service; Flags: runhidden waituntilterminated

; 3. Start service
Filename: "net"; Parameters: "start TcpRedirectorService"; Components: service; Flags: runhidden waituntilterminated

[UninstallRun]
; 1. Stop service
Filename: "net"; Parameters: "stop TcpRedirectorService"; Flags: runhidden
; 2. Uninstall service via --uninstall
Filename: "{app}\{#ServiceExeName}"; Parameters: "--uninstall"; WorkingDir: "{app}"; Flags: runhidden
; 3. Delete service from SCM (fallback if --uninstall fails)
Filename: "sc"; Parameters: "delete TcpRedirectorService"; Flags: runhidden

[Icons]
Name: "{group}\{#AppName} GUI"; Filename: "{app}\gui\{#AppExeName}"; Components: gui
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{commondesktop}\{#AppName} GUI"; Filename: "{app}\gui\{#AppExeName}"; Components: gui; Flags: createonlyiffileexists

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
