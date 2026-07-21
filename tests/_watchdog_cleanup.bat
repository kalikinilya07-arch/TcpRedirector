@echo off
:: ===========================================================================
::  _watchdog_cleanup.bat
::
::  Background connectivity-restoration watchdog for
::  wintun_direct_passthrough_safe_test.bat.
::
::  Guarantees the box NEVER stays black-holed even if the foreground test
::  hangs: after %1 seconds it force-stops the service and purges the whole
::  split-tunnel ladder (on-link IPv4 /1../8 leaves) + IPv6 ::/1 + 8000::/1
::  catch-all, mirroring RouteInstaller::UninstallSplitTunnel.
::
::  Invoked as:  _watchdog_cleanup.bat <TEST_WINDOW_SECONDS> <SVC_NAME>
::
::  Kept in a SEPARATE file (instead of an inline `start cmd /c "..."`) so we
::  avoid the nested-quote escaping that broke the parser previously.
:: ===========================================================================
setlocal EnableExtensions

set "WIN=%~1"
set "SVC=%~2"
if not defined WIN set "WIN=25"
if not defined SVC set "SVC=TcpRedirectorService"

:: Wait out the test window (ping-based sleep; robust on headless/redirected stdin).
set /a _p=%WIN%+1
ping -n %_p% 127.0.0.1 >nul 2>&1

:: Stop the service, give it time, force-kill if still alive.
sc stop "%SVC%" >nul 2>&1
ping -n 9 127.0.0.1 >nul 2>&1
sc query "%SVC%" | findstr /I "STOPPED" >nul 2>&1
if not %errorlevel%==0 taskkill /F /IM TcpRedirectorService.exe >nul 2>&1

:: Purge the entire on-link IPv4 /1../8 ladder + IPv6 catch-all (ladder-depth
:: independent). Full PowerShell path for environments where powershell is not
:: on the child shell PATH.
powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-NetRoute -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.DestinationPrefix -match '/([1-8])$' -and $_.NextHop -eq '0.0.0.0' } | Remove-NetRoute -Confirm:$false -ErrorAction SilentlyContinue; Get-NetRoute -AddressFamily IPv6 -ErrorAction SilentlyContinue | Where-Object { $_.DestinationPrefix -eq '::/1' -or $_.DestinationPrefix -eq '8000::/1' } | Remove-NetRoute -Confirm:$false -ErrorAction SilentlyContinue" >nul 2>&1

:: Legacy safety net for the historical /1 supernet pair.
route delete 0.0.0.0 mask 128.0.0.0 >nul 2>&1
route delete 128.0.0.0 mask 128.0.0.0 >nul 2>&1

endlocal
exit /b 0
