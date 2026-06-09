@echo off
setlocal enabledelayedexpansion

echo ========================================
echo  TcpRedirector - Full Build
echo ========================================
echo.

set ROOT_DIR=%~dp0..\..

:: Check for VS2022
if not defined VS170COMNTOOLS (
    echo [ERROR] Visual Studio 2022 not detected.
    echo Please run from "Developer Command Prompt for VS 2022"
    exit /b 1
)

:: ==========================================
:: Phase 1: Build WFP Driver
:: ==========================================
echo [1/4] Building WFP Driver...
cd /d "%ROOT_DIR%\src\driver\TcpRedirectorDriver"

if exist "%ROOT_DIR%\build\driver\amd64" (
    echo Driver build directory exists, skipping MSBuild...
) else (
    msbuild TcpRedirectorDriver.vcxproj /p:Configuration=Release /p:Platform=x64 /p:Target=Build
    if errorlevel 1 (
        echo [ERROR] Driver build failed
        exit /b 1
    )
)

:: ==========================================
:: Phase 2: Build Service
:: ==========================================
echo [2/4] Building Windows Service...
cd /d "%ROOT_DIR%\src\service\TcpRedirectorService"

if exist "%ROOT_DIR%\build\service\Release" (
    echo Service build directory exists, skipping MSBuild...
) else (
    msbuild TcpRedirectorService.sln /p:Configuration=Release /p:Platform=x64
    if errorlevel 1 (
        echo [ERROR] Service build failed
        exit /b 1
    )
)

:: ==========================================
:: Phase 3: Build GUI
:: ==========================================
echo [3/4] Building GUI...
cd /d "%ROOT_DIR%\src\gui\TcpRedirectorGUI"

dotnet build TcpRedirectorGUI.csproj -c Release
if errorlevel 1 (
    echo [ERROR] GUI build failed
    exit /b 1
)

:: ==========================================
:: Phase 4: Copy outputs
:: ==========================================
echo [4/4] Copying build outputs...
mkdir "%ROOT_DIR%\build\final" 2>nul

:: TODO: Copy driver .sys, service .exe, GUI .exe to build\final

echo.
echo ========================================
echo  Build completed successfully!
echo ========================================
echo.
echo Outputs:
echo   Driver:  build\driver\amd64\TcpRedirectorDriver.sys
echo   Service: build\service\Release\TcpRedirectorService.exe
echo   GUI:     src\gui\TcpRedirectorGUI\bin\Release\net8.0-windows\TcpRedirectorGUI.exe
echo.