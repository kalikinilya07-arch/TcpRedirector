# Fix: `.bin\wintun` DLL missing from final delivery

## Problem
The TcpRedirector service loads/validates wintun from a fixed runtime path:

- `WintunApi::DefaultDllPath` → `{exeDir}\.bin\wintun\<arch>\wintun.dll`
  (`src/service/TcpRedirectorService/infrastructure/capture/wintun/WintunApi.cpp:59-74`)
- `WintunPreflight::Check` expects the same
  (`src/service/TcpRedirectorService/infrastructure/preflight/WintunPreflight.h:78-83`)
- `arch` = `x64` on the native 64-bit build; `binDir` = `{exeDir}\.bin\`
  (`src/service/TcpRedirectorService/infrastructure/paths/AppPaths.h:102-104`)

But every packaging step ships `wintun.dll` **flat at the app root**, so it never
reaches `{app}\.bin\wintun\x64\wintun.dll` and wintun mode silently fails.

Repo source tree to mirror:
```
.bin\tun2socks\tun2socks.exe
.bin\wintun\{arm,arm64,x86,x64}\wintun.dll
```

## Decision
Mirror the **entire** repo `.bin\` tree (preserving structure) into the delivery
at every stage, so `.bin\wintun\x64\wintun.dll`, tun2socks, and any future DLL are
included automatically. This replaces the per-engine flat copies for wintun and the
special-case tun2socks copy with one generic `.bin\` mirror (build outputs overlaid
on top when present).

## Files to change (implementation-capable agent)

### 1. `easy_mode_deploy.bat` (staging into `deploy_latest\`)
- Replace the Wintun block (`:157-171`) and the tun2socks block (`:173-186`) with a
  single generic mirror of the repo `.bin\` tree into `%STAGE%\.bin\`:
  - `xcopy /Y /E /I "%ROOT%\.bin\*" "%STAGE%\.bin\"` (creates `%STAGE%\.bin` if absent).
  - Then overlay build outputs when present so a freshly built engine wins:
    - if `build\wintun.dll` exists → copy into `%STAGE%\.bin\wintun\x64\`
    - if `build\.bin\tun2socks\tun2socks.exe` exists → copy into `%STAGE%\.bin\tun2socks\`
- Keep engine-detection summary flags working: set `ENG_WINTUN=yes` if
  `%STAGE%\.bin\wintun\x64\wintun.dll` exists after staging; set `ENG_TUN2SOCKS=yes`
  if `%STAGE%\.bin\tun2socks\tun2socks.exe` exists.
- Remove the now-obsolete `mkdir "%STAGE%\.bin\tun2socks"` special-casing if it
  becomes redundant. WinDivert handling (`:141-154`) is unchanged (WinDivert ships at root).

### 2. `installer\package.bat` (staging into `obfuscated\`)
- Replace the flat wintun copy (`:89-94`) and the tun2socks copy (`:99-104`) with a
  mirror of the staged `.bin\` tree:
  - `if exist "%DEPLOY%\.bin" xcopy /Y /Q /E /I "%DEPLOY%\.bin\*" "%OBFUSCATED%\.bin\"`
- Update `mkdir "%OBFUSCATED%\.bin\tun2socks"` (`:34`) as needed (xcopy `/I` creates
  dirs, so the pre-mkdir can be dropped or kept harmlessly).
- Update the `[SKIP]`/`[OK]` log lines and the "Included" summary (`:277-278`) to
  reflect the `.bin\wintun\<arch>\` layout.

### 3. Generated `install.bat` (emitted inside `installer\package.bat`)
- The generated installer copies binaries to `%APP_DIR%`. Replace the flat
  `wintun.dll` copy (`package.bat:176-182`) and the tun2socks copy (`:184-190`) with a
  recursive copy of the shipped `.bin\` tree:
  - `xcopy /Y /E /I "%~dp0.bin\*" "%APP_DIR%\.bin\"` (escaped for the heredoc, i.e.
    `echo xcopy /Y /E /I "%%~dp0.bin\*" "%%APP_DIR%%\.bin\" ^>nul 2^>^&1`).
- Ensure the pre-created dir at `install.bat` step includes `%APP_DIR%\.bin\wintun`
  (or rely on xcopy `/I`). `uninstall.bat` already preserves `%APP_DIR%\.bin\` — leave as is.

### 4. `installer\setup.iss` (Inno Setup)
- Fix the misleading comment (`:85`): wintun.dll is loaded from
  `{app}\.bin\wintun\<arch>\wintun.dll`, NOT the default LoadLibrary search.
- Replace the flat wintun entry (`:89`) with a recursive `.bin\wintun` copy:
  - `Source: ".bin\wintun\*"; DestDir: "{app}\.bin\wintun"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Components: wintun`
- Keep/confirm the tun2socks entry (`:90`) → `{app}\.bin\tun2socks`.
- Confirm `[Code] CurUninstallStepChanged` still preserves `{app}\.bin\` (already does).

## Validation
1. Run `easy_mode_deploy.bat`. Confirm `deploy_latest\.bin\wintun\x64\wintun.dll`
   and `deploy_latest\.bin\tun2socks\tun2socks.exe` exist.
2. Inspect `output\TcpRedirector_latest.zip`: it must contain
   `.bin\wintun\x64\wintun.dll` (plus other arch folders).
3. Extract the ZIP, run `install.bat` as admin, confirm
   `%ProgramFiles%\TcpRedirector\.bin\wintun\x64\wintun.dll` exists.
4. If ISCC is available, install the `TcpRedirector_Setup.exe` with the Wintun/Full
   component and confirm the same `.bin\wintun\x64\wintun.dll` lands under `{app}`.
5. Switch config to `capture_mode="wintun"`, start the service, confirm WintunPreflight
   passes (no "wintun.dll not found (expected: ...\.bin\wintun\x64\wintun.dll)" failure).

## Notes / risks
- WinDivert continues to ship at app root (its resolver differs); do not move it.
- `easy_mode_deploy.bat` must keep CRLF line endings (documented in its header).
- Shipping all arch subfolders is intentional (matches "все dll из .bin"); only x64 is
  used by the 64-bit build but the extra folders are harmless and future-proof.
