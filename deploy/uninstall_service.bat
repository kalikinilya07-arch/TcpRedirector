@echo off
title Uninstall TcpRedirector Service

:: Must run as Administrator
openfiles >nul 2>&1 || (echo This script must be run as Administrator! & pause & exit /b 1)

:: Stop and remove service
"%~dp0TcpRedirectorService.exe" --uninstall

echo Service uninstalled.
pause
