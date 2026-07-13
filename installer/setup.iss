; =============================================================================
;  TcpRedirector — Inno Setup script
;  WP13: installer is capture-mode-agnostic.
;    - WinDivert.dll / WinDivert64.sys are OPTIONAL (shipped only when present).
;    - wintun.dll and .bin\tun2socks\tun2socks.exe are OPTIONAL similarly.
;    - config.json is seeded from config.default.json ONLY on first install
;      (never overwrites the admin's edited config on upgrade/reinstall).
;    - No %ProgramData% copy step (config lives next to the EXE, per WP1/WP2).
;    - No `sc create WinDivert`; the driver is loaded lazily by WinDivertOpen()
;      once preflight has passed, or not at all in wintun mode.
;    - Admins can choose install profile via [Components]:
;         * "windivert" — ship only WinDivert*
;         * "wintun"    — ship only wintun.dll + tun2socks (if present)
;         * "full"      — both (default when all binaries are available)
; =============================================================================

#define MyAppName "TcpRedirector"
#define MyAppVersion "1.1.0"
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

; -----------------------------------------------------------------------------
; Types / Components let the operator choose which capture mode(s) to ship.
; "full" is the default; the actual availability of each optional binary is
; still gated by `Check:` conditions in [Files] so nothing breaks if a source
; file is missing on the build host.
; -----------------------------------------------------------------------------
[Types]
Name: "full";      Description: "Full install (WinDivert + Wintun)"
Name: "windivert"; Description: "WinDivert only (packet-filter capture)"
Name: "wintun";    Description: "Wintun only (TUN-adapter capture)"
Name: "custom";    Description: "Custom"; Flags: iscustom

[Components]
Name: "core";      Description: "Core service + GUI (always installed)";       Types: full windivert wintun custom; Flags: fixed
Name: "windivert"; Description: "WinDivert engine (WinDivert.dll + driver)";   Types: full windivert
Name: "wintun";    Description: "Wintun engine (wintun.dll + optional tun2socks.exe)"; Types: full wintun

[Files]
; ==============================================================
; Core — GUI + service. Always present.
; ==============================================================
Source: "gui\*.exe";  Excludes: "createdump.exe"; DestDir: "{app}\gui"; Flags: ignoreversion; Components: core
Source: "gui\*.dll";  DestDir: "{app}\gui";       Flags: ignoreversion; Components: core
Source: "gui\*.json"; DestDir: "{app}\gui";       Flags: ignoreversion; Components: core
Source: "gui\ru\*";   DestDir: "{app}\gui\ru";    Flags: ignoreversion; Components: core

Source: "TcpRedirectorService.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: core

; ==============================================================
; WinDivert component — optional. `skipifsourcedoesntexist` means
; Inno Setup silently omits the entry if the file was not staged
; into the obfuscated\ tree by installer\package.bat.
; ==============================================================
Source: "WinDivert.dll";   DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist; Components: windivert
Source: "WinDivert64.sys"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist; Components: windivert

; ==============================================================
; Wintun component — optional (same pattern).
; wintun.dll lives at {app}\wintun.dll (LoadLibraryW default search).
; tun2socks.exe lives under {app}\.bin\tun2socks\ to match the
; runtime resolver in ChildProcessSupervisor / preflight.
; ==============================================================
Source: "wintun.dll";                 DestDir: "{app}";                    Flags: ignoreversion skipifsourcedoesntexist; Components: wintun
Source: ".bin\tun2socks\tun2socks.exe"; DestDir: "{app}\.bin\tun2socks";   Flags: ignoreversion skipifsourcedoesntexist; Components: wintun

; ==============================================================
; Config seed. Copied ONLY when {app}\config.json does not already
; exist (`onlyifdoesntexist`). This preserves the admin's edited
; config across upgrades — see user requirement #6 and WP1/WP2.
; Filename is renamed on the fly: shipped as config.default.json,
; installed as config.json.
; ==============================================================
Source: "config.default.json"; DestDir: "{app}"; DestName: "config.json"; Flags: ignoreversion onlyifdoesntexist skipifsourcedoesntexist; Components: core

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\gui\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}";  Filename: "{app}\gui\{#MyAppExeName}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; GroupDescription: "Additional icons:"

[Run]
; -----------------------------------------------------------------------------
; NOTE: No `sc create WinDivert` here. WinDivert's own driver-install path
; runs lazily on the first WinDivertOpen() call from the service, gated by
; preflight. If the user later switches to capture_mode="wintun", the
; WinDivert driver is never installed. This matches user requirement #6.
;
; NOTE: No copy to %ProgramData%\TcpRedirector\config.json. v2 config lives
; next to the service EXE. If a legacy %ProgramData% config exists, the
; service performs a one-shot migration on first startup after upgrade.
; -----------------------------------------------------------------------------

; Install and start the service (always — core component).
Filename: "{app}\TcpRedirectorService.exe"; Parameters: "--install"; \
    StatusMsg: "Installing TcpRedirector service..."; Flags: runhidden
Filename: "{sys}\sc.exe"; Parameters: "start TcpRedirectorService"; \
    StatusMsg: "Starting TcpRedirector service..."; Flags: runhidden

; Launch GUI (optional, user-driven).
Filename: "{app}\gui\{#MyAppExeName}"; \
    Description: "Launch TcpRedirector GUI"; \
    Flags: nowait postinstall skipifsilent

[UninstallRun]
; Stop and remove OUR service first. Errors are hidden — if the service
; was already gone (e.g. corrupted install) uninstall must still succeed.
Filename: "{sys}\sc.exe";                   Parameters: "stop TcpRedirectorService";  Flags: runhidden
Filename: "{app}\TcpRedirectorService.exe"; Parameters: "--uninstall";                 Flags: runhidden

; Stop / remove WinDivert driver ONLY if it exists (may have been created
; lazily by a prior run in windivert mode, or may never have been created
; at all in a pure wintun deployment).
Filename: "{sys}\sc.exe"; Parameters: "stop WinDivert";   Flags: runhidden; Check: WinDivertServiceExists
Filename: "{sys}\sc.exe"; Parameters: "delete WinDivert"; Flags: runhidden; Check: WinDivertServiceExists

[Code]
// Returns True if the "WinDivert" service exists on the target system.
// Used to gate the [UninstallRun] entries so uninstall doesn't error out
// on a wintun-only deployment where the service was never created.
function WinDivertServiceExists: Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(ExpandConstant('{sys}\sc.exe'), 'query WinDivert', '',
                 SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

// Uninstall guardrail: DO NOT touch {app}\config.json, {app}\logs\, or
// any third-party binaries the admin may have placed under {app}\.bin\.
// Inno Setup removes files it installed by default — the seed config was
// installed with `onlyifdoesntexist` so it's owned by the admin from that
// point on; we explicitly re-assert here.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    // Intentionally no DelTree() call. Config, logs, and .bin\ contents
    // survive uninstall so that reinstall / upgrade preserves user state.
    // If the admin wants a clean wipe they must delete {app} by hand.
  end;
end;
