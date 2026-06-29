@echo off
title TcpRedirector — Build
cd /d "%~dp0"
set ROOT=%CD%

echo ========================================
echo  Building TcpRedirector
echo ========================================
echo.

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

:: Step 1: Unit Tests
echo [1/3] Compiling RuleEngine tests...
cd /d "%ROOT%\tests\unit\service"
cl /EHsc /std:c++20 /Fe:RuleEngineTest.exe RuleEngineTest.cpp /I "%ROOT%\src\service\TcpRedirectorService" /nologo 2>&1
if errorlevel 1 echo [FAIL] Tests compilation failed! & pause & exit /b 1

RuleEngineTest.exe 2>&1
if errorlevel 1 echo [FAIL] Tests failed! & pause & exit /b 1
echo [PASS]

:: Step 2: GUI
echo [2/3] Building GUI...
cd /d "%ROOT%\src\gui\TcpRedirectorGUI"
dotnet build TcpRedirectorGUI.csproj -c Release --nologo 2>&1
if errorlevel 1 echo [FAIL] & pause & exit /b 1
xcopy /Y /I "bin\Release\net10.0-windows\*" "%ROOT%\build\gui\" >nul 2>&1
echo [OK]

:: Step 3: Service
echo [3/3] Building Service...
cd /d "%ROOT%\src\service\TcpRedirectorService"
msbuild TcpRedirectorService.vcxproj /p:Configuration=Release /p:Platform=x64 /nologo 2>&1
if errorlevel 1 echo [FAIL] & pause & exit /b 1
copy /Y "build\service\x64\Release\TcpRedirectorService.exe" "%ROOT%\build\" >nul 2>&1
echo [OK]

copy /Y "build\service\x64\Release\*.exe" "%ROOT%\build\" >nul 2>&1

echo.
echo ========================================
echo  Build complete!
echo  Output: %ROOT%\build\
echo  Run: deploy.bat
echo ========================================
pause
