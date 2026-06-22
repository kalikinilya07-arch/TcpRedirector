@echo off
setlocal

set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
set PROJ=C:\Users\user\Desktop\TcpRedirector\src\service\TcpRedirectorService\TcpRedirectorService.vcxproj
set BUILD=C:\Users\user\Desktop\TcpRedirector\build_refactored

if not exist "%BUILD%" mkdir "%BUILD%"

call %VCVARS% x64 >nul

echo === Building TcpRedirectorService (refactored) ===
echo Build log: %BUILD%\build_log.txt
msbuild "%PROJ%" /p:Configuration=Release /p:Platform=x64 /p:OutDir="%BUILD%\\" /t:Build /verbosity:minimal > "%BUILD%\build_log.txt" 2>&1

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo === BUILD FAILED ===
    type "%BUILD%\build_log.txt"
    echo.
    echo Build log saved to: %BUILD%\build_log.txt
    pause
    exit /b 1
)

echo.
echo === Build SUCCESS ===
echo Files in build_refactored:
dir "%BUILD%\*.exe" /b
echo.

echo === Copying WinDivert files ===
copy /Y "C:\Users\user\Desktop\TcpRedirector\external\WinDivert\WinDivert-2.2.2-A\x64\WinDivert.dll" "%BUILD%\" >nul
copy /Y "C:\Users\user\Desktop\TcpRedirector\external\WinDivert\WinDivert-2.2.2-A\x64\WinDivert64.sys" "%BUILD%\" >nul
echo Done.