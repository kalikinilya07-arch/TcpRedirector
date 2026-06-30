@echo off
title TcpRedirector (console)

:: Try to install WinDivert driver first (required for capture)
sc start WinDivert >nul 2>&1
if errorlevel 1 (
    echo [INFO] Installing WinDivert driver...
    sc create WinDivirt binPath="%~dp0WinDivert64.sys" type=kernel start=demand >nul 2>&1
    sc start WinDivert >nul 2>&1
    if errorlevel 1 echo [WARN] Could not install WinDivert driver. Trying auto-load...
)

"%~dp0TcpRedirectorService.exe" --console
pause
