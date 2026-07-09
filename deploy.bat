@echo off
title TcpRedirector — Deploy
cd /d "%~dp0"
set ROOT=%CD%

:: Read version
set /p VERSION=<"%ROOT%\VERSION"
echo ========================================
echo  Deploying TcpRedirector v%VERSION%
echo ========================================
echo.

:: Check build exists
if not exist "%ROOT%\build\TcpRedirectorService.exe" (
    echo [FAIL] Build not found! Run build.bat first.
    pause
    exit /b 1
)

:: Create versioned release directory
set RELEASE_DIR=%ROOT%\releases\v%VERSION%
if exist "%RELEASE_DIR%" (
    echo [WARN] Release v%VERSION% already exists!
    echo        Delete it manually or bump VERSION file.
    pause
    exit /b 1
)

mkdir "%RELEASE_DIR%" >nul 2>&1
mkdir "%RELEASE_DIR%\gui" >nul 2>&1

:: Copy service binary
echo [1/4] Copying service binary...
copy /Y "%ROOT%\build\TcpRedirectorService.exe" "%RELEASE_DIR%\" >nul 2>&1
echo     OK

:: Copy GUI (cleaned)
echo [2/4] Copying GUI...
xcopy /Y /E /I "%ROOT%\build\gui\*" "%RELEASE_DIR%\gui\" >nul 2>&1

:: Remove unnecessary files from self-contained publish
if exist "%RELEASE_DIR%\gui\*.pdb" del /Q "%RELEASE_DIR%\gui\*.pdb" >nul 2>&1
if exist "%RELEASE_DIR%\gui\clretwrc.dll" del "%RELEASE_DIR%\gui\clretwrc.dll" >nul 2>&1
if exist "%RELEASE_DIR%\gui\mscordaccore.dll" del "%RELEASE_DIR%\gui\mscordaccore.dll" >nul 2>&1
if exist "%RELEASE_DIR%\gui\mscordaccore_amd64*.dll" del "%RELEASE_DIR%\gui\mscordaccore_amd64*.dll" >nul 2>&1
if exist "%RELEASE_DIR%\gui\mscordbi.dll" del "%RELEASE_DIR%\gui\mscordbi.dll" >nul 2>&1
if exist "%RELEASE_DIR%\gui\createdump.exe" del "%RELEASE_DIR%\gui\createdump.exe" >nul 2>&1
if exist "%RELEASE_DIR%\gui\Microsoft.DiaSymReader.Native.*.dll" del "%RELEASE_DIR%\gui\Microsoft.DiaSymReader.Native.*.dll" >nul 2>&1
if exist "%RELEASE_DIR%\gui\msquic.dll" del "%RELEASE_DIR%\gui\msquic.dll" >nul 2>&1
:: Remove non-ru language folders
for /d %%d in ("%RELEASE_DIR%\gui\cs" "%RELEASE_DIR%\gui\de" "%RELEASE_DIR%\gui\es" "%RELEASE_DIR%\gui\fr" "%RELEASE_DIR%\gui\it" "%RELEASE_DIR%\gui\ja" "%RELEASE_DIR%\gui\ko" "%RELEASE_DIR%\gui\pl" "%RELEASE_DIR%\gui\pt-BR" "%RELEASE_DIR%\gui\tr" "%RELEASE_DIR%\gui\zh-Hans" "%RELEASE_DIR%\gui\zh-Hant") do (
    if exist "%%d" rmdir /S /Q "%%d" >nul 2>&1
)
echo     OK

:: Copy WinDivert
echo [3/4] Copying WinDivert...
copy /Y "%ROOT%\build\WinDivert.dll" "%RELEASE_DIR%\" >nul 2>&1
copy /Y "%ROOT%\build\WinDivert64.sys" "%RELEASE_DIR%\" >nul 2>&1
echo     OK

:: Copy install.bat
echo [4/4] Copying installer...
copy /Y "%ROOT%\build\install.bat" "%RELEASE_DIR%\" >nul 2>&1
echo     OK

:: Auto-increment patch version
for /f "tokens=1,2,3 delims=." %%a in ("%VERSION%") do (
    set MAJOR=%%a
    set MINOR=%%b
    set PATCH=%%c
)
set /a NEW_PATCH=%PATCH%+1
set NEW_VERSION=%MAJOR%.%MINOR%.%NEW_PATCH%
echo %NEW_VERSION%> "%ROOT%\VERSION"

echo.
echo ========================================
echo  Release v%VERSION% created!
echo  Location: %RELEASE_DIR%
echo  Next version will be: v%NEW_VERSION%
echo ========================================
