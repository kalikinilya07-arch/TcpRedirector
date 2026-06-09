@echo off
title TcpRedirector
cd /d "%~dp0"

:: Kill old instance quietly
taskkill /im TcpRedirectorService.exe /f >nul 2>&1
timeout /t 1 /nobreak >nul

:: Launch service hidden
start /MIN "" "src\service\TcpRedirectorService\build\service\x64\Release\TcpRedirectorService.exe" --console
timeout /t 2 /nobreak >nul

:: Launch GUI
start "" "build\gui\TcpRedirectorGUI.exe"