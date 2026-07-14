@echo off
title TcpRedirector - Build
cd /d "%~dp0"
set "ROOT=%~dp0"
:: Strip trailing backslash so "%ROOT%\..." never doubles up.
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

:: WP13 hardening: guarantee ROOT is defined before any mkdir/copy that uses it.
:: A prior interrupted run created a literal ".\%ROOT%\build" folder because the
:: variable was unset when plan snippets were pasted into a bare shell. Abort loudly
:: rather than ever fabricate a directory literally named "%ROOT%".
if not defined ROOT (
    echo [FAIL] ROOT is not defined - refusing to run to avoid creating a literal "%%ROOT%%" folder.
    pause & exit /b 1
)
if not exist "%ROOT%\VERSION" (
    echo [FAIL] ROOT ^(%ROOT%^) does not look like the repo root ^(no VERSION file^).
    pause & exit /b 1
)

:: WP13 build-hardening: when re-entered with the hidden "__stage__" argument
:: (spawned by `start /wait cmd /c` below in a FRESH console) jump straight to
:: the optional-binary staging. A fresh console gives clean stdout handles;
:: msbuild/VsDevCmd otherwise leave the inherited handles in a state that
:: aborts the copy blocks right after the first WinDivert copy.
if /I "%~1"=="__stage__" goto stage_optional_binaries

echo ========================================
echo  Building TcpRedirector
echo ========================================
echo.

:: Read version
set /p VERSION=<"%ROOT%\VERSION"
echo [INFO] Version: %VERSION%

