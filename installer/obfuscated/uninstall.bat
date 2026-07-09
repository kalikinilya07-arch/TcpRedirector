@echo off
title TcpRedirector Uninstall
net session >nul 2>&1 || (echo Run as Administrator & pause & exit /b 1)
set "APP_DIR=%ProgramFiles%\TcpRedirector"
set "APP_DATA=%ProgramData%\TcpRedirector"
sc stop TcpRedirectorService >nul 2>&1
"%APP_DIR%\TcpRedirectorService.exe" --uninstall >nul 2>&1
sc stop WinDivert >nul 2>&1
sc delete WinDivert >nul 2>&1
rmdir /S /Q "%APP_DIR%" 2>nul
echo Uninstalled. Config kept at %APP_DATA%
pause
