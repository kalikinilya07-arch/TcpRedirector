@echo off
echo ========================================
echo   TcpRedirector — Install Certificate
echo   (Run as Administrator!)
echo ========================================
echo.

cd /d "%~dp0..\.."
set CERTFILE=tools\certs\TcpRedirector.cer

if not exist "%CERTFILE%" (
    echo [FAIL] Certificate file not found: %CERTFILE%
    echo        Run sign_driver.bat first to create it.
    pause
    exit /b 1
)

echo [1/2] Installing certificate into Trusted Root...
certutil -addstore Root "%CERTFILE%"
if errorlevel 1 (
    echo [FAIL] Root store install failed.
    pause
    exit /b 1
)

echo [2/2] Installing certificate into Trusted Publishers...
certutil -addstore TrustedPublisher "%CERTFILE%"
if errorlevel 1 (
    echo [FAIL] Trusted Publishers install failed.
    pause
    exit /b 1
)

echo.
echo ========================================
echo   Certificate installed successfully!
echo   Driver can now be loaded without
echo   Test Mode (Secure Boot compatible).
echo ========================================
echo.
echo   To load the driver:
echo      sc create TcpRedirectorDriver type= kernel start= demand binPath= "%cd%\build\driver\TcpRedirectorDriver.sys"
echo      sc start TcpRedirectorDriver
