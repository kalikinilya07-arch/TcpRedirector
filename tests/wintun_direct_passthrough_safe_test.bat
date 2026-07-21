@echo off
:: ===========================================================================
::  wintun_direct_passthrough_safe_test.bat
::
::  CONNECTIVITY-SAFE, SELF-TERMINATING test for the Wintun embedded DIRECT
::  passthrough fix.
::
::  WHY THIS SCRIPT IS SAFE:
::    When the service runs in embedded mode it pulls ALL IPv4-TCP into the TUN
::    via split-tunnel routes (0.0.0.0/1 + 128.0.0.0/1 ladder). If DIRECT
::    passthrough were still broken, the machine (and this SSH/agent session)
::    would LOSE connectivity until the service is stopped. Therefore this
::    script:
::      1) starts the service,
::      2) reproduces DIRECT + PROXY traffic and captures logs / route table /
::         connectivity results to FILES on disk,
::      3) ALWAYS stops the service and removes any leftover TUN / /32 routes
::         in a finally-style :cleanup block, even on error or timeout,
::      4) is bounded by a hard watchdog timeout that force-kills the run.
::
::    Read the captured files AFTER connectivity is restored.
::
::  USAGE (ELEVATED / Administrator cmd.exe REQUIRED):
::    tests\wintun_direct_passthrough_safe_test.bat
::
::  OPTIONAL ENV OVERRIDES:
::    set SVC_NAME=TcpRedirectorService        (Windows service name)
::    set TEST_WINDOW=25                        (seconds service stays up)
::    set DIRECT_HOST=1.1.1.1                   (a NON-listed dst to test DIRECT)
::    set OUTDIR=%TEMP%\tcpredir_test           (where result files go)
:: ===========================================================================

setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0.."
set "ROOT=%CD%"

:: ---- Config (overridable via env) ----
if not defined SVC_NAME    set "SVC_NAME=TcpRedirectorService"
if not defined TEST_WINDOW set "TEST_WINDOW=25"
if not defined DIRECT_HOST set "DIRECT_HOST=1.1.1.1"
if not defined OUTDIR       set "OUTDIR=%TEMP%\tcpredir_test"

:: Log file the service writes (adjust if your install logs elsewhere).
:: iter-2 FIX: correct path is the .logs dir under the install root.
set "SVC_LOG=C:\Program Files\TcpRedirector\.logs\tcp_redirector.log"
if not exist "%SVC_LOG%" set "SVC_LOG=%ProgramData%\TcpRedirector\.logs\tcp_redirector.log"
if not exist "%SVC_LOG%" set "SVC_LOG=C:\Program Files\TcpRedirector\logs\service.log"

if not exist "%OUTDIR%" mkdir "%OUTDIR%" >nul 2>&1

echo ============================================================
echo  Wintun DIRECT passthrough SAFE test
echo  Output dir : %OUTDIR%
echo  Service    : %SVC_NAME%
echo  Test window: %TEST_WINDOW%s
echo  DIRECT host: %DIRECT_HOST%
echo ============================================================

:: ---- Require elevation ----
:: NOTE: use a goto-based check (no parenthesized block) to avoid any
:: paren-reading fragility in the batch parser.
net session >nul 2>&1
if not errorlevel 1 goto elev_ok
echo [FAIL] This script must be run from an ELEVATED (Administrator) cmd.exe.
exit /b 1
:elev_ok

:: ---- 0. Baseline connectivity BEFORE start (proof the box is online) ----
echo [0/5] Capturing baseline (service NOT running)...
route print -4 > "%OUTDIR%\route_before.txt" 2>&1
ping -n 2 %DIRECT_HOST% > "%OUTDIR%\conn_before.txt" 2>&1

:: ===========================================================================
::  From here on the service may be UP. Any exit path MUST go through :cleanup.
:: ===========================================================================

:: ---- 1. Start the service ----
echo [1/5] Starting service %SVC_NAME% ...
sc start "%SVC_NAME%" > "%OUTDIR%\sc_start.txt" 2>&1
if errorlevel 1 (
    echo [WARN] sc start returned an error - see sc_start.txt. Proceeding to cleanup.
    goto cleanup
)

:: ---- 1a. Arm a background watchdog that force-stops the service AND purges
::          leftover TUN routes after the test window even if this script hangs
::          (guaranteed connectivity restoration). This mirrors :cleanup so that
::          even a total hang of the foreground script cannot black-hole the box.
::          NOTE: the split-tunnel ladder is /1../8 leaves (route_ladder_prefix),
::          NOT just 0.0.0.0/1 + 128.0.0.0/1, so a plain `route delete` of the two
::          /1 supernets is INSUFFICIENT. We purge every on-link IPv4 route with
::          prefix 1..8 and the IPv6 ::/1 + 8000::/1 catch-all via PowerShell. ----
:: FIX: the previous inline `start cmd /c "...nested quotes..."` broke the batch
:: parser (unexpected 'cmd.exe' / exit 255). The watchdog now lives in its own
:: file so no nested-quote escaping is needed. It is fully self-contained and
:: mirrors :cleanup for guaranteed connectivity restoration.
start "tcpredir-watchdog" /min cmd /c ""%ROOT%\tests\_watchdog_cleanup.bat" %TEST_WINDOW% "%SVC_NAME%""

