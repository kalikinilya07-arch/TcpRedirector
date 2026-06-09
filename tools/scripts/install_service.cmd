@echo off
setlocal enabledelayedexpansion

echo Installing TcpRedirector Service...
echo.

:: Check admin rights
net session >nul 2>&1
if errorlevel 1 (
    echo [ERROR] This script must be run as Administrator.
    pause
    exit /b 1
)

set ROOT_DIR=%~dp0..\..
set SERVICE_EXE=%ROOT_DIR%\build\service\x64\Release\TcpRedirectorService.exe

if not exist "%SERVICE_EXE%" (
    echo [ERROR] Service executable not found at:
    echo   %SERVICE_EXE%
    echo.
    echo Build it first:
    echo   msbuild src\service\TcpRedirectorService\TcpRedirectorService.vcxproj /p:Configuration=Release /p:Platform=x64
    pause
    exit /b 1
)

:: Remove existing if any
sc stop TcpRedirectorService >nul 2>&1
sc delete TcpRedirectorService >nul 2>&1

:: Install service
sc create TcpRedirectorService binPath="%SERVICE_EXE%" start=auto
sc description TcpRedirectorService "Redirects TCP connections through HTTP proxy using WFP"

echo.
echo === Service installed successfully ===
echo.
echo Commands:
echo   net start TcpRedirectorService      - Start service
echo   net stop TcpRedirectorService       - Stop service
echo   "%SERVICE_EXE%" --console          - Run in console (debug)
echo   "%SERVICE_EXE%" --uninstall        - Remove service
echo.
pause