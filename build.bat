@echo off
title TcpRedirector — Build
cd /d "%~dp0"
set ROOT=%CD%

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

:: Clean build directory (temporary — releases go to releases\)
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

:: Step 2: GUI — self-contained publish
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
msbuild TcpRedirectorService.vcxproj /p:Configuration=Release /p:Platform=x64 /nologo 2>&1
if errorlevel 1 echo [FAIL] & pause & exit /b 1
copy /Y "build\service\x64\Release\TcpRedirectorService.exe" "%ROOT%\build\" >nul 2>&1
echo [OK]

:: Copy WinDivert.dll + WinDivert64.sys to build directory
if exist "%ROOT%\external\WinDivert\WinDivert-2.2.2-A\x64\WinDivert.dll" (
    copy /Y "%ROOT%\external\WinDivert\WinDivert-2.2.2-A\x64\WinDivert.dll" "%ROOT%\build\" >nul 2>&1
    copy /Y "%ROOT%\external\WinDivert\WinDivert-2.2.2-A\x64\WinDivert64.sys" "%ROOT%\build\" >nul 2>&1
) else if exist "%ROOT%\deploy\WinDivert.dll" (
    copy /Y "%ROOT%\deploy\WinDivert.dll" "%ROOT%\build\" >nul 2>&1
    if exist "%ROOT%\deploy\WinDivert64.sys" (
        copy /Y "%ROOT%\deploy\WinDivert64.sys" "%ROOT%\build\" >nul 2>&1
    )
)

echo.
echo ========================================
echo  Build complete!
echo  Output: %ROOT%\build\
echo  Next:   deploy.bat  (creates versioned release)
echo ========================================