:: Give the TUN + routes a moment to come up.
:: iter-2 FIX: use ping-based sleep (timeout /t fails on redirected/headless stdin).
ping -n 5 127.0.0.1 >nul 2>&1

:: ---- 2. Capture route table WHILE running (should show /1 ladder on TUN
::         AND physical /32 bypass routes for proxy + DNS) ----
echo [2/5] Capturing route table while running...
route print -4 > "%OUTDIR%\route_during.txt" 2>&1

:: ---- 3. Reproduce traffic ----
:: 3a. DIRECT: a NON-listed app (this cmd's curl/ping) reaching the internet.
::     If the fix works, this SUCCEEDS via the physical NIC.
echo [3/5] Reproducing DIRECT traffic (non-listed) ...
where curl >nul 2>&1
if %errorlevel%==0 (
    curl -s -o nul -w "DIRECT_HTTP_CODE=%%{http_code} TIME=%%{time_total}\n" --max-time 8 http://%DIRECT_HOST%/ > "%OUTDIR%\direct_curl.txt" 2>&1
    curl -s -o nul -w "DIRECT_HTTPS_CODE=%%{http_code} TIME=%%{time_total}\n" --max-time 8 https://example.com/ >> "%OUTDIR%\direct_curl.txt" 2>&1
) else (
    echo curl not found - using ping as a weaker DIRECT probe > "%OUTDIR%\direct_curl.txt"
)
ping -n 3 %DIRECT_HOST% > "%OUTDIR%\direct_ping.txt" 2>&1

:: 3b. DNS: resolve a name (exercises the LWIP_UDP=0 mitigation /32 bypass).
echo [3b] DNS resolution probe ...
nslookup example.com > "%OUTDIR%\dns_probe.txt" 2>&1

:: 3c. PROXY: a LISTED app should still be proxied. This is app-specific;
::     launch your configured target app here if you have one, e.g.:
::       start "" "C:\Path\To\ListedApp.exe"
::     We record a placeholder + let the service log show PROXY flows.
echo (Launch your LISTED app here to verify PROXY still works) > "%OUTDIR%\proxy_note.txt"

:: Let flows establish so logs capture DIRECT egress + PROXY decisions.
:: iter-2 FIX: ping-based sleep for headless runs.
ping -n 7 127.0.0.1 >nul 2>&1

:: ---- 4. Snapshot the service log (DIRECT egress ifIndex etc.) ----
:: iter-3 HARNESS FIX: de-parenthesized this block.  Previously the whole
:: snapshot lived inside an `if exist "%SVC_LOG%" ( ... ) else ( ... )` block,
:: and the multi-line `findstr /C:"...DIRECT /32..." /C:"connect("` patterns
:: (which contain `/`, `(` and `)` characters) confused cmd.exe's block parser
:: AFTER the evidence was already written — a non-fatal but noisy crash on exit.
:: Using a goto-based flow with no parentheses around the findstr commands makes
:: the harness self-terminate cleanly.
echo [4/5] Snapshotting service log ...
if not exist "%SVC_LOG%" goto :snapshot_missing

copy /Y "%SVC_LOG%" "%OUTDIR%\service_log_snapshot.txt" >nul 2>&1
:: Extract the key evidence lines (iter-3: include new close-source instrumentation).
findstr /I /C:"[wintun][flow] DIRECT connect OK" /C:"[wintun][flow] DIRECT connect FAILED" /C:"[wintun][DIRECT][pin]" /C:"[wintun][bypass] installed DIRECT" /C:"[wintun][bypass] InstallHostBypassVia FAILED" /C:"egress_ifindex" /C:"egress_src" /C:"[wintun][flow] PROXY" /C:"[wintun][flow][close]" /C:"ResolvePhysicalEgress" /C:"physical" "%SVC_LOG%" > "%OUTDIR%\evidence_lines.txt" 2>&1
:: iter-2/3: capture the decoded WSA reason codes / select / SO_ERROR diagnostics
:: plus the close-source line so a FAIL can be reported verbatim.
findstr /I /C:"reason:" /C:"WSA" /C:"SO_ERROR" /C:"select" /C:"IP_UNICAST_IF" /C:"src-bind" /C:"[wintun][DIRECT]" /C:"[wintun][bypass]" /C:"[wintun][flow]" "%SVC_LOG%" > "%OUTDIR%\wsa_reason_lines.txt" 2>&1
goto :snapshot_done

:snapshot_missing
echo Service log not found at "%SVC_LOG%" - set SVC_LOG env to correct path > "%OUTDIR%\service_log_snapshot.txt"

:snapshot_done

:cleanup
:: ===========================================================================
::  FINALLY: guaranteed connectivity restoration. Runs on EVERY exit path.
:: ===========================================================================
echo [5/5] CLEANUP - stopping service and removing leftover TUN / /32 routes ...

:: Stop the service (idempotent).
sc stop "%SVC_NAME%" > "%OUTDIR%\sc_stop.txt" 2>&1

:: Wait for it to actually stop (bounded). Use a for /l loop (no goto labels)
:: for parser robustness across shells; delayed expansion reads the live
:: errorlevel inside the loop body.
set "_stopped="
for /l %%I in (1,1,15) do (
    sc query "%SVC_NAME%" | findstr /I "STOPPED" >nul 2>&1
    if !errorlevel!==0 (
        set "_stopped=1"
    ) else (
        ping -n 2 127.0.0.1 >nul 2>&1
    )
)
if not defined _stopped (
    echo [WARN] Service did not stop cleanly; killing process image.
    taskkill /F /IM TcpRedirectorService.exe >nul 2>&1
)
:: Belt-and-suspenders: delete any leftover split-tunnel + bypass routes so the
:: machine NEVER stays black-holed even if TearDown didn't run.
::
:: IMPORTANT: the split-tunnel ladder is a set of on-link (NextHop 0.0.0.0)
:: leaves at prefix length route_ladder_prefix (1..8) - NOT just 0.0.0.0/1 +
:: 128.0.0.0/1. Deleting only the two /1 supernets would leave /2../8 leaves
:: behind and black-hole the box. We therefore purge EVERY on-link IPv4 route
:: with prefix 1..8 (mirrors RouteInstaller::UninstallSplitTunnel) plus the
:: IPv6 ::/1 + 8000::/1 catch-all. PowerShell Get-NetRoute/Remove-NetRoute is
:: the reliable, ladder-depth-independent way to do this.
echo Purging leftover TUN split-tunnel ladder (on-link IPv4 /1../8) + IPv6 catch-all ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-NetRoute -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.DestinationPrefix -match '/([1-8])$' -and $_.NextHop -eq '0.0.0.0' } | ForEach-Object { Write-Output ('deleting ' + $_.DestinationPrefix + ' ifIndex=' + $_.ifIndex); Remove-NetRoute -InputObject $_ -Confirm:$false -ErrorAction SilentlyContinue }; Get-NetRoute -AddressFamily IPv6 -ErrorAction SilentlyContinue | Where-Object { $_.DestinationPrefix -eq '::/1' -or $_.DestinationPrefix -eq '8000::/1' } | ForEach-Object { Write-Output ('deleting ' + $_.DestinationPrefix); Remove-NetRoute -InputObject $_ -Confirm:$false -ErrorAction SilentlyContinue }" > "%OUTDIR%\route_purge.txt" 2>&1

