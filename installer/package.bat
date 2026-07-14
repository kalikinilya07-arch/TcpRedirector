@echo off
title TcpRedirector — Build Installer
cd /d "%~dp0"
setlocal enabledelayedexpansion

:: -----------------------------------------------------------------------------
:: WP13 update: this script stages files into obfuscated\ for Inno Setup and
:: for the ZIP fallback (install.bat/uninstall.bat).  ALL third-party
:: binaries (WinDivert*, wintun.dll, tun2socks.exe) are OPTIONAL — absence
:: does not fail packaging; the installer just ships fewer files.
:: -----------------------------------------------------------------------------

:: WP-easy: DEPLOY / PKG_VERSION may be pre-set by a caller (e.g.
:: easy_mode_deploy.bat) via the environment. `if not defined` keeps the
:: historical manual behaviour intact (defaults unchanged) while letting the
:: easy-mode wrapper redirect the source tree and version label.
if not defined DEPLOY set DEPLOY=..\deploy_v3
if not defined PKG_VERSION set PKG_VERSION=1.1.0
set OBFUSCATED=obfuscated
set OUTPUT=..\output

echo ========================================
echo  TcpRedirector Installer Builder
echo ========================================
echo.

:: ==========================================
:: Step 1: Clean
:: ==========================================
echo [1/3] Cleaning...
if exist "%OBFUSCATED%" rmdir /S /Q "%OBFUSCATED%" >nul 2>&1
if exist "%OUTPUT%" rmdir /S /Q "%OUTPUT%" >nul 2>&1
mkdir "%OBFUSCATED%\gui\ru" >nul 2>&1
mkdir "%OBFUSCATED%\.bin\tun2socks" >nul 2>&1
mkdir "%OUTPUT%" >nul 2>&1
echo       OK

:: ==========================================
:: Step 2: Copy + Clean files
:: ==========================================
echo [2/3] Copying files...
if exist "%DEPLOY%\gui" (
    xcopy /Y /Q /E "%DEPLOY%\gui\*" "%OBFUSCATED%\gui\" >nul 2>&1
) else (
    echo       [WARN] %DEPLOY%\gui not found — GUI will be absent from the package
)

:: Remove debug files
del /Q "%OBFUSCATED%\gui\*.pdb" >nul 2>&1
del /Q "%OBFUSCATED%\gui\createdump.exe" >nul 2>&1
del /Q "%OBFUSCATED%\gui\clretwrc.dll" >nul 2>&1
del /Q "%OBFUSCATED%\gui\mscordaccore.dll" >nul 2>&1
del /Q "%OBFUSCATED%\gui\mscordaccore_amd64*.dll" >nul 2>&1
del /Q "%OBFUSCATED%\gui\mscordbi.dll" >nul 2>&1
del /Q "%OBFUSCATED%\gui\Microsoft.DiaSymReader.Native.amd64.dll" >nul 2>&1
del /Q "%OBFUSCATED%\gui\msquic.dll" >nul 2>&1

:: Remove non-ru language folders
for /d %%d in (cs de es fr it ja ko pl pt-BR tr zh-Hans zh-Hant) do (
    rmdir /S /Q "%OBFUSCATED%\gui\%%d" >nul 2>&1
)

:: -------------------------------------------------------------------
:: Service — REQUIRED. Fail loudly if it's missing.
:: -------------------------------------------------------------------
if not exist "%DEPLOY%\TcpRedirectorService.exe" (
    echo       [FAIL] %DEPLOY%\TcpRedirectorService.exe not found — run build.bat + deploy.bat first
    pause
    exit /b 1
)
copy /Y "%DEPLOY%\TcpRedirectorService.exe" "%OBFUSCATED%\" >nul

:: -------------------------------------------------------------------
:: WinDivert — OPTIONAL. WP13: wintun-only builds skip these.
:: -------------------------------------------------------------------
if exist "%DEPLOY%\WinDivert.dll" (
    copy /Y "%DEPLOY%\WinDivert.dll" "%OBFUSCATED%\" >nul
    if exist "%DEPLOY%\WinDivert64.sys" (
        copy /Y "%DEPLOY%\WinDivert64.sys" "%OBFUSCATED%\" >nul
    )
    echo       [OK] WinDivert staged
) else (
    echo       [SKIP] WinDivert absent — installer will run windivert-less
)

:: -------------------------------------------------------------------
:: Wintun — OPTIONAL.
:: -------------------------------------------------------------------
if exist "%DEPLOY%\wintun.dll" (
    copy /Y "%DEPLOY%\wintun.dll" "%OBFUSCATED%\" >nul
    echo       [OK] wintun.dll staged
) else (
    echo       [SKIP] wintun.dll absent — installer will not offer Wintun mode DLL
)

