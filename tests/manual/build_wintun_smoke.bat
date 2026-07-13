@echo off
REM =============================================================================
REM  build_wintun_smoke.bat
REM
REM  MANUAL smoke test builder for WP8 primitives.  NOT invoked from build.bat.
REM  Requires:
REM    * MSVC toolchain in PATH (run from "x64 Native Tools Command Prompt for VS")
REM    * .bin\wintun\x64\wintun.dll present next to the resulting EXE at runtime
REM    * Administrator privileges to actually create a Wintun adapter
REM
REM  Output: %~dp0wintun_smoke.exe
REM =============================================================================
setlocal enableextensions
cd /d "%~dp0"

set OUT=wintun_smoke.exe
set SRV=..\..\src\service\TcpRedirectorService

REM Sanity-check MSVC.
where cl.exe >nul 2>nul
if errorlevel 1 (
    echo [FAIL] cl.exe not found in PATH.  Open the "x64 Native Tools Command Prompt for VS" and re-run.
    exit /b 1
)

echo [smoke-build] Compiling %OUT% ...
cl /nologo /EHsc /std:c++20 /W3 /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN ^
    /Fe:%OUT% ^
    wintun_smoke.cpp ^
    "%SRV%\infrastructure\capture\wintun\WintunApi.cpp" ^
    "%SRV%\infrastructure\capture\wintun\WintunAdapter.cpp" ^
    "%SRV%\infrastructure\capture\wintun\WintunSession.cpp" ^
    /I "%SRV%" ^
    /I "..\..\external" ^
    /link iphlpapi.lib ws2_32.lib advapi32.lib
if errorlevel 1 (
    echo [FAIL] Build failed.
    exit /b 1
)

echo.
echo [smoke-build] Built: %~dp0%OUT%
echo Ensure .bin\wintun\x64\wintun.dll is available next to the EXE (or under a parent's .bin\).
echo Run from an elevated shell:  %OUT%
endlocal
