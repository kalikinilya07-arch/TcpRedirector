@echo off
title TcpRedirector Uninstall

:: -----------------------------------------------------------------------------
:: WP13 — mode-agnostic uninstall.
::   * Removes the TcpRedirector service.
::   * Removes the WinDivert driver ONLY if it was ever installed.
::   * PRESERVES %APP_DIR%\config.json, %APP_DIR%\logs\, %APP_DIR%\.bin\.
:: -----------------------------------------------------------------------------

net session >nul 2>&1 || (echo Run as Administrator & pause & exit /b 1)
set "APP_DIR=%ProgramFiles%\TcpRedirector"

echo Stopping TcpRedirector service...
sc stop TcpRedirectorService >nul 2>&1
"%APP_DIR%\TcpRedirectorService.exe" --uninstall >nul 2>&1

:: Only touch WinDivert if it was ever installed.
sc query WinDivert >nul 2>&1
if not errorlevel 1 (
    echo Removing WinDivert driver...
    sc stop WinDivert   >nul 2>&1
    sc delete WinDivert >nul 2>&1
) else (
    echo WinDivert driver not present ^(wintun-only deployment^) — skipping
)

:: Delete binaries but PRESERVE config.json, logs\, and .bin\.
if exist "%APP_DIR%\gui" rmdir /S /Q "%APP_DIR%\gui" 2>nul
del /Q "%APP_DIR%\TcpRedirectorService.exe" 2>nul
del /Q "%APP_DIR%\WinDivert.dll"            2>nul
del /Q "%APP_DIR%\WinDivert64.sys"          2>nul
del /Q "%APP_DIR%\wintun.dll"               2>nul

echo.
echo Uninstalled. Preserved:
echo   - %APP_DIR%\config.json  (if present)
echo   - %APP_DIR%\logs\         (if present)
echo   - %APP_DIR%\.bin\          (third-party binaries)
echo Delete %APP_DIR% by hand for a full wipe.
pause
