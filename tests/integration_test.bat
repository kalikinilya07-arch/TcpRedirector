@echo off
chcp 65001 > nul
setlocal enabledelayedexpansion

REM ============================================================
REM  integration_test.bat — Integration test for TcpRedirector
REM  
REM  Phase 1: Direct packet_generator -> mock_proxy
REM           (without TcpRedirector, no admin required)
REM  
REM  Phase 2: Full cycle with TcpRedirectorService
REM           (requires: admin + WinDivert.dll)
REM ============================================================
cd /d "%~dp0"

REM ----- Configuration -----
set MOCK_PROXY=%~dp0mock_proxy.exe
set PACKET_GEN=%~dp0packet_generator.exe
set PROXY_APP=%~dp0..\build\service\TcpRedirectorService.exe
set PROXY_PORT=8888
set PROXY_LOG=%~dp0proxy_log.txt
set TEST_IP=1.2.3.4
set TEST_PORT=9999
set PASSED=0
set FAILED=0
set SKIPPED=0

echo ============================================================
echo   INTEGRATION TEST — TcpRedirector
echo ============================================================
echo   Proxy port:       %PROXY_PORT%
echo   Mock proxy:       %MOCK_PROXY%
echo   Packet generator: %PACKET_GEN%
echo   Service exe:      %PROXY_APP%
echo   Target:           packet_generator.exe --^> %TEST_IP%:%TEST_PORT%
echo ============================================================
echo.

REM -------------------------------------------------------
REM  Phase 1: Direct packet_generator -> mock_proxy
REM  Verifies mock_proxy and packet_generator work together
REM -------------------------------------------------------
echo [PHASE 1/2] Direct test: packet_generator --^> mock_proxy...
echo.

REM Check tools exist
if not exist "%MOCK_PROXY%" (
    echo [ERROR] %MOCK_PROXY% not found!
    echo Build: g++ mock_proxy.cpp -o mock_proxy.exe -lws2_32
    set /a FAILED+=1
    goto :phase2
)
if not exist "%PACKET_GEN%" (
    echo [ERROR] %PACKET_GEN% not found!
    echo Build: g++ packet_generator.cpp -o packet_generator.exe -lws2_32
    set /a FAILED+=1
    goto :phase2
)

REM Clean old log
if exist "%PROXY_LOG%" del "%PROXY_LOG%"

REM Start mock_proxy on port 8888
echo [TEST] Starting mock_proxy on port %PROXY_PORT%...
start "mock_proxy" /B "%MOCK_PROXY%" --port %PROXY_PORT% --protocol tcp --log "%PROXY_LOG%"

REM Wait for server startup
ping -n 3 127.0.0.1 > nul

REM Send packet directly to mock_proxy
echo [TEST] Sending packet to 127.0.0.1:%PROXY_PORT%...
"%PACKET_GEN%" --dest_ip 127.0.0.1 --dest_port %PROXY_PORT% --count 1 --protocol tcp
set PG_RESULT=!errorlevel!
echo [TEST] packet_generator exit code: !PG_RESULT!

REM Wait for processing
ping -n 3 127.0.0.1 > nul

REM Stop mock_proxy
taskkill /IM mock_proxy.exe /F > nul 2>&1
ping -n 2 127.0.0.1 > nul

REM Check log
echo.
echo [PHASE 1] Result:

if exist "%PROXY_LOG%" (
    for %%A in ("%PROXY_LOG%") do (
        if %%~zA gtr 100 (
            echo   [OK] Packets reached mock_proxy (%%~zA bytes^)
            set /a PASSED+=1
        ) else (
            echo   [FAIL] Packets did NOT reach mock_proxy
            echo   Log size: %%~zA bytes
            echo   Possible causes:
            echo   - Port %PROXY_PORT% is busy
            echo   - Firewall blocking local connections
            echo   - mock_proxy failed to start
            set /a FAILED+=1
        )
    )
) else (
    echo   [FAIL] No log file found
    set /a FAILED+=1
)
echo.

REM -------------------------------------------------------
REM  Phase 2: Full cycle with TcpRedirectorService
REM  Requires: admin rights + WinDivert.dll
REM -------------------------------------------------------
:phase2
echo [PHASE 2/2] Full test: TcpRedirectorService --^> mock_proxy...
echo.

REM Check admin rights
net session > nul 2>&1
set IS_ADMIN=!errorlevel!
if !IS_ADMIN! neq 0 (
    echo [SKIP] Phase 2 skipped - admin rights required.
    echo Run run_integration_test.bat as Administrator.
    set /a SKIPPED+=1
    goto :print_summary
)

