#define MyAppName "TcpRedirector"
#define MyAppVersion "1.0.1"
#define MyAppPublisher "TcpRedirector"
#define MyAppExeName "TcpRedirectorGUI.exe"

[Setup]
AppId={{B8E4F2A1-1234-5678-9ABC-DEF012345679}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
SourceDir=obfuscated
OutputDir=..\output
OutputBaseFilename=TcpRedirector_Setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Files]
; === GUI (self-contained, .NET included) ===
Source: "gui\*.exe";  Excludes: "createdump.exe"; DestDir: "{app}\gui"; Flags: ignoreversion
Source: "gui\*.dll";  DestDir: "{app}\gui"; Flags: ignoreversion
Source: "gui\*.json"; DestDir: "{app}\gui"; Flags: ignoreversion
Source: "gui\ru\*";   DestDir: "{app}\gui\ru"; Flags: ignoreversion

; === Service ===
Source: "TcpRedirectorService.exe"; DestDir: "{app}"; Flags: ignoreversion

; === WinDivert ===
Source: "WinDivert.dll";   DestDir: "{app}"; Flags: ignoreversion
Source: "WinDivert64.sys"; DestDir: "{app}"; Flags: ignoreversion

; === Config ===
Source: "config.json"; DestDir: "{app}"; Flags: ignoreversion onlyifdoesntexist

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\gui\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}";  Filename: "{app}\gui\{#MyAppExeName}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; GroupDescription: "Additional icons:"

[Run]
; Create ProgramData config
Filename: "{sys}\cmd.exe"; \
    Parameters: "/c mkdir ""{commonappdata}\TcpRedirector"" 2>nul & copy /Y ""{app}\config.json"" ""{commonappdata}\TcpRedirector\config.json"""; \
    StatusMsg: "Creating config in ProgramData..."; Flags: runhidden

; Install WinDivert kernel driver
Filename: "{sys}\sc.exe"; Parameters: "create WinDivert binPath=""{app}\WinDivert64.sys"" type=kernel start=demand"; \
    StatusMsg: "Installing WinDivert driver..."; Flags: runhidden
Filename: "{sys}\sc.exe"; Parameters: "start WinDivert"; \
    StatusMsg: "Starting WinDivert driver..."; Flags: runhidden

; Install and start service
Filename: "{app}\TcpRedirectorService.exe"; Parameters: "--install"; \
    StatusMsg: "Installing TcpRedirector service..."; Flags: runhidden
Filename: "{sys}\sc.exe"; Parameters: "start TcpRedirectorService"; \
    StatusMsg: "Starting TcpRedirector service..."; Flags: runhidden

; Launch GUI
Filename: "{app}\gui\{#MyAppExeName}"; \
    Description: "Launch TcpRedirector GUI"; \
    Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\sc.exe"; Parameters: "stop TcpRedirectorService"; Flags: runhidden
Filename: "{app}\TcpRedirectorService.exe"; Parameters: "--uninstall"; Flags: runhidden
Filename: "{sys}\sc.exe"; Parameters: "stop WinDivert"; Flags: runhidden
Filename: "{sys}\sc.exe"; Parameters: "delete WinDivert"; Flags: runhidden

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    if MsgBox('Delete configuration files in ' +
      ExpandConstant('{commonappdata}\TcpRedirector') + '?',
      mbConfirmation, MB_YESNO) = IDYES then
    begin
      DelTree(ExpandConstant('{commonappdata}\TcpRedirector'), True, True, True);
    end;
  end;
end;
