@echo off
title TcpRedirector
cd /d "%~dp0"

:: Kill old instance quietly
taskkill /im TcpRedirectorService.exe /f >nul 2>&1
timeout /t 1 /nobreak >nul

:: Launch service hidden (from build directory)
start /MIN "" "build\TcpRedirectorService.exe" --console
timeout /t 2 /nobreak >nul

:: Launch GUI
start "" "build\gui\TcpRedirectorGUI.exe"