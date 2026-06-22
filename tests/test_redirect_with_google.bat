@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion
cd /d "%~dp0"

set MOCK_PROXY=%~dp0mock_proxy.exe
set PACKET_GEN=%~dp0packet_generator.exe
set PROXY_APP=%~dp0..\build\service\TcpRedirectorService.exe
set PROXY_PORT=3128
set PROXY_LOG=%~dp0proxy_google_log.txt

echo ============================================================
echo   TEST: Redirect packet_generator --^> google.com:80
echo          through mock_proxy:127.0.0.1:%PROXY_PORT%
echo ============================================================
echo.

REM -------------------------------------------------------
REM  Step 1: Kill old processes
REM -------------------------------------------------------
echo [1/5] Killing old processes...
taskkill /f /im TcpRedirectorService.exe 2>nul
taskkill /f /im mock_proxy.exe 2>nul
taskkill /f /im packet_generator.exe 2>nul
timeout /t 1 /nobreak >nul
echo [OK] Processes stopped
echo.

REM -------------------------------------------------------
REM  Step 2: Clean old log
REM -------------------------------------------------------
echo [2/5] Cleaning old log...
if exist "%PROXY_LOG%" del "%PROXY_LOG%"
echo [OK] Log cleaned
echo.

REM -------------------------------------------------------
REM  Step 3: Start mock_proxy on port 3128
REM -------------------------------------------------------
echo [3/5] Starting mock_proxy on port %PROXY_PORT%...
echo       Log: %PROXY_LOG%
start "mock_proxy" /B "%MOCK_PROXY%" --port %PROXY_PORT% --protocol tcp --log "%PROXY_LOG%"
if !errorlevel! neq 0 (
    echo [ERROR] Failed to start mock_proxy!
    pause
    exit /b 1
)
echo [OK] mock_proxy started
echo.

REM Wait for server to start
timeout /t 2 /nobreak >nul

REM -------------------------------------------------------
REM  Step 4: Start TcpRedirectorService
REM -------------------------------------------------------
echo [4/5] Starting TcpRedirectorService (console mode)...
echo       Make sure WinDivert.dll is in build/service/
echo       Config: C:\ProgramData\TcpRedirector\config.json
start "TcpRedirector" /B "%PROXY_APP%" --console
if !errorlevel! neq 0 (
    echo [ERROR] Failed to start TcpRedirectorService!
    pause
    exit /b 1
)
echo [OK] TcpRedirectorService started
echo.

REM Wait for service to initialize WinDivert
timeout /t 3 /nobreak >nul

REM -------------------------------------------------------
REM  Step 5: Run packet_generator to google.com:80
REM -------------------------------------------------------
echo [5/5] Running packet_generator to google.com:80...
echo.

"%PACKET_GEN%" google.com 80
set PG_RESULT=!errorlevel!
echo.
echo [PACKET_GEN] Exit code: !PG_RESULT!

REM Give time for async processing
timeout /t 2 /nobreak >nul

REM -------------------------------------------------------
REM  Cleanup test processes
REM -------------------------------------------------------
echo.
echo ============================================================
echo   CLEANUP
echo ============================================================
echo Stopping test processes...
taskkill /f /im TcpRedirectorService.exe 2>nul
taskkill /f /im mock_proxy.exe 2>nul
timeout /t 1 /nobreak >nul

REM -------------------------------------------------------
REM  Check results
REM -------------------------------------------------------
echo.
echo ============================================================
echo   RESULTS
echo ============================================================
echo.

REM Check proxy log
if exist "%PROXY_LOG%" (
    echo [MOCK_PROXY LOG] %PROXY_LOG%
    echo ----------------------------------------
    type "%PROXY_LOG%"
    echo ----------------------------------------
) else (
    echo [WARN] mock_proxy log not found: %PROXY_LOG%
)

echo.

REM Check TcpRedirector logs
set LOG_DIR=C:\ProgramData\TcpRedirector\logs
if exist "%LOG_DIR%\tcp_redirector.log" (
    echo [TCP_REDIRECTOR LOG] %LOG_DIR%\tcp_redirector.log (last 20 lines)
    echo ----------------------------------------
    powershell -Command "Get-Content '%LOG_DIR%\tcp_redirector.log' -Tail 20"
    echo ----------------------------------------
) else (
    echo [WARN] tcp_redirector.log not found
)

echo.
if exist "%LOG_DIR%\windivert_debug.log" (
    echo [WINDIVERT DEBUG LOG] %LOG_DIR%\windivert_debug.log (last 20 lines)
    echo ----------------------------------------
    powershell -Command "Get-Content '%LOG_DIR%\windivert_debug.log' -Tail 20"
    echo ----------------------------------------
) else (
    echo [WARN] windivert_debug.log not found
)

echo.
echo ============================================================
if !PG_RESULT! equ 0 (
    echo [RESULT] packet_generator succeeded
) else (
    echo [RESULT] packet_generator finished (exit code: !PG_RESULT!)
    echo         (WSAECONNREFUSED is expected if redirect failed)
)
echo ============================================================
echo.
echo Expected log lines to check manually:
echo   tcp_redirector.log: "Redirected: packet_generator.exe"
echo   windivert_debug.log: "PROXIED SYN"
echo   proxy_google_log.txt: "CONNECT google.com:80"
echo.
pause