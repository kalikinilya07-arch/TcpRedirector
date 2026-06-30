@echo off
title Install WinDivert Driver

:: Must run as Administrator
openfiles >nul 2>&1 || (echo This script must be run as Administrator! & pause & exit /b 1)

:: Install WinDivert driver as a kernel service
sc create WinDivirt binPath="%~dp0WinDivert64.sys" type=kernel start=demand >nul 2>&1
sc start WinDivert >nul 2>&1
if errorlevel 1 (
    echo [WARN] Failed to install WinDivert driver. Trying API-PPA method...
    echo The service will attempt to auto-load WinDivert64.sys from its directory.
) else (
    echo [OK] WinDivert driver installed successfully
)

pause
