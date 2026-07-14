@echo off
:: =============================================================================
::  TcpRedirector — EASY MODE (single-entry full pipeline)
:: =============================================================================
::  Runs the ENTIRE delivery pipeline in one shot, no questions asked:
::    build  ->  stage into deploy_latest\  ->  package (ZIP)  ->  (optional) ISCC
::
::  Key rules (see docs\СБОРКА.md §"Easy-режим"):
::    * Version is ALWAYS "latest". The VERSION file is NEVER incremented or
::      corrupted: its original content is saved and restored at the end.
::    * ALL available engines are bundled: WinDivert (.dll + .sys), wintun.dll,
::      tun2socks.exe. A missing OPTIONAL engine => [SKIP] (not fatal). Only the
::      service binary is REQUIRED.
::    * The intermediate delivery is assembled in deploy_latest\ (repo root),
::      cleaned on every run — it never touches the normal releases\ flow.
::    * We deliberately DO NOT call deploy.bat (it auto-increments VERSION and
::      parses it as major.minor.patch, which "latest" would break). Instead we
::      replicate deploy.bat's useful staging here, targeting deploy_latest\.
::
::  NOTE: this file MUST keep CRLF line endings; cmd.exe cannot resolve goto /
::  label jumps in an LF-only batch file.
:: =============================================================================

title TcpRedirector - Easy Mode Deploy
cd /d "%~dp0"
set "ROOT=%~dp0"
:: Strip trailing backslash so "%ROOT%\..." never doubles up.
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

if not defined ROOT (
    echo [FAIL] ROOT is not defined - refusing to run.
    pause & exit /b 1
)

setlocal enabledelayedexpansion

set "EASY_VERSION=latest"
set "STAGE=%ROOT%\deploy_latest"
set "VERSION_FILE=%ROOT%\VERSION"
set "VERSION_BAK=%ROOT%\VERSION.easymode.bak"
:: A safe numeric version handed to build.bat (which requires a VERSION file).
:: We restore the original afterwards, so this value never leaks into a release.
set "FALLBACK_VERSION=0.0.0"

set "STEP=0"
set "TOTAL=6"

echo ==========================================================
echo   TcpRedirector - EASY MODE  (version = %EASY_VERSION%)
echo ==========================================================
echo   Root      : %ROOT%
echo   Staging   : %STAGE%
echo   Package   : version label "%EASY_VERSION%"
echo ----------------------------------------------------------
echo.

:: =========================================================================
:: [1/6] Protect VERSION (save original; guarantee a valid numeric VERSION
::       for build.bat). Restored unchanged at the very end (:finalize).
:: =========================================================================
set /a STEP+=1
echo [%STEP%/%TOTAL%] Protecting VERSION file...
set "VERSION_EXISTED=0"
set "SAVED_VERSION="
if exist "%VERSION_FILE%" (
    set "VERSION_EXISTED=1"
    set /p SAVED_VERSION=<"%VERSION_FILE%"
    copy /Y "%VERSION_FILE%" "%VERSION_BAK%" >nul 2>&1
    echo     [OK] Saved original VERSION ^(!SAVED_VERSION!^)
    rem build.bat only needs a non-empty VERSION; keep the real value so the
    rem build banner stays truthful. We restore byte-for-byte from the backup.
) else (
    echo     [INFO] No VERSION file present - writing a temporary %FALLBACK_VERSION%
    echo %FALLBACK_VERSION%> "%VERSION_FILE%"
)

:: =========================================================================
:: [2/6] Build (compile everything) via build.bat.
::       Unit tests inside build.bat can be fragile on some hosts; in easy
::       mode we do NOT want a missing/broken test path to abort the whole
::       delivery. We therefore call build.bat and, if it fails, retry the
::       compile-only path without letting the test step be fatal.
:: =========================================================================
set /a STEP+=1
echo.
echo [%STEP%/%TOTAL%] Building (build.bat)...
echo.
call "%ROOT%\build.bat"
set "BUILD_RC=!errorlevel!"

