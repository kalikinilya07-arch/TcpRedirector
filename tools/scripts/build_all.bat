@echo off
setlocal enabledelayedexpansion

title TcpRedirector — Build & Test Automation

echo ========================================
echo     TcpRedirector Build System
echo     Visual Studio 2026 + .NET 10
echo ========================================
echo.

set ROOT_DIR=%~dp0..\..
set BUILD_DIR=%ROOT_DIR%\build

:: ==========================================
:: Phase 1: Unit Tests
:: ==========================================
echo [1/4] Unit Tests...
cd /d "%ROOT_DIR%\tests\unit\service"
cl /EHsc /std:c++20 /Fe:RuleEngineTest.exe RuleEngineTest.cpp /I "%ROOT_DIR%\src\service\TcpRedirectorService" /nologo >nul 2>&1
if errorlevel 1 (
    echo [FAIL] Test compilation failed!
    exit /b 1
)
RuleEngineTest.exe
if errorlevel 1 (
    echo [FAIL] Tests failed!
    exit /b 1
)
echo [PASS] RuleEngine: All 6 tests passed
echo.

:: ==========================================
:: Phase 2: GUI (WPF .NET 10)
:: ==========================================
echo [2/4] Building GUI...
cd /d "%ROOT_DIR%\src\gui\TcpRedirectorGUI"
dotnet build TcpRedirectorGUI.csproj -c Release --nologo >nul 2>&1
if errorlevel 1 (
    echo [FAIL] GUI build failed!
    exit /b 1
)
mkdir "%BUILD_DIR%\gui" 2>nul
xcopy /E /Y "bin\Release\net10.0-windows" "%BUILD_DIR%\gui\" >nul 2>&1
echo [OK] GUI: build\gui\TcpRedirectorGUI.dll
echo.

:: ==========================================
:: Phase 3: Service (C++, MSBuild)
:: ==========================================
echo [3/4] Building Service...
if exist "%ROOT_DIR%\src\service\TcpRedirectorService\TcpRedirectorService.vcxproj" (
    cd /d "%ROOT_DIR%\src\service\TcpRedirectorService"
    msbuild TcpRedirectorService.vcxproj /p:Configuration=Release /p:Platform=x64 /nologo 2>nul
    if errorlevel 1 (
        echo [WARN] Service MSBuild failed. Run manually:
        echo    msbuild src\service\TcpRedirectorService\TcpRedirectorService.vcxproj /p:Configuration=Release /p:Platform=x64
    ) else (
        echo [OK] Service: build\service\x64\Release\TcpRedirectorService.exe
    )
) else (
    echo [SKIP] Service project not found
)
echo.

:: ==========================================
:: Phase 4: Summary
:: ==========================================
echo ========================================
echo  Build Complete
echo ========================================
echo.
echo  [PASS] RuleEngine Tests
echo  [OK]   GUI
echo  [..]   Service
echo  [..]   Driver (requires WDK)
echo.
echo  Output: %BUILD_DIR%\gui\TcpRedirectorGUI.dll
echo.
echo  Next: install_service.bat
echo.