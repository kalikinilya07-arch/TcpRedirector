@echo off
title TcpRedirector — Build Installer
cd /d "%~dp0"
setlocal enabledelayedexpansion

set DEPLOY=..\deploy_v3
set OBFUSCATED=obfuscated
set OUTPUT=..\output

echo ========================================
echo  TcpRedirector Installer Builder
echo ========================================
echo.

:: ==========================================
:: Step 1: Clean
:: ==========================================
echo [1/3] Cleaning...
if exist "%OBFUSCATED%" rmdir /S /Q "%OBFUSCATED%" >nul 2>&1
if exist "%OUTPUT%" rmdir /S /Q "%OUTPUT%" >nul 2>&1
mkdir "%OBFUSCATED%\gui\ru" >nul 2>&1
mkdir "%OUTPUT%" >nul 2>&1
echo       OK

:: ==========================================
:: Step 2: Copy + Clean files
:: ==========================================
echo [2/3] Copying files...
xcopy /Y /Q "..\deploy_v3\gui\*" "%OBFUSCATED%\gui\" >nul 2>&1

:: Remove debug files
del /Q "%OBFUSCATED%\gui\*.pdb" >nul 2>&1
del /Q "%OBFUSCATED%\gui\createdump.exe" >nul 2>&1
del /Q "%OBFUSCATED%\gui\clretwrc.dll" >nul 2>&1
del /Q "%OBFUSCATED%\gui\mscordaccore.dll" >nul 2>&1
del /Q "%OBFUSCATED%\gui\mscordaccore_amd64*.dll" >nul 2>&1
del /Q "%OBFUSCATED%\gui\mscordbi.dll" >nul 2>&1
del /Q "%OBFUSCATED%\gui\Microsoft.DiaSymReader.Native.amd64.dll" >nul 2>&1
del /Q "%OBFUSCATED%\gui\msquic.dll" >nul 2>&1

:: Remove non-ru language folders
for /d %%d in (cs de es fr it ja ko pl pt-BR tr zh-Hans zh-Hant) do (
    rmdir /S /Q "%OBFUSCATED%\gui\%%d" >nul 2>&1
)

:: Copy Service + WinDivert
copy /Y "%DEPLOY%\TcpRedirectorService.exe" "%OBFUSCATED%\" >nul
copy /Y "%DEPLOY%\WinDivert.dll" "%OBFUSCATED%\" >nul
copy /Y "%DEPLOY%\WinDivert64.sys" "%OBFUSCATED%\" >nul
copy /Y "%DEPLOY%\config.json" "%OBFUSCATED%\" >nul
echo       OK

:: ==========================================
:: Step 3: Create install.bat + ZIP
:: ==========================================
echo [3/3] Creating installer package...

:: Create install.bat
(
echo @echo off
echo title TcpRedirector Setup
echo.
echo :: Check admin rights
echo net session ^>nul 2^>^&1
echo if errorlevel 1 ^(
echo     echo This installer requires Administrator privileges.
echo     echo Right-click install.bat ^> Run as Administrator.
echo     pause
echo     exit /b 1
echo ^)
echo.
echo echo ========================================
echo echo  TcpRedirector Setup
echo echo ========================================
echo echo.
echo set "APP_DIR=%%ProgramFiles%%\TcpRedirector"
echo set "APP_DATA=%%ProgramData%%\TcpRedirector"
echo.
echo echo [1/5] Creating directories...
echo mkdir "%%APP_DIR%%\gui" 2^>nul
echo mkdir "%%APP_DATA%%" 2^>nul
echo.
echo echo [2/5] Copying files...
echo xcopy /Y /E /Q "%%~dp0gui\*" "%%APP_DIR%%\gui\" ^>nul
echo copy /Y "%%~dp0TcpRedirectorService.exe" "%%APP_DIR%%\" ^>nul
echo copy /Y "%%~dp0WinDivert.dll" "%%APP_DIR%%\" ^>nul
echo copy /Y "%%~dp0WinDivert64.sys" "%%APP_DIR%%\" ^>nul
echo if not exist "%%APP_DATA%%\config.json" copy /Y "%%~dp0config.json" "%%APP_DATA%%\config.json" ^>nul
echo.
echo echo [3/5] Installing WinDivert driver...
echo sc create WinDivert binPath="%%APP_DIR%%\WinDivert64.sys" type=kernel start=demand ^>nul 2^>^&1
echo sc start WinDivert ^>nul 2^>^&1
echo.
echo echo [4/5] Installing TcpRedirector service...
echo "%%APP_DIR%%\TcpRedirectorService.exe" --install
echo sc start TcpRedirectorService ^>nul 2^>^&1
echo.
echo echo [5/5] Creating shortcuts...
echo powershell -Command "$ws = New-Object -ComObject WScript.Shell; $s = $ws.CreateShortcut([Environment]::GetFolderPath('CommonPrograms') + '\TcpRedirector.lnk'); $s.TargetPath = '%%APP_DIR%%\gui\TcpRedirectorGUI.exe'; $s.WorkingDirectory = '%%APP_DIR%%\gui'; $s.Save(); $s = $ws.CreateShortcut([Environment]::GetFolderPath('CommonDesktopDirectory') + '\TcpRedirector.lnk'); $s.TargetPath = '%%APP_DIR%%\gui\TcpRedirectorGUI.exe'; $s.WorkingDirectory = '%%APP_DIR%%\gui'; $s.Save()" ^>nul 2^>^&1
echo.
echo echo ========================================
echo echo  Installation complete!
echo echo  Launch: Start Menu ^> TcpRedirector
echo echo ========================================
echo pause
) > "%OBFUSCATED%\install.bat"

:: Create uninstall.bat
(
echo @echo off
echo title TcpRedirector Uninstall
echo net session ^>nul 2^>^&1 ^|^| ^(echo Run as Administrator! ^& pause ^& exit /b 1^)
echo set "APP_DIR=%%ProgramFiles%%\TcpRedirector"
echo set "APP_DATA=%%ProgramData%%\TcpRedirector"
echo sc stop TcpRedirectorService ^>nul 2^>^&1
echo "%%APP_DIR%%\TcpRedirectorService.exe" --uninstall ^>nul 2^>^&1
echo sc stop WinDivert ^>nul 2^>^&1
echo sc delete WinDivert ^>nul 2^>^&1
echo rmdir /S /Q "%%APP_DIR%%" 2^>nul
echo echo Uninstalled. Config kept at %%APP_DATA%%
echo pause
) > "%OBFUSCATED%\uninstall.bat"

:: Create ZIP
powershell -Command "Compress-Archive -Path '%OBFUSCATED%\*' -DestinationPath '%OUTPUT%\TcpRedirector_1.0.1.zip' -Force" 2>&1

echo.
echo ========================================
echo  DONE!
echo  Package: ..\output\TcpRedirector_1.0.1.zip
echo ========================================
echo.
echo To install:
echo   1. Extract TcpRedirector_1.0.1.zip
echo   2. Right-click install.bat ^> Run as Administrator
echo   3. Launch from Start Menu
echo.
echo Included:
echo   - TcpRedirector GUI (self-contained, .NET 9.0 inside)
echo   - TcpRedirector Service
echo   - WinDivert driver + DLL
echo   - config.json (default)
echo   - install.bat / uninstall.bat
echo.
pause
