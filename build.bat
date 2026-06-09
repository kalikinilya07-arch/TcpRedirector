@echo off
title TcpRedirector — Build
cd /d "%~dp0"
set ROOT=%CD%

echo ========================================
echo  Building TcpRedirector
echo ========================================
echo.

:: Step 1: Unit Tests
echo [1/3] Compiling RuleEngine tests...
cd "%ROOT%\tests\unit\service"
cl /EHsc /std:c++20 /Fe:RuleEngineTest.exe RuleEngineTest.cpp /I "%ROOT%\src\service\TcpRedirectorService" /nologo 2>&1
if errorlevel 1 echo [FAIL] Tests compilation failed! & pause & exit /b 1

RuleEngineTest.exe 2>&1
if errorlevel 1 echo [FAIL] Tests failed! & pause & exit /b 1
echo [PASS]

:: Step 2: GUI
echo [2/3] Building GUI...
cd "%ROOT%\src\gui\TcpRedirectorGUI"
dotnet build TcpRedirectorGUI.csproj -c Release --nologo 2>&1
if errorlevel 1 echo [FAIL] & pause & exit /b 1
xcopy /Y /I "bin\Release\net10.0-windows\*" "%ROOT%\build\gui\" >nul 2>&1
echo [OK]

:: Step 3: Service
echo [3/3] Building Service...
cd "%ROOT%\src\service\TcpRedirectorService"
msbuild TcpRedirectorService.vcxproj /p:Configuration=Release /p:Platform=x64 /nologo 2>&1
if errorlevel 1 echo [FAIL] & pause & exit /b 1
copy /Y "build\service\x64\Release\TcpRedirectorService.exe" "%ROOT%\build\" >nul 2>&1
echo [OK]

echo.
echo ========================================
echo  Build complete!
echo  Run: run.bat
echo ========================================
pause