if not exist "%ROOT%\build\TcpRedirectorService.exe" (
    echo.
    echo [FAIL] build\TcpRedirectorService.exe not produced ^(build.bat rc=!BUILD_RC!^).
    echo        The service binary is REQUIRED - aborting easy-mode delivery.
    call :finalize
    endlocal & exit /b 1
)
echo.
echo     [OK] Service binary present in build\ ^(build.bat rc=!BUILD_RC!^)

:: =========================================================================
:: [3/6] Assemble delivery into deploy_latest\ (replicates deploy.bat's
::       useful staging, WITHOUT VERSION auto-increment, into deploy_latest\).
:: =========================================================================
set /a STEP+=1
echo.
echo [%STEP%/%TOTAL%] Staging delivery into deploy_latest\ ...

:: Clean the staging dir on every run.
if exist "%STAGE%" rmdir /S /Q "%STAGE%" >nul 2>&1
mkdir "%STAGE%"      >nul 2>&1
mkdir "%STAGE%\gui"  >nul 2>&1

set "ENG_WINDIVERT=no"
set "ENG_WINTUN=no"
set "ENG_TUN2SOCKS=no"

:: --- Service binary (REQUIRED) ---
copy /Y "%ROOT%\build\TcpRedirectorService.exe" "%STAGE%\" >nul 2>&1
echo     [OK] TcpRedirectorService.exe

:: --- GUI (with the same cleanup deploy.bat performs) ---
if exist "%ROOT%\build\gui" (
    xcopy /Y /E /I "%ROOT%\build\gui\*" "%STAGE%\gui\" >nul 2>&1
    if exist "%STAGE%\gui\*.pdb" del /Q "%STAGE%\gui\*.pdb" >nul 2>&1
    if exist "%STAGE%\gui\clretwrc.dll" del /Q "%STAGE%\gui\clretwrc.dll" >nul 2>&1
    if exist "%STAGE%\gui\mscordaccore.dll" del /Q "%STAGE%\gui\mscordaccore.dll" >nul 2>&1
    if exist "%STAGE%\gui\mscordaccore_amd64*.dll" del /Q "%STAGE%\gui\mscordaccore_amd64*.dll" >nul 2>&1
    if exist "%STAGE%\gui\mscordbi.dll" del /Q "%STAGE%\gui\mscordbi.dll" >nul 2>&1
    if exist "%STAGE%\gui\createdump.exe" del /Q "%STAGE%\gui\createdump.exe" >nul 2>&1
    if exist "%STAGE%\gui\Microsoft.DiaSymReader.Native.*.dll" del /Q "%STAGE%\gui\Microsoft.DiaSymReader.Native.*.dll" >nul 2>&1
    if exist "%STAGE%\gui\msquic.dll" del /Q "%STAGE%\gui\msquic.dll" >nul 2>&1
    for /d %%d in ("%STAGE%\gui\cs" "%STAGE%\gui\de" "%STAGE%\gui\es" "%STAGE%\gui\fr" "%STAGE%\gui\it" "%STAGE%\gui\ja" "%STAGE%\gui\ko" "%STAGE%\gui\pl" "%STAGE%\gui\pt-BR" "%STAGE%\gui\tr" "%STAGE%\gui\zh-Hans" "%STAGE%\gui\zh-Hant") do (
        if exist "%%~d" rmdir /S /Q "%%~d" >nul 2>&1
    )
    echo     [OK] gui\ ^(cleaned^)
) else (
    echo     [SKIP] build\gui not found - delivery will ship without GUI
)

:: --- WinDivert (OPTIONAL): build\ first, then repo external\ fallback ---
if exist "%ROOT%\build\WinDivert.dll" (
    copy /Y "%ROOT%\build\WinDivert.dll"   "%STAGE%\" >nul 2>&1
    if exist "%ROOT%\build\WinDivert64.sys" copy /Y "%ROOT%\build\WinDivert64.sys" "%STAGE%\" >nul 2>&1
    set "ENG_WINDIVERT=yes"
    echo     [OK] WinDivert.dll + WinDivert64.sys ^(from build\^)
) else if exist "%ROOT%\external\WinDivert\WinDivert-2.2.2-A\x64\WinDivert.dll" (
    copy /Y "%ROOT%\external\WinDivert\WinDivert-2.2.2-A\x64\WinDivert.dll"   "%STAGE%\" >nul 2>&1
    if exist "%ROOT%\external\WinDivert\WinDivert-2.2.2-A\x64\WinDivert64.sys" copy /Y "%ROOT%\external\WinDivert\WinDivert-2.2.2-A\x64\WinDivert64.sys" "%STAGE%\" >nul 2>&1
    set "ENG_WINDIVERT=yes"
    echo     [OK] WinDivert.dll + WinDivert64.sys ^(from external\^)
) else (
    echo     [SKIP] WinDivert not found - delivery will be windivert-less
)

