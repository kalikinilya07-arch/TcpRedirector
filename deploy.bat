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

    :: Remove unnecessary files from self-contained publish
    :: 1. Debug symbols
    if exist "%DEPLOY_DIR%\gui\*.pdb" del /Q "%DEPLOY_DIR%\gui\*.pdb"
    :: 2. Debug/ETW DLLs
    if exist "%DEPLOY_DIR%\gui\clretwrc.dll" del "%DEPLOY_DIR%\gui\clretwrc.dll"
    if exist "%DEPLOY_DIR%\gui\mscordaccore.dll" del "%DEPLOY_DIR%\gui\mscordaccore.dll"
    if exist "%DEPLOY_DIR%\gui\mscordaccore_amd64*.dll" del "%DEPLOY_DIR%\gui\mscordaccore_amd64*.dll"
    if exist "%DEPLOY_DIR%\gui\mscordbi.dll" del "%DEPLOY_DIR%\gui\mscordbi.dll"
    if exist "%DEPLOY_DIR%\gui\createdump.exe" del "%DEPLOY_DIR%\gui\createdump.exe"
    if exist "%DEPLOY_DIR%\gui\Microsoft.DiaSymReader.Native.*.dll" del "%DEPLOY_DIR%\gui\Microsoft.DiaSymReader.Native.*.dll"
    :: 3. QUIC (HTTP/3) — не используется
    if exist "%DEPLOY_DIR%\gui\msquic.dll" del "%DEPLOY_DIR%\gui\msquic.dll"
    :: 4. Language folders — оставить только ru и en (invariant)
    for /d %%d in ("%DEPLOY_DIR%\gui\cs" "%DEPLOY_DIR%\gui\de" "%DEPLOY_DIR%\gui\es" "%DEPLOY_DIR%\gui\fr" "%DEPLOY_DIR%\gui\it" "%DEPLOY_DIR%\gui\ja" "%DEPLOY_DIR%\gui\ko" "%DEPLOY_DIR%\gui\pl" "%DEPLOY_DIR%\gui\pt-BR" "%DEPLOY_DIR%\gui\tr" "%DEPLOY_DIR%\gui\zh-Hans" "%DEPLOY_DIR%\gui\zh-Hant") do (
        if exist "%%d" rmdir /S /Q "%%d" >nul 2>&1
    )

    echo     OK (%DEPLOY_DIR%\gui\)
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

:: Create WinDivert install helper
(
echo @echo off
echo title Install WinDivert Driver
echo.
echo :: Must run as Administrator
echo openfiles ^>nul 2^>^&1 ^|^| ^(echo This script must be run as Administrator! ^& pause ^& exit /b 1^)
echo.
echo :: Install WinDivert driver as a kernel service
echo sc create WinDivirt binPath="%%~dp0WinDivert64.sys" type=kernel start=demand ^>nul 2^>^&1
echo sc start WinDivert ^>nul 2^>^&1
echo if errorlevel 1 ^(
echo     echo [WARN] Failed to install WinDivert driver. Trying API-PPA method...
echo     echo The service will attempt to auto-load WinDivert64.sys from its directory.
echo ^) else ^(
echo     echo [OK] WinDivert driver installed successfully
echo ^)
echo.
echo pause
) > "%DEPLOY_DIR%\install_windivert.bat"

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
echo :: Auto-elevate if not running as Administrator
echo net session ^>nul 2^>^&1
echo if errorlevel 1 ^(
echo     echo [INFO] Restarting with Administrator privileges...
echo     powershell -Command "Start-Process '%%~f0' -Verb RunAs"
echo     exit /b
echo ^)
echo.
echo :: Ensure WinDivert driver is installed
echo echo [INFO] Checking WinDivert driver...
echo sc query WinDivert ^>nul 2^>^&1
echo if errorlevel 1 ^(
echo     echo [INFO] Installing WinDivert driver...
echo     sc create WinDivert binPath="%%~dp0WinDivert64.sys" type=kernel start=demand ^>nul 2^>^&1
echo     sc start WinDivert ^>nul 2^>^&1
echo     if errorlevel 1 ^(
echo         echo [WARN] sc install failed. Trying auto-load via API-PPA...
echo     ^)
echo ^)
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
