@echo off
title TcpRedirector (console)

:: Auto-elevate if not running as Administrator
net session >nul 2>&1
if errorlevel 1 (
    echo [INFO] Restarting with Administrator privileges...
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

:: Ensure WinDivert driver is installed
echo [INFO] Checking WinDivert driver...
sc query WinDivert >nul 2>&1
if errorlevel 1 (
    echo [INFO] Installing WinDivert driver...
    sc create WinDivert binPath="%~dp0WinDivert64.sys" type=kernel start=demand >nul 2>&1
    sc start WinDivert >nul 2>&1
    if errorlevel 1 (
        echo [WARN] sc install failed. Trying auto-load via API-PPA...
    )
)

"%~dp0TcpRedirectorService.exe" --console
pause