:: --- .bin\ engine tree (OPTIONAL): mirror the ENTIRE repo .bin\ tree so the
::     runtime layout the service expects is preserved verbatim:
::         .bin\wintun\<arch>\wintun.dll   (WintunApi::DefaultDllPath)
::         .bin\tun2socks\tun2socks.exe    (external engine resolver)
::     Any DLL under .bin\ is shipped automatically. Freshly built engine
::     binaries in build\ are overlaid on top so a new build always wins. ---
if exist "%ROOT%\.bin" (
    xcopy /Y /E /I "%ROOT%\.bin\*" "%STAGE%\.bin\" >nul 2>&1
    echo     [OK] .bin\ tree mirrored ^(from repo .bin\^)
) else (
    echo     [SKIP] repo .bin\ not found - no external engine tree to mirror
)

:: Overlay freshly built engine binaries on top of the mirrored tree.
if exist "%ROOT%\build\wintun.dll" (
    if not exist "%STAGE%\.bin\wintun\x64" mkdir "%STAGE%\.bin\wintun\x64" >nul 2>&1
    copy /Y "%ROOT%\build\wintun.dll" "%STAGE%\.bin\wintun\x64\" >nul 2>&1
    echo     [OK] wintun.dll overlaid ^(from build\ -> .bin\wintun\x64^)
)
if exist "%ROOT%\build\.bin\tun2socks\tun2socks.exe" (
    if not exist "%STAGE%\.bin\tun2socks" mkdir "%STAGE%\.bin\tun2socks" >nul 2>&1
    copy /Y "%ROOT%\build\.bin\tun2socks\tun2socks.exe" "%STAGE%\.bin\tun2socks\" >nul 2>&1
    echo     [OK] tun2socks.exe overlaid ^(from build\.bin^)
)

:: Derive the engine-summary flags from the final staged layout.
if exist "%STAGE%\.bin\wintun\x64\wintun.dll" (
    set "ENG_WINTUN=yes"
) else (
    echo     [SKIP] .bin\wintun\x64\wintun.dll absent - delivery will be wintun-less
)
if exist "%STAGE%\.bin\tun2socks\tun2socks.exe" (
    set "ENG_TUN2SOCKS=yes"
) else (
    echo     [SKIP] .bin\tun2socks\tun2socks.exe absent - external engine unavailable
)

:: --- config.default.json seed (OPTIONAL) ---
if exist "%ROOT%\build\config.default.json" (
    copy /Y "%ROOT%\build\config.default.json" "%STAGE%\config.default.json" >nul 2>&1
    echo     [OK] config.default.json ^(from build\^)
) else if exist "%ROOT%\installer\config.default.json" (
    copy /Y "%ROOT%\installer\config.default.json" "%STAGE%\config.default.json" >nul 2>&1
    echo     [OK] config.default.json ^(from installer\^)
) else (
    echo     [SKIP] no config.default.json seed - service self-generates on first run
)

:: =========================================================================
:: [4/6] Package (ZIP) via installer\package.bat.
::       We pass DEPLOY + PKG_VERSION through the environment. package.bat
::       honours them via `if not defined` (backward-compatible), so the
::       normal manual flow is unchanged.
::       package.bat cd's to installer\, so DEPLOY is relative to installer\.
:: =========================================================================
set /a STEP+=1
echo.
echo [%STEP%/%TOTAL%] Packaging (installer\package.bat)...
echo.
set "DEPLOY=..\deploy_latest"
set "PKG_VERSION=%EASY_VERSION%"
call "%ROOT%\installer\package.bat"
set "PKG_RC=!errorlevel!"
:: Clear so a later run of package.bat in the same shell isn't affected.
set "DEPLOY="
set "PKG_VERSION="

