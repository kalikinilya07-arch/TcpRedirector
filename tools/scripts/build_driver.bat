@echo off
title TcpRedirector — Build Driver
setlocal enabledelayedexpansion
cd /d "%~dp0..\.."

set SRC=src\driver\TcpRedirectorDriver
set OUT=build\driver
set WDK=C:\Program Files (x86)\Windows Kits\10
set VER=10.0.28000.0

echo === Building TcpRedirectorDriver (WDK %VER% / VS 2026) ===
echo.

:: Initialize VS 18 2026 environment
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
    echo [FAIL] Cannot initialize VC++ environment
    pause
    exit /b 1
)

:: Create output dir
mkdir %OUT% 2>nul

:: Include paths — local wfp first (overrides broken WDK fwpsk.h)
set CFLAGS=/nologo /c /GS- /W3 /WX- /O2 /Oi /Gy /Zl /kernel /DNDEBUG /D_KERNEL_MODE /DWIN32=100 /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A000000 /D_M_AMD64 /D_AMD64_ /D_WIN64 ^
    /I"%SRC%\infrastructure\wfp" ^
    /I"%WDK%\Include\%VER%\km" /I"%WDK%\Include\%VER%\shared" /I"%WDK%\Include\%VER%\um" ^
    /I"%WDK%\Include\%VER%\cppwinrt"

:: Compile
echo [COMPILE] DriverEntry.c...
cl.exe %CFLAGS% /Fo"%OUT%\DriverEntry.obj" /Fd"%OUT%\driver.pdb" ^
    "%SRC%\adapters\driving\DriverEntry.c"
if errorlevel 1 echo [FAIL] & pause & exit /b 1

echo [COMPILE] RedirectService.c...
cl.exe %CFLAGS% /Fo"%OUT%\RedirectService.obj" "%SRC%\domain\services\RedirectService.c"
if errorlevel 1 echo [FAIL] & pause & exit /b 1

echo [COMPILE] WfpCallout.c...
cl.exe %CFLAGS% /Fo"%OUT%\WfpCallout.obj" "%SRC%\infrastructure\wfp\WfpCallout.c"
if errorlevel 1 echo [FAIL] & pause & exit /b 1

echo [COMPILE] IoctlHandler.c...
cl.exe %CFLAGS% /Fo"%OUT%\IoctlHandler.obj" "%SRC%\infrastructure\ioctl\IoctlHandler.c"
if errorlevel 1 echo [FAIL] & pause & exit /b 1

:: Link
echo [LINK] TcpRedirectorDriver.sys...
link.exe /nologo /driver /subsystem:native /entry:DriverEntry /dynamicbase /nxcompat ^
    /out:"%OUT%\TcpRedirectorDriver.sys" ^
    "%OUT%\DriverEntry.obj" "%OUT%\RedirectService.obj" "%OUT%\WfpCallout.obj" "%OUT%\IoctlHandler.obj" ^
    "%WDK%\Lib\%VER%\km\x64\ntoskrnl.lib" ^
    "%WDK%\Lib\%VER%\km\x64\hal.lib" ^
    "%WDK%\Lib\%VER%\km\x64\fwpkclnt.lib" ^
    "%WDK%\Lib\%VER%\km\x64\wdmguid.lib" ^
    "%WDK%\Lib\%VER%\km\x64\libcntpr.lib" ^
    "%WDK%\Lib\%VER%\km\x64\bufferoverflowfastfailk.lib"
if errorlevel 1 (echo [FAIL]) else (echo [OK])

echo.
echo === Build complete ===
echo Driver: %OUT%\TcpRedirectorDriver.sys
pause
