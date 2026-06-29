@echo off
title TcpRedirector — Deploy
cd /d "%~dp0"

set DEPLOY_DIR=.\deploy
set BUILD_DIR=.\build

echo ========================================
echo  Preparing deployment package
echo ========================================
echo.

:: Create deploy directory
if exist "%DEPLOY_DIR%" rmdir /S /Q "%DEPLOY_DIR%" >nul 2>&1
mkdir "%DEPLOY_DIR%" >nul 2>&1
mkdir "%DEPLOY_DIR%\gui" >nul 2>&1

:: Copy service binary
echo [1] Copying service binary...
if exist "%BUILD_DIR%\TcpRedirectorService.exe" (
    copy /Y "%BUILD_DIR%\TcpRedirectorService.exe" "%DEPLOY_DIR%\" >nul 2>&1
    echo     OK
) else (
    echo     [WARN] Not found — run build.bat first
)

:: Copy GUI
echo [2] Copying GUI...
if exist "%BUILD_DIR%\gui" (
    xcopy /Y /E /I "%BUILD_DIR%\gui\*" "%DEPLOY_DIR%\gui\" >nul 2>&1
    echo     OK
)

:: Copy WinDivert (required for capture)
echo [3] Copying WinDivert (required)...
if exist "%BUILD_DIR%\WinDivert.dll" (
    copy /Y "%BUILD_DIR%\WinDivert.dll" "%DEPLOY_DIR%\" >nul 2>&1
) else (
    echo     [INFO] Place WinDivert.dll and WinDivert64.sys in %DEPLOY_DIR%\
    echo     Download from: https://github.com/nickhutchinson/libdivert/releases
)
if exist "%BUILD_DIR%\WinDivert64.sys" (
    copy /Y "%BUILD_DIR%\WinDivert64.sys" "%DEPLOY_DIR%\" >nul 2>&1
)

:: Create default config.json
echo [4] Creating default config.json...
(
echo {
echo     "app": {
echo         "exePath": "C:\\Path\\To\\YourApp.exe"
echo     },
echo     "proxy": {
echo         "host": "192.168.1.1",
echo         "port": 3128,
echo         "enabled": true
echo     },
echo     "auth": {
echo         "enabled": false,
echo         "username": "",
echo         "encryptedPassword": "",
echo         "kerberos": false
echo     },
echo     "log": {
echo         "level": 2,
echo         "fileEnabled": true,
echo         "maxSizeMB": 10
echo     },
echo     "rules": [
echo         {
echo             "id": "default",
echo             "pattern": "YourApp.exe",
echo             "description": "Redirect target app",
echo             "priority": 1,
echo             "enabled": true,
echo             "type": "process_name",
echo             "action": "proxy"
echo         }
echo     ]
echo }
) > "%DEPLOY_DIR%\config.json"

:: Create install script
echo [5] Creating install_service.bat...
(
echo @echo off
echo title Install TcpRedirector Service
echo.
echo :: Must run as Administrator
echo openfiles ^>nul 2^>^&1 ^|^| ^(echo This script must be run as Administrator! ^& pause ^& exit /b 1^)
echo.
echo :: Install service
echo "%%~dp0TcpRedirectorService.exe" --install
echo.
echo :: Start service
echo sc start TcpRedirectorService
echo.
echo echo Service installed and started.
echo pause
) > "%DEPLOY_DIR%\install_service.bat"

:: Create uninstall script
echo [6] Creating uninstall_service.bat...
(
echo @echo off
echo title Uninstall TcpRedirector Service
echo.
echo :: Must run as Administrator
echo openfiles ^>nul 2^>^&1 ^|^| ^(echo This script must be run as Administrator! ^& pause ^& exit /b 1^)
echo.
echo :: Stop and remove service
echo "%%~dp0TcpRedirectorService.exe" --uninstall
echo.
echo echo Service uninstalled.
echo pause
) > "%DEPLOY_DIR%\uninstall_service.bat"

:: Create console mode launcher
echo [7] Creating run_console.bat...
(
echo @echo off
echo title TcpRedirector ^(console^)
echo.
echo "%%~dp0TcpRedirectorService.exe" --console
echo pause
) > "%DEPLOY_DIR%\run_console.bat"

echo.
echo ========================================
echo  Deployment package ready!
echo  Package: %DEPLOY_DIR%\
echo ========================================
echo.
echo Required files to add manually:
echo   - WinDivert.dll   (from https://github.com/nickhutchinson/libdivert/releases)
echo   - WinDivert64.sys (same package, place next to exe)
echo   - nlohmann/json.hpp (already included in source)
echo.
echo On target machine:
echo   1. Copy entire "deploy" folder
echo   2. Place WinDivert.dll and WinDivert64.sys in the same folder
echo   3. Edit config.json
echo   4. Run install_service.bat as Administrator
echo.
pause
