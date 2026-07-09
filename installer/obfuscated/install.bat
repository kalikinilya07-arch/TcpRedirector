@echo off
title TcpRedirector Setup

:: Check admin rights
net session >nul 2>&1
if errorlevel 1 (
    echo This installer requires Administrator privileges.
    echo Right-click install.bat > Run as Administrator.
    pause
    exit /b 1
)

echo ========================================
echo  TcpRedirector Setup
echo ========================================
echo.
set "APP_DIR=%ProgramFiles%\TcpRedirector"
set "APP_DATA=%ProgramData%\TcpRedirector"

echo [1/5] Creating directories...
mkdir "%APP_DIR%\gui" 2>nul
mkdir "%APP_DATA%" 2>nul

echo [2/5] Copying files...
xcopy /Y /E /Q "%~dp0gui\*" "%APP_DIR%\gui\" >nul
copy /Y "%~dp0TcpRedirectorService.exe" "%APP_DIR%\" >nul
copy /Y "%~dp0WinDivert.dll" "%APP_DIR%\" >nul
copy /Y "%~dp0WinDivert64.sys" "%APP_DIR%\" >nul
if not exist "%APP_DATA%\config.json" copy /Y "%~dp0config.json" "%APP_DATA%\config.json" >nul

echo [3/5] Installing WinDivert driver...
sc create WinDivert binPath="%APP_DIR%\WinDivert64.sys" type=kernel start=demand >nul 2>&1
sc start WinDivert >nul 2>&1

echo [4/5] Installing TcpRedirector service...
"%APP_DIR%\TcpRedirectorService.exe" --install
sc start TcpRedirectorService >nul 2>&1

echo [5/5] Creating shortcuts...
powershell -Command "$ws = New-Object -ComObject WScript.Shell; $s = $ws.CreateShortcut([Environment]::GetFolderPath('CommonPrograms') + '\TcpRedirector.lnk'); $s.TargetPath = '%APP_DIR%\gui\TcpRedirectorGUI.exe'; $s.WorkingDirectory = '%APP_DIR%\gui'; $s.Save(); $s = $ws.CreateShortcut([Environment]::GetFolderPath('CommonDesktopDirectory') + '\TcpRedirector.lnk'); $s.TargetPath = '%APP_DIR%\gui\TcpRedirectorGUI.exe'; $s.WorkingDirectory = '%APP_DIR%\gui'; $s.Save()" >nul 2>&1

echo ========================================
echo  Installation complete
echo  Launch: Start Menu > TcpRedirector
echo ========================================
pause