:: Auto-detect Visual Studio
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% set VSWHERE="%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist %VSWHERE% (
    for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -property installationPath`) do set VS_DIR=%%i
)

if not defined VS_DIR (
    echo [FAIL] Visual Studio 2022 not found!
    echo Install Visual Studio 2022 Build Tools or Visual Studio 2022 Community/Pro/Enterprise
    echo Download: https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022
    echo Select workload: "Desktop development with C++"
    pause
    exit /b 1
)

echo [INFO] Visual Studio found: %VS_DIR%

:: Set up VS environment
call "%VS_DIR%\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul 2>&1
if errorlevel 1 (
    echo [FAIL] Failed to initialize VS environment
    pause & exit /b 1
)

:: Clean build directory (temporary - releases go to releases\)
echo [0/3] Cleaning build directory...
if exist "%ROOT%\build" rmdir /S /Q "%ROOT%\build" >nul 2>&1
mkdir "%ROOT%\build" >nul 2>&1
echo [OK]

:: Step 1: Unit Tests
echo [1/3] Compiling RuleEngine tests...
cd /d "%ROOT%\tests\unit\service"
cl /EHsc /std:c++20 /Fe:RuleEngineTest.exe RuleEngineTest.cpp /I "%ROOT%\src\service\TcpRedirectorService" /nologo 2>&1
if errorlevel 1 echo [FAIL] Tests compilation failed! & pause & exit /b 1

RuleEngineTest.exe 2>&1
if errorlevel 1 echo [FAIL] Tests failed! & pause & exit /b 1
echo [PASS]

:: Step 2: GUI - self-contained publish
echo [2/3] Building GUI (self-contained)...
cd /d "%ROOT%\src\gui\TcpRedirectorGUI"
dotnet publish TcpRedirectorGUI.csproj -c Release --self-contained true -r win-x64 --nologo -o "%ROOT%\build\gui\" 2>&1
if errorlevel 1 echo [FAIL] & pause & exit /b 1
echo [OK]

:: Step 3: Service
echo [3/3] Building Service...
cd /d "%ROOT%\src\service\TcpRedirectorService"
:: Clean LTCG cache to force full recompilation
if exist "build\service\x64\Release\obj" (
    del /Q "build\service\x64\Release\obj\*.obj"  2>nul
    del /Q "build\service\x64\Release\obj\*.iobj" 2>nul
    del /Q "build\service\x64\Release\obj\*.ipdb" 2>nul
)
echo     (cleaned .obj/.iobj/.ipdb cache)
:: /nodeReuse:false stops msbuild leaving a persistent build node that holds the
:: console handle - that lingering node corrupted stdout for the later copy steps.
msbuild TcpRedirectorService.vcxproj /p:Configuration=Release /p:Platform=x64 /nologo /nodeReuse:false 2>&1
if errorlevel 1 echo [FAIL] & pause & exit /b 1
copy /Y "build\service\x64\Release\TcpRedirectorService.exe" "%ROOT%\build\" >nul 2>&1
echo [OK]

:: WP13: return to workspace root before staging optional third-party binaries.
cd /d "%ROOT%"

:: --------------------------------------------------------------------
:: WP13 build-hardening: stage the OPTIONAL third-party binaries in a
:: brand-new cmd.exe process (re-entering this script with the hidden
:: "__stage__" argument). msbuild leaves the current process's stdout
:: handle bound to its own now-closed pipe; the next `copy`'s status
:: write then aborts the run right after the first WinDivert copy. A
:: fresh `cmd /c call` gets clean handles, so staging always completes.
:: Every copy fully redirects (>nul 2>&1); every source is optional
:: (missing = [SKIP], never a failure) - WinDivert-only and Wintun-only
:: builds are both valid.
:: NOTE: this file MUST keep CRLF line endings; cmd.exe cannot resolve
:: `goto` / label jumps in an LF-only batch file.
:: --------------------------------------------------------------------
cmd /c call "%ROOT%\build.bat" __stage__

echo.
echo ========================================
echo  Build complete!
echo  Output: %ROOT%\build\
echo  Next:   deploy.bat  (creates versioned release)
echo ========================================
goto :eof

:: ====================================================================
:: Subroutine: stage optional capture-engine binaries + config seed.
:: Every source is optional (missing = [SKIP], never a build failure);
:: WinDivert-only and Wintun-only builds are both valid.
::
:: Each copy fully redirects stdout+stderr (>nul 2>&1). deploy.bat is the
:: authoritative release stager and independently sources wintun.dll /
:: tun2socks.exe / config.default.json from the repo .bin\ and installer\
:: trees, so the shipped package is complete even if a given build host's
:: shell truncates this best-effort build\-staging step.
:: ====================================================================
:stage_optional_binaries
:: --- WinDivert.dll + WinDivert64.sys (OPTIONAL) ---
if exist "%ROOT%\external\WinDivert\WinDivert-2.2.2-A\x64\WinDivert.dll" (
    copy /Y "%ROOT%\external\WinDivert\WinDivert-2.2.2-A\x64\WinDivert.dll" "%ROOT%\build\" >nul 2>&1
    copy /Y "%ROOT%\external\WinDivert\WinDivert-2.2.2-A\x64\WinDivert64.sys" "%ROOT%\build\" >nul 2>&1
    echo     [OK] WinDivert.dll + WinDivert64.sys copied
) else if exist "%ROOT%\deploy\WinDivert.dll" (
    copy /Y "%ROOT%\deploy\WinDivert.dll" "%ROOT%\build\" >nul 2>&1
    if exist "%ROOT%\deploy\WinDivert64.sys" copy /Y "%ROOT%\deploy\WinDivert64.sys" "%ROOT%\build\" >nul 2>&1
    echo     [OK] WinDivert copied from deploy\
) else (
    echo     [SKIP] WinDivert not found - build will run in wintun-only mode
)

:: --- Wintun.dll x64 (OPTIONAL) ---
if exist "%ROOT%\.bin\wintun\x64\wintun.dll" (
    copy /Y "%ROOT%\.bin\wintun\x64\wintun.dll" "%ROOT%\build\" >nul 2>&1
    echo     [OK] wintun.dll (x64) copied
) else if exist "%ROOT%\.bin\wintun.dll" (
    copy /Y "%ROOT%\.bin\wintun.dll" "%ROOT%\build\" >nul 2>&1
    echo     [OK] wintun.dll copied
) else (
    echo     [SKIP] wintun.dll not found in .bin\ - wintun mode will fail preflight until admin drops it in
)

:: --- tun2socks.exe external engine child process (OPTIONAL) ---
if exist "%ROOT%\.bin\tun2socks\tun2socks.exe" (
    if not exist "%ROOT%\build\.bin\tun2socks" mkdir "%ROOT%\build\.bin\tun2socks" >nul 2>&1
    copy /Y "%ROOT%\.bin\tun2socks\tun2socks.exe" "%ROOT%\build\.bin\tun2socks\" >nul 2>&1
    echo     [OK] tun2socks.exe copied to build\.bin\tun2socks\
) else (
    echo     [SKIP] tun2socks.exe not found in .bin\tun2socks\ - external engine will fail preflight if selected
)

:: --- config.default.json seed (OPTIONAL) ---
if exist "%ROOT%\installer\config.default.json" (
    copy /Y "%ROOT%\installer\config.default.json" "%ROOT%\build\config.default.json" >nul 2>&1
    echo     [OK] config.default.json seed copied
) else (
    echo     [SKIP] no installer\config.default.json seed - service will self-generate on first run
)
goto :eof