:: -------------------------------------------------------------------
:: tun2socks external engine — OPTIONAL. Preserves .bin\tun2socks\ layout.
:: -------------------------------------------------------------------
if exist "%DEPLOY%\.bin\tun2socks\tun2socks.exe" (
    copy /Y "%DEPLOY%\.bin\tun2socks\tun2socks.exe" "%OBFUSCATED%\.bin\tun2socks\" >nul
    echo       [OK] tun2socks.exe staged
) else (
    echo       [SKIP] tun2socks.exe absent — external engine unavailable
)

:: -------------------------------------------------------------------
:: Config seed — OPTIONAL. Ship as config.default.json; the installer
:: renames it to config.json ONLY if that file does not already exist
:: at the destination.  Prefer the file that lives next to this script
:: (installer\config.default.json) over any staged copy in %DEPLOY%.
:: -------------------------------------------------------------------
if exist "config.default.json" (
    copy /Y "config.default.json" "%OBFUSCATED%\config.default.json" >nul
    echo       [OK] config.default.json staged from installer\
) else if exist "%DEPLOY%\config.default.json" (
    copy /Y "%DEPLOY%\config.default.json" "%OBFUSCATED%\config.default.json" >nul
    echo       [OK] config.default.json staged from %DEPLOY%\
) else if exist "%DEPLOY%\config.json" (
    copy /Y "%DEPLOY%\config.json" "%OBFUSCATED%\config.default.json" >nul
    echo       [OK] legacy config.json staged as config.default.json
) else (
    echo       [SKIP] no config seed — service will self-generate on first run
)
echo       OK

:: ==========================================
:: Step 3: Create install.bat + uninstall.bat + ZIP
:: ==========================================
echo [3/3] Creating installer package...

:: -----------------------------------------------------------------------------
:: install.bat — mode-agnostic (WP13):
::   * copies whatever binaries were staged (skips missing ones);
::   * seeds config.json ONLY if it doesn't already exist;
::   * does NOT run `sc create WinDivert` (driver loads lazily);
::   * does NOT create %ProgramData%\TcpRedirector\config.json;
::   * installs the TcpRedirector service via its own --install entry point.
:: -----------------------------------------------------------------------------
(
echo @echo off
echo title TcpRedirector Setup
echo.
echo :: Require admin
echo net session ^>nul 2^>^&1
echo if errorlevel 1 ^(
echo     echo This installer requires Administrator privileges.
echo     echo Right-click install.bat ^> Run as Administrator.
echo     pause
echo     exit /b 1
echo ^)
echo.
echo echo ========================================
echo echo  TcpRedirector Setup
echo echo ========================================
echo echo.
echo set "APP_DIR=%%ProgramFiles%%\TcpRedirector"
echo.
echo echo [1/4] Creating directories...
echo mkdir "%%APP_DIR%%\gui" 2^>nul
echo mkdir "%%APP_DIR%%\.bin\tun2socks" 2^>nul
echo mkdir "%%APP_DIR%%\logs" 2^>nul
echo.
echo echo [2/4] Copying files...
echo xcopy /Y /E /Q "%%~dp0gui\*" "%%APP_DIR%%\gui\" ^>nul 2^>^&1
echo copy /Y "%%~dp0TcpRedirectorService.exe" "%%APP_DIR%%\" ^>nul
echo.
echo :: --- Optional WinDivert ---
echo if exist "%%~dp0WinDivert.dll" ^(
echo     copy /Y "%%~dp0WinDivert.dll"   "%%APP_DIR%%\" ^>nul
echo     copy /Y "%%~dp0WinDivert64.sys" "%%APP_DIR%%\" ^>nul 2^>^&1
echo     echo     [OK] WinDivert engine present
echo ^) else ^(
echo     echo     [SKIP] WinDivert not shipped
echo ^)
echo.
echo :: --- Optional Wintun ---
echo if exist "%%~dp0wintun.dll" ^(
echo     copy /Y "%%~dp0wintun.dll" "%%APP_DIR%%\" ^>nul
echo     echo     [OK] wintun engine present
echo ^) else ^(
echo     echo     [SKIP] wintun.dll not shipped
echo ^)
echo.
echo :: --- Optional external tun2socks ---
echo if exist "%%~dp0.bin\tun2socks\tun2socks.exe" ^(
echo     copy /Y "%%~dp0.bin\tun2socks\tun2socks.exe" "%%APP_DIR%%\.bin\tun2socks\" ^>nul
echo     echo     [OK] tun2socks.exe present
echo ^) else ^(
echo     echo     [SKIP] tun2socks.exe not shipped
echo ^)
echo.
echo :: --- Seed config.json only if missing (v2: lives next to EXE) ---
echo if not exist "%%APP_DIR%%\config.json" ^(
echo     if exist "%%~dp0config.default.json" ^(
echo         copy /Y "%%~dp0config.default.json" "%%APP_DIR%%\config.json" ^>nul
echo         echo     [OK] config.json seeded from config.default.json
echo     ^) else ^(
echo         echo     [INFO] no seed shipped; service will generate defaults on first run
echo     ^)
echo ^) else ^(
echo     echo     [KEEP] existing config.json preserved
echo ^)
echo.
echo echo [3/4] Installing TcpRedirector service...
echo "%%APP_DIR%%\TcpRedirectorService.exe" --install
echo sc start TcpRedirectorService ^>nul 2^>^&1
echo.
echo echo [4/4] Creating shortcuts...
echo powershell -Command "$ws = New-Object -ComObject WScript.Shell; $s = $ws.CreateShortcut([Environment]::GetFolderPath('CommonPrograms') + '\TcpRedirector.lnk'); $s.TargetPath = '%%APP_DIR%%\gui\TcpRedirectorGUI.exe'; $s.WorkingDirectory = '%%APP_DIR%%\gui'; $s.Save(); $s = $ws.CreateShortcut([Environment]::GetFolderPath('CommonDesktopDirectory') + '\TcpRedirector.lnk'); $s.TargetPath = '%%APP_DIR%%\gui\TcpRedirectorGUI.exe'; $s.WorkingDirectory = '%%APP_DIR%%\gui'; $s.Save()" ^>nul 2^>^&1
echo.
echo echo ========================================
echo echo  Installation complete!
echo echo  Launch: Start Menu ^> TcpRedirector
echo echo ========================================
echo pause
) > "%OBFUSCATED%\install.bat"

