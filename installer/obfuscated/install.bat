@echo off
title TcpRedirector Setup

:: Require admin
net session >nul 2>&1
if errorlevel 1 (
    echo This installer requires Administrator privileges.
    echo Right-click install.bat > Run as Administrator.
    pause
    exit /b 1
)

echo ========================================
echo  TcpRedirector Setup
echo ========================================
echo.
set "APP_DIR=%ProgramFiles%\TcpRedirector"

echo [1/4] Creating directories...
mkdir "%APP_DIR%\gui" 2>nul
mkdir "%APP_DIR%\.bin" 2>nul
mkdir "%APP_DIR%\logs" 2>nul

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

:: --- Optional .bin\ engine tree (wintun\<arch>\wintun.dll, tun2socks\) ---
if exist "%~dp0.bin" (
    xcopy /Y /E /I "%~dp0.bin\*" "%APP_DIR%\.bin\" >nul 2>&1
    if exist "%APP_DIR%\.bin\wintun\x64\wintun.dll" (
        echo     [OK] wintun engine present
    ) else (
        echo     [SKIP] wintun.dll not shipped
    )
    if exist "%APP_DIR%\.bin\tun2socks\tun2socks.exe" (
        echo     [OK] tun2socks.exe present
    ) else (
        echo     [SKIP] tun2socks.exe not shipped
    )
) else (
    echo     [SKIP] .bin\ engine tree not shipped
)

:: --- Seed config.json only if missing (v2: lives next to EXE) ---
if not exist "%APP_DIR%\config.json" (
    if exist "%~dp0config.default.json" (
        copy /Y "%~dp0config.default.json" "%APP_DIR%\config.json" >nul
        echo     [OK] config.json seeded from config.default.json
    ) else (
        echo     [INFO] no seed shipped; service will generate defaults on first run
    )
) else (
    echo     [KEEP] existing config.json preserved
)

echo [3/4] Installing TcpRedirector service...
"%APP_DIR%\TcpRedirectorService.exe" --install
:: Auto-recovery: restart the service automatically if it ever terminates
:: unexpectedly (belt-and-suspenders; the EXE also sets this on --install).
sc failure TcpRedirectorService reset= 3600 actions= restart/5000/restart/10000/restart/30000 >nul 2>&1
sc failureflag TcpRedirectorService 1 >nul 2>&1
sc start TcpRedirectorService >nul 2>&1

echo [4/4] Creating shortcuts...
powershell -Command "$ws = New-Object -ComObject WScript.Shell; $s = $ws.CreateShortcut([Environment]::GetFolderPath('CommonPrograms') + '\TcpRedirector.lnk'); $s.TargetPath = '%APP_DIR%\gui\TcpRedirectorGUI.exe'; $s.WorkingDirectory = '%APP_DIR%\gui'; $s.Save(); $s = $ws.CreateShortcut([Environment]::GetFolderPath('CommonDesktopDirectory') + '\TcpRedirector.lnk'); $s.TargetPath = '%APP_DIR%\gui\TcpRedirectorGUI.exe'; $s.WorkingDirectory = '%APP_DIR%\gui'; $s.Save()" >nul 2>&1

echo ========================================
echo  Installation complete
echo  Launch: Start Menu > TcpRedirector
echo ========================================
pause
