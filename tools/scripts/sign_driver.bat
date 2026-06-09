@echo off
setlocal
cd /d "%~dp0..\.."

set SIGNCERT=tools\certs\TcpRedirector.pfx
set CERTFILE=tools\certs\TcpRedirector.cer
set SIGNTOOL=C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x64\signtool.exe
set DRIVER=build\driver\TcpRedirectorDriver.sys

echo ========================================
echo   TcpRedirector — Sign Driver
echo ========================================
echo.

if not exist "%DRIVER%" (
    echo [FAIL] Driver not found: %DRIVER%
    echo        Run build_driver.bat first.
    pause
    exit /b 1
)

:: Create cert if it doesn't exist
if not exist "%SIGNCERT%" (
    echo [1/3] Creating self-signed certificate...
    powershell -NoProfile -ExecutionPolicy Bypass -File "tools\scripts\create_cert.ps1"
    if errorlevel 1 (
        echo [FAIL] Certificate creation failed!
        pause
        exit /b 1
    )
) else (
    echo [1/3] Certificate already exists: %SIGNCERT%
)

:: Sign the driver
echo [2/3] Signing driver...
"%SIGNTOOL%" sign /fd SHA256 /f "%SIGNCERT%" /p tcp123 ^
    /tr http://timestamp.digicert.com /td SHA256 ^
    "%DRIVER%"
if errorlevel 1 (
    echo [FAIL] Signing failed!
    pause
    exit /b 1
)

:: Verify signature
echo [3/3] Verifying signature...
"%SIGNTOOL%" verify /pa "%DRIVER%" >nul 2>&1
if errorlevel 1 (
    echo [WARN] Signature verification (expected for self-signed cert)
) else (
    echo [OK] Signature verified
)

echo.
echo ========================================
echo   Driver signed successfully!
echo   %DRIVER%
echo.
echo   BEFORE loading the driver, run as Administrator:
echo      tools\scripts\install_cert.bat
echo ========================================