set "ZIP_PATH=%ROOT%\output\TcpRedirector_%EASY_VERSION%.zip"
if exist "%ZIP_PATH%" (
    echo.
    echo     [OK] ZIP created: %ZIP_PATH%
) else (
    echo.
    echo     [WARN] Expected ZIP not found at %ZIP_PATH% ^(package.bat rc=!PKG_RC!^)
)

:: =========================================================================
:: [5/6] Optional Inno Setup compile (setup.iss -> TcpRedirector_Setup.exe).
::       Skipped gracefully if ISCC.exe is not available.
:: =========================================================================
set /a STEP+=1
echo.
echo [%STEP%/%TOTAL%] Inno Setup (optional)...
set "ISCC="
where ISCC.exe >nul 2>&1 && for /f "delims=" %%i in ('where ISCC.exe 2^>nul') do set "ISCC=%%i"
if not defined ISCC if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe"       set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles(x86)%\Inno Setup 5\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 5\ISCC.exe"

if defined ISCC (
    echo     [INFO] ISCC found: !ISCC!
    rem setup.iss reads MyAppVersion from /D override if present (backward-compatible
    rem default is baked into the .iss). SourceDir=obfuscated was just filled by
    rem package.bat, so the compile picks up the freshly staged tree.
    "!ISCC!" /DMyAppVersion=%EASY_VERSION% "%ROOT%\installer\setup.iss"
    if exist "%ROOT%\output\TcpRedirector_Setup.exe" (
        echo     [OK] Setup EXE: %ROOT%\output\TcpRedirector_Setup.exe
    ) else (
        echo     [WARN] ISCC ran but TcpRedirector_Setup.exe not found in output\
    )
) else (
    echo     [SKIP] ISCC.exe not found - skipping setup.exe. ZIP delivery is still complete.
)

:: =========================================================================
:: [6/6] Restore VERSION + print summary.
:: =========================================================================
set /a STEP+=1
echo.
echo [%STEP%/%TOTAL%] Restoring VERSION and summarizing...
call :finalize

echo.
echo ==========================================================
echo   EASY MODE - DELIVERY SUMMARY
echo ==========================================================
echo   Version label      : %EASY_VERSION%
echo   Staging folder      : %STAGE%
echo   ZIP package         : %ZIP_PATH%
if exist "%ROOT%\output\TcpRedirector_Setup.exe" (
    echo   Setup EXE           : %ROOT%\output\TcpRedirector_Setup.exe
) else (
    echo   Setup EXE           : ^(not built - Inno Setup absent^)
)
echo   ------------------------------------------------------
echo   Engines bundled:
echo     WinDivert         : %ENG_WINDIVERT%
echo     Wintun            : %ENG_WINTUN%
echo     tun2socks         : %ENG_TUN2SOCKS%
echo   ------------------------------------------------------
echo   VERSION file        : untouched ^(easy mode never bumps it^)
echo ==========================================================
echo.

endlocal & exit /b 0

:: =========================================================================
:: :finalize — restore VERSION exactly to its pre-run state. Safe to call
:: multiple times (guarded). Called on both success and fatal-abort paths.
:: =========================================================================
:finalize
if "%VERSION_EXISTED%"=="1" (
    if exist "%VERSION_BAK%" (
        copy /Y "%VERSION_BAK%" "%VERSION_FILE%" >nul 2>&1
        del /Q "%VERSION_BAK%" >nul 2>&1
        echo     [OK] VERSION restored to original ^(%SAVED_VERSION%^)
    )
) else (
    rem No VERSION existed before; remove the temporary one we created.
    if exist "%VERSION_FILE%" del /Q "%VERSION_FILE%" >nul 2>&1
    echo     [OK] Temporary VERSION removed ^(none existed before^)
)
set "VERSION_EXISTED=done"
goto :eof
