@echo off
setlocal

set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
set PROJ=C:\Users\user\Desktop\TcpRedirector\src\service\TcpRedirectorService\TcpRedirectorService.vcxproj
set BUILD=C:\Users\user\Desktop\TcpRedirector\build

if not exist "%BUILD%" mkdir "%BUILD%"

call %VCVARS% x64 >nul

echo === Building TcpRedirectorService ===
msbuild "%PROJ%" /p:Configuration=Release /p:Platform=x64 /p:OutDir="%BUILD%\\" /t:Build /verbosity:minimal

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo === BUILD FAILED ===
    exit /b 1
)

echo.
echo === Build SUCCESS ===
copy /Y "%BUILD%\TcpRedirectorService.exe" "%BUILD%\" >nul
echo Files in build:
dir "%BUILD%\*.exe" /b
echo.
echo === Copying WinDivert files ===
copy /Y "C:\Users\user\Desktop\TcpRedirector\external\WinDivert\WinDivert-2.2.2-A\x64\WinDivert.dll" "%BUILD%\" >nul
copy /Y "C:\Users\user\Desktop\TcpRedirector\external\WinDivert\WinDivert-2.2.2-A\x64\WinDivert64.sys" "%BUILD%\" >nul
echo Done.

endlocal