REM Check TcpRedirectorService.exe
if not exist "%PROXY_APP%" (
    echo [SKIP] Phase 2 skipped - %PROXY_APP% not found.
    echo Build the project first (VS: F7, or MSBuild).
    set /a SKIPPED+=1
    goto :print_summary
)

REM Check WinDivert.dll
set PROXY_DIR=%~dp0..\build\service
if not exist "%PROXY_DIR%\WinDivert.dll" (
    echo [SKIP] Phase 2 skipped - WinDivert.dll not found.
    echo Copy WinDivert.dll and WinDivert64.sys to %PROXY_DIR%
    set /a SKIPPED+=1
    goto :print_summary
)

echo [OK] All prerequisites met.

REM Clean old log
if exist "%PROXY_LOG%" del "%PROXY_LOG%"

REM 2.1 — Start mock_proxy
echo.
echo [TEST] Starting mock_proxy on port %PROXY_PORT%...
start "mock_proxy" /B "%MOCK_PROXY%" --port %PROXY_PORT% --protocol tcp --log "%PROXY_LOG%"
ping -n 3 127.0.0.1 > nul

REM 2.2 — Start TcpRedirectorService in console mode
echo [TEST] Starting TcpRedirectorService --console...
echo [TEST] Service configured: proxy 127.0.0.1:8888, rule: packet_generator.exe
start "TcpRedirectorService" /B "%PROXY_APP%" --console > "%TEMP%\tcp_redirector_test.log" 2>&1"
ping -n 3 127.0.0.1 > nul

REM 2.3 — Run packet_generator to external IP (1.2.3.4:9999)
echo [TEST] Running packet_generator to %TEST_IP%:%TEST_PORT%...
echo [TEST] TcpRedirectorService should capture and redirect via WinDivert...
"%PACKET_GEN%" --dest_ip %TEST_IP% --dest_port %TEST_PORT% --count 1 --protocol tcp
set PG_RESULT=!errorlevel!
echo [TEST] packet_generator exit code: !PG_RESULT!

REM Wait for WinDivert processing
ping -n 4 127.0.0.1 > nul

REM 2.4 — Stop all processes
echo.
echo [TEST] Stopping test processes...
taskkill /IM TcpRedirectorService.exe /F > nul 2>&1
taskkill /IM mock_proxy.exe /F > nul 2>&1
ping -n 2 127.0.0.1 > nul

REM 2.5 — Check results
echo.
echo [PHASE 2] Result:

set SVC_LOG_SIZE=0
if exist "%TEMP%\tcp_redirector_test.log" (
    for %%A in ("%TEMP%\tcp_redirector_test.log") do set SVC_LOG_SIZE=%%~zA
)

if exist "%PROXY_LOG%" (
    for %%A in ("%PROXY_LOG%") do (
        set LOG_SIZE2=%%~zA
        echo   proxy_log.txt: %%~zA bytes
        echo   service log:   !SVC_LOG_SIZE! bytes
        if %%~zA gtr 100 (
            echo.
            echo   [OK] Packets reached mock_proxy!
            echo   TcpRedirectorService successfully captured and redirected traffic.
            echo.
            set /a PASSED+=1
        ) else (
            echo.
            echo   [FAIL] Packets did NOT reach mock_proxy!
            echo   Possible causes:
            echo   - WinDivert not capturing (WinDivert.dll missing^)
            echo   - Rule not matching packet_generator.exe
            echo   - TcpRedirectorService failed to start
            echo.
            echo   --- Service log ---
            if !SVC_LOG_SIZE! gtr 0 (
                type "%TEMP%\tcp_redirector_test.log"
            ) else (
                echo   (empty)
            )
            echo   -------------------
            set /a FAILED+=1
        )
    )
) else (
    echo   proxy_log.txt: not found
    echo   service log:   !SVC_LOG_SIZE! bytes
    echo.
    echo   [FAIL] No log file found
    set /a FAILED+=1
)

:print_summary
echo.
echo ============================================================
echo   SUMMARY
echo ============================================================
echo   Passed:  %PASSED%
echo   Failed:  %FAILED%
echo   Skipped: %SKIPPED%
echo ============================================================

if %PASSED% gtr 0 (
    if %FAILED% equ 0 (
        echo.
        echo   ALL TESTS PASSED
    ) else (
        echo.
        echo   SOME TESTS FAILED
    )
) else if %FAILED% gtr 0 (
    echo.
    echo   TESTS FAILED
) else (
    echo.
    echo   NO RESULTS - all tests skipped
)

echo.
echo [DONE] integration_test.bat finished.

:end
endlocal