:: Legacy safety net for the historical /1 pair (harmless if already gone).
route delete 0.0.0.0 mask 128.0.0.0 >nul 2>&1
route delete 128.0.0.0 mask 128.0.0.0 >nul 2>&1

:: Re-capture the route table so leftovers (incl. any NETMGMT /32 proxy/DNS/
:: direct bypass host routes) can be eyeballed for verification.
route print -4 > "%OUTDIR%\route_after.txt" 2>&1
route print -6 > "%OUTDIR%\route_after_v6.txt" 2>&1

:: Explicit connectivity-safety assertion: fail loudly (in the file) if any
:: on-link /1../8 ladder leaf still remains after purge.
powershell -NoProfile -Command "$r = Get-NetRoute -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.DestinationPrefix -match '/([1-8])$' -and $_.NextHop -eq '0.0.0.0' }; if ($r) { 'LEFTOVER_LADDER_ROUTES=YES'; $r | Format-Table -AutoSize | Out-String } else { 'LEFTOVER_LADDER_ROUTES=NO' }" > "%OUTDIR%\cleanup_assert.txt" 2>&1

:: Final connectivity proof AFTER cleanup.
ping -n 2 %DIRECT_HOST% > "%OUTDIR%\conn_after.txt" 2>&1

echo.
echo ============================================================
echo  DONE. Connectivity should be restored.
echo  Review these files:
echo    %OUTDIR%\conn_before.txt      (baseline online)
echo    %OUTDIR%\route_during.txt     (ladder + physical /32 bypass while up)
echo    %OUTDIR%\direct_curl.txt      (DIRECT non-listed reaches internet?)
echo    %OUTDIR%\dns_probe.txt        (DNS resolves via /32 bypass?)
echo    %OUTDIR%\evidence_lines.txt   (DIRECT egress_ifindex = PHYSICAL, PROXY intact)
echo    %OUTDIR%\wsa_reason_lines.txt (decoded WSA reason: codes for FAIL triage)
echo    %OUTDIR%\cleanup_assert.txt   (LEFTOVER_LADDER_ROUTES=NO expected)
echo    %OUTDIR%\conn_after.txt       (connectivity restored)
echo ============================================================
endlocal
exit /b 0
