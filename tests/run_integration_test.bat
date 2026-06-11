@echo off
chcp 65001 > nul
setlocal enabledelayedexpansion

cd /d "%~dp0"

set MOCK_PROXY=%~dp0mock_proxy.exe
set PACKET_GEN=%~dp0packet_generator.exe
set PROXY_APP=%~dp0..\build\service\TcpRedirectorService.exe
set TOOLS_OK=1

echo ============================================================
echo   RUN INTEGRATION TEST - TcpRedirector
echo ============================================================
echo.

echo [CHECK] mock_proxy.exe...
if exist "%MOCK_PROXY%" (
    echo   [OK] Found: %MOCK_PROXY%
) else (
    echo   [MISSING] mock_proxy.exe not found
    echo   Build: g++ mock_proxy.cpp -o mock_proxy.exe -lws2_32
    set TOOLS_OK=0
)
echo.

echo [CHECK] packet_generator.exe...
if exist "%PACKET_GEN%" (
    echo   [OK] Found: %PACKET_GEN%
) else (
    echo   [MISSING] packet_generator.exe not found
    echo   Build: g++ packet_generator.cpp -o packet_generator.exe -lws2_32
    set TOOLS_OK=0
)
echo.

echo [CHECK] TcpRedirectorService.exe...
if exist "%PROXY_APP%" (
    echo   [OK] Found: %PROXY_APP%
) else (
    echo   [WARN] TcpRedirectorService.exe not found
)
echo.

set PROXY_DIR=%~dp0..\build\service
echo [CHECK] WinDivert.dll...
if exist "%PROXY_DIR%\WinDivert.dll" (
    echo   [OK] WinDivert.dll found in %PROXY_DIR%
) else (
    echo   [WARN] WinDivert.dll not found
    echo   Copy WinDivert.dll and WinDivert64.sys to %PROXY_DIR%
)
echo.

echo [CHECK] Administrator privileges...
net session > nul 2>&1
if !errorlevel! equ 0 (
    echo   [OK] Running as Administrator
) else (
    echo   [WARN] NOT running as Administrator
    echo   Phase 2 requires admin rights
)
echo.

if !TOOLS_OK! equ 0 (
    echo ============================================================
    echo   ERROR: Required tools not found
    echo ============================================================
    echo.
    echo   Quick build:
    echo     g++ packet_generator.cpp -o packet_generator.exe -lws2_32
    echo     g++ mock_proxy.cpp -o mock_proxy.exe -lws2_32
    goto :end
)

echo ============================================================
echo   All checks passed. Starting integration_test.bat...
echo ============================================================
echo.

call "%~dp0integration_test.bat"

:end
echo.
echo [DONE] run_integration_test.bat finished.
endlocal