:: -----------------------------------------------------------------------------
:: uninstall.bat — mode-agnostic (WP13):
::   * stops + removes the TcpRedirector service;
::   * removes WinDivert ONLY if it exists (`sc query` gate);
::   * does NOT touch %APP_DIR%\config.json, logs\, or admin-added .bin\ files.
:: -----------------------------------------------------------------------------
(
echo @echo off
echo title TcpRedirector Uninstall
echo net session ^>nul 2^>^&1 ^|^| ^(echo Run as Administrator ^& pause ^& exit /b 1^)
echo set "APP_DIR=%%ProgramFiles%%\TcpRedirector"
echo.
echo echo Stopping TcpRedirector service...
echo sc stop TcpRedirectorService ^>nul 2^>^&1
echo "%%APP_DIR%%\TcpRedirectorService.exe" --uninstall ^>nul 2^>^&1
echo.
echo :: Only touch WinDivert if it was ever installed.
echo sc query WinDivert ^>nul 2^>^&1
echo if not errorlevel 1 ^(
echo     echo Removing WinDivert driver...
echo     sc stop WinDivert   ^>nul 2^>^&1
echo     sc delete WinDivert ^>nul 2^>^&1
echo ^) else ^(
echo     echo WinDivert driver not present ^(wintun-only deployment^) — skipping
echo ^)
echo.
echo :: Delete binaries but PRESERVE config.json, logs\, and .bin\.
echo if exist "%%APP_DIR%%\gui"                 rmdir /S /Q "%%APP_DIR%%\gui"
echo del /Q "%%APP_DIR%%\TcpRedirectorService.exe" 2^>nul
echo del /Q "%%APP_DIR%%\WinDivert.dll"            2^>nul
echo del /Q "%%APP_DIR%%\WinDivert64.sys"          2^>nul
echo del /Q "%%APP_DIR%%\wintun.dll"               2^>nul
echo.
echo echo Uninstalled. Preserved:
echo echo   - %%APP_DIR%%\config.json  ^(if present^)
echo echo   - %%APP_DIR%%\logs\         ^(if present^)
echo echo   - %%APP_DIR%%\.bin\          ^(third-party binaries^)
echo echo Delete %%APP_DIR%% by hand for a full wipe.
echo pause
) > "%OBFUSCATED%\uninstall.bat"

:: Create ZIP fallback (mirrors the Inno Setup output)
powershell -Command "Compress-Archive -Path '%OBFUSCATED%\*' -DestinationPath '%OUTPUT%\TcpRedirector_%PKG_VERSION%.zip' -Force" 2>&1

echo.
echo ========================================
echo  DONE!
echo  Package: ..\output\TcpRedirector_%PKG_VERSION%.zip
echo ========================================
echo.
echo To install:
echo   1. Extract TcpRedirector_%PKG_VERSION%.zip
echo   2. Right-click install.bat ^> Run as Administrator
echo   3. Launch from Start Menu
echo.
echo Included (present only if staged in %DEPLOY%\):
echo   - TcpRedirector GUI (self-contained, .NET 9.0 inside)
echo   - TcpRedirector Service
echo   - WinDivert.dll + WinDivert64.sys        [optional]
echo   - wintun.dll                              [optional]
echo   - .bin\tun2socks\tun2socks.exe           [optional]
echo   - config.default.json  (seed, first-run only)
echo   - install.bat / uninstall.bat
echo.
pause
