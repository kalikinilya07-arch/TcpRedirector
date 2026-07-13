@echo off
cd /d "%~dp0"
set ROOT=%CD%
echo Testing conditional copies...

if exist "%ROOT%\.bin\wintun\x64\wintun.dll" (
    copy /Y "%ROOT%\.bin\wintun\x64\wintun.dll" "%ROOT%\build\" >nul 2>&1
    echo     [OK] wintun.dll copied
) else (
    echo     [SKIP] wintun.dll not found
)

if exist "%ROOT%\.bin\tun2socks\tun2socks.exe" (
    if not exist "%ROOT%\build\.bin\tun2socks" mkdir "%ROOT%\build\.bin\tun2socks" >nul 2>&1
    copy /Y "%ROOT%\.bin\tun2socks\tun2socks.exe" "%ROOT%\build\.bin\tun2socks\" >nul 2>&1
    echo     [OK] tun2socks.exe copied
) else (
    echo     [SKIP] tun2socks.exe not found
)

if exist "%ROOT%\installer\config.default.json" (
    copy /Y "%ROOT%\installer\config.default.json" "%ROOT%\build\config.default.json" >nul 2>&1
    echo     [OK] config.default.json seed copied
) else (
    echo     [SKIP] no seed
)

echo Done.
