@echo off
title TcpRedirector
cd /d "%~dp0"

:: Check admin rights (required for WinDivert + Named Pipe)
net session >nul 2>&1
if errorlevel 1 (
    echo [ERROR] This script requires Administrator privileges.
    echo         Right-click run.bat ^> Run as Administrator
    pause
    exit /b 1
)

echo ========================================
echo  TcpRedirector — Quick Launch
echo ========================================
echo.

:: Kill old instance quietly
taskkill /im TcpRedirectorService.exe /f >nul 2>&1
timeout /t 1 /nobreak >nul

:: Launch service in console mode (hidden window)
echo [1/2] Starting TcpRedirector Service...
start /MIN "" "build\TcpRedirectorService.exe" --console
timeout /t 3 /nobreak >nul

:: Verify service started
tasklist /fi "imagename eq TcpRedirectorService.exe" 2>nul | find "TcpRedirectorService.exe" >nul
if errorlevel 1 (
    echo [FAIL] Service failed to start! Check logs:
    echo        %%ProgramData%%\TcpRedirector\logs\tcp_redirector.log
    pause
    exit /b 1
)
echo [OK] Service running

:: Launch GUI
echo [2/2] Starting GUI...
start "" "build\gui\TcpRedirectorGUI.exe"
echo [OK] GUI launched

echo.
echo ========================================
echo  Both processes started.
echo  Close GUI window to stop.
echo  Service runs in background (minimized).
echo ========================================