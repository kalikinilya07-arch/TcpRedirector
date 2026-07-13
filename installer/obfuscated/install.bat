@echo off
title TcpRedirector Setup

:: -----------------------------------------------------------------------------
:: WP13 — mode-agnostic bootstrap. Copies whatever binaries are present in the
:: package (WinDivert, Wintun, tun2socks are all optional). Config lives next
:: to the EXE per WP1/WP2; %ProgramData% is no longer used.
:: `sc create WinDivert` is intentionally NOT performed here — WinDivert
:: installs its driver lazily on first WinDivertOpen() from the service.
:: -----------------------------------------------------------------------------

:: Require admin
net session >nul 2>&1
if errorlevel 1 (
    echo This installer requires Administrator privileges.
    echo Right-click install.bat ^> Run as Administrator.
    pause
    exit /b 1
)

echo ========================================
echo  TcpRedirector Setup
echo ========================================
echo.
set "APP_DIR=%ProgramFiles%\TcpRedirector"

echo [1/4] Creating directories...
mkdir "%APP_DIR%\gui"                2>nul
mkdir "%APP_DIR%\.bin\tun2socks"     2>nul
mkdir "%APP_DIR%\logs"               2>nul

echo [2/4] Copying files...
xcopy /Y /E /Q "%~dp0gui\*" "%APP_DIR%\gui\" >nul 2>&1
copy /Y "%~dp0TcpRedirectorService.exe" "%APP_DIR%\" >nul

:: --- Optional WinDivert ---
if exist "%~dp0WinDivert.dll" (
    copy /Y "%~dp0WinDivert.dll"   "%APP_DIR%\" >nul
    copy /Y "%~dp0WinDivert64.sys" "%APP_DIR%\" >nul 2>&1
    echo     [OK] WinDivert engine present
) else (
    echo     [SKIP] WinDivert not shipped
)

:: --- Optional Wintun ---
if exist "%~dp0wintun.dll" (
    copy /Y "%~dp0wintun.dll" "%APP_DIR%\" >nul
    echo     [OK] wintun engine present
) else (
    echo     [SKIP] wintun.dll not shipped
)

:: --- Optional external tun2socks ---
if exist "%~dp0.bin\tun2socks\tun2socks.exe" (
    copy /Y "%~dp0.bin\tun2socks\tun2socks.exe" "%APP_DIR%\.bin\tun2socks\" >nul
    echo     [OK] tun2socks.exe present
) else (
    echo     [SKIP] tun2socks.exe not shipped
)

:: --- Seed config.json only if missing (v2: lives next to EXE) ---
if not exist "%APP_DIR%\config.json" (
    if exist "%~dp0config.default.json" (
        copy /Y "%~dp0config.default.json" "%APP_DIR%\config.json" >nul
        echo     [OK] config.json seeded from config.default.json
    ) else if exist "%~dp0config.json" (
        copy /Y "%~dp0config.json" "%APP_DIR%\config.json" >nul
        echo     [OK] config.json seeded from package
    ) else (
        echo     [INFO] no seed shipped; service will generate defaults on first run
    )
) else (
    echo     [KEEP] existing config.json preserved
)

echo [3/4] Installing TcpRedirector service...
"%APP_DIR%\TcpRedirectorService.exe" --install
sc start TcpRedirectorService >nul 2>&1

echo [4/4] Creating shortcuts...
powershell -Command "$ws = New-Object -ComObject WScript.Shell; $s = $ws.CreateShortcut([Environment]::GetFolderPath('CommonPrograms') + '\TcpRedirector.lnk'); $s.TargetPath = '%APP_DIR%\gui\TcpRedirectorGUI.exe'; $s.WorkingDirectory = '%APP_DIR%\gui'; $s.Save(); $s = $ws.CreateShortcut([Environment]::GetFolderPath('CommonDesktopDirectory') + '\TcpRedirector.lnk'); $s.TargetPath = '%APP_DIR%\gui\TcpRedirectorGUI.exe'; $s.WorkingDirectory = '%APP_DIR%\gui'; $s.Save()" >nul 2>&1

echo ========================================
echo  Installation complete
echo  Launch: Start Menu ^> TcpRedirector
echo ========================================
pause
