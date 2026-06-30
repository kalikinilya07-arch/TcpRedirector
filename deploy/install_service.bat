@echo off
title Install TcpRedirector Service

:: Must run as Administrator
openfiles >nul 2>&1 || (echo This script must be run as Administrator! & pause & exit /b 1)

:: Install service
"%~dp0TcpRedirectorService.exe" --install

:: Start service
sc start TcpRedirectorService

echo Service installed and started.
pause
