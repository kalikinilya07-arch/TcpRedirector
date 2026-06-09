@echo off
setlocal enabledelayedexpansion

title TcpRedirector — Download Dependencies

set ROOT_DIR=%~dp0..\..
set DEPS_DIR=%ROOT_DIR%\external

echo ========================================
echo  Downloading TcpRedirector dependencies
echo ========================================
echo.

if not exist "%DEPS_DIR%" mkdir "%DEPS_DIR%"

:: nlohmann/json (header-only)
echo [1/3] nlohmann/json...
if not exist "%DEPS_DIR%\json.hpp" (
    curl -L --ssl-no-revoke -o "%DEPS_DIR%\json.hpp" ^
        "https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp" >nul 2>&1
    if errorlevel 1 (
        echo   Error downloading json.hpp
    ) else (
        echo   OK
    )
) else ( echo   Already exists )

:: spdlog (full headers)
echo [2/3] spdlog...
if not exist "%DEPS_DIR%\spdlog\spdlog.h" (
    curl -L --ssl-no-revoke -o "%TEMP%\spdlog.zip" ^
        "https://github.com/gabime/spdlog/archive/refs/heads/v1.x.zip" >nul 2>&1
    if errorlevel 1 (
        echo   Error downloading spdlog
    ) else (
        powershell -command "Expand-Archive -Path '%TEMP%\spdlog.zip' -DestinationPath '%TEMP%\spdlog_extract' -Force" >nul 2>&1
        if exist "%TEMP%\spdlog_extract\spdlog-1.x\include\spdlog" (
            move /Y "%TEMP%\spdlog_extract\spdlog-1.x\include\spdlog" "%DEPS_DIR%\spdlog\" >nul 2>&1
            echo   OK
        ) else ( echo   Error extracting )
        del "%TEMP%\spdlog.zip" 2>nul
    )
) else ( echo   Already exists )

:: boost (minimal - header only asio parts)
echo [3/3] boost-asio...
if not exist "%DEPS_DIR%\boost\asio.hpp" (
    powershell -command "& {param([string]$url,[string]$out);[Net.ServicePointManager]::SecurityProtocol='tls12';$wc=New-Object Net.WebClient;$wc.DownloadFile($url,$out)}" ^
        -url "https://github.com/boostorg/asio/archive/refs/tags/boost-1.86.0.zip" ^
        -out "%TEMP%\asio.zip" >nul 2>&1
    if errorlevel 1 (
        echo   Error downloading boost.asio
    ) else (
        powershell -command "Expand-Archive -Path '%TEMP%\asio.zip' -DestinationPath '%TEMP%\asio_extract' -Force" >nul 2>&1
        if exist "%TEMP%\asio_extract\asio-boost-1.86.0\include" (
            mkdir "%DEPS_DIR%\boost" 2>nul
            move /Y "%TEMP%\asio_extract\asio-boost-1.86.0\include\*" "%DEPS_DIR%\boost\" >nul 2>&1
            echo   OK
        ) else ( echo   Error extracting )
        del "%TEMP%\asio.zip" 2>nul
    )
) else ( echo   Already exists )

:: Also get boost.config and boost.type_traits (asio dependencies)
echo [3b/3] boost-config and dependencies...
if not exist "%DEPS_DIR%\boost\config.hpp" (
    powershell -command "& {$url='https://github.com/boostorg/config/archive/refs/tags/boost-1.86.0.zip';$out='%TEMP%\bconfig.zip';$wc=New-Object Net.WebClient;$wc.DownloadFile($url,$out)}" >nul 2>&1
    powershell -command "Expand-Archive -Path '%TEMP%\bconfig.zip' -DestinationPath '%TEMP%\bconfig_extract' -Force" >nul 2>&1
    if exist "%TEMP%\bconfig_extract" (
        xcopy /E /Y "%TEMP%\bconfig_extract\config-boost-1.86.0\include\*" "%DEPS_DIR%\boost\" >nul 2>&1
        echo   OK
    )
    del "%TEMP%\bconfig.zip" 2>nul
)

if not exist "%DEPS_DIR%\boost\type_traits.hpp" (
    powershell -command "& {$url='https://github.com/boostorg/type_traits/archive/refs/tags/boost-1.86.0.zip';$out='%TEMP%\btt.zip';$wc=New-Object Net.WebClient;$wc.DownloadFile($url,$out)}" >nul 2>&1
    powershell -command "Expand-Archive -Path '%TEMP%\btt.zip' -DestinationPath '%TEMP%\btt_extract' -Force" >nul 2>&1
    if exist "%TEMP%\btt_extract" (
        xcopy /E /Y "%TEMP%\btt_extract\type_traits-boost-1.86.0\include\*" "%DEPS_DIR%\boost\" >nul 2>&1
        echo   OK
    )
    del "%TEMP%\btt.zip" 2>nul
)

if not exist "%DEPS_DIR%\boost\assert.hpp" (
    powershell -command "& {$url='https://github.com/boostorg/assert/archive/refs/tags/boost-1.86.0.zip';$out='%TEMP%\bassert.zip';$wc=New-Object Net.WebClient;$wc.DownloadFile($url,$out)}" >nul 2>&1
    powershell -command "Expand-Archive -Path '%TEMP%\bassert.zip' -DestinationPath '%TEMP%\bassert_extract' -Force" >nul 2>&1
    if exist "%TEMP%\bassert_extract" (
        xcopy /E /Y "%TEMP%\bassert_extract\assert-boost-1.86.0\include\*" "%DEPS_DIR%\boost\" >nul 2>&1
        echo   OK
    )
    del "%TEMP%\bassert.zip" 2>nul
)

if not exist "%DEPS_DIR%\boost\core\addressof.hpp" (
    powershell -command "& {$url='https://github.com/boostorg/core/archive/refs/tags/boost-1.86.0.zip';$out='%TEMP%\bcore.zip';$wc=New-Object Net.WebClient;$wc.DownloadFile($url,$out)}" >nul 2>&1
    powershell -command "Expand-Archive -Path '%TEMP%\bcore.zip' -DestinationPath '%TEMP%\bcore_extract' -Force" >nul 2>&1
    if exist "%TEMP%\bcore_extract" (
        xcopy /E /Y "%TEMP%\bcore_extract\core-boost-1.86.0\include\*" "%DEPS_DIR%\boost\" >nul 2>&1
        echo   OK
    )
    del "%TEMP%\bcore.zip" 2>nul
)

if not exist "%DEPS_DIR%\boost\throw_exception.hpp" (
    powershell -command "& {$url='https://github.com/boostorg/throw_exception/archive/refs/tags/boost-1.86.0.zip';$out='%TEMP%\bthrow.zip';$wc=New-Object Net.WebClient;$wc.DownloadFile($url,$out)}" >nul 2>&1
    powershell -command "Expand-Archive -Path '%TEMP%\bthrow.zip' -DestinationPath '%TEMP%\bthrow_extract' -Force" >nul 2>&1
    if exist "%TEMP%\bthrow_extract" (
        xcopy /E /Y "%TEMP%\bthrow_extract\throw_exception-boost-1.86.0\include\*" "%DEPS_DIR%\boost\" >nul 2>&1
        echo   OK
    )
    del "%TEMP%\bthrow.zip" 2>nul
)

if not exist "%DEPS_DIR%\boost\system\error_code.hpp" (
    powershell -command "& {$url='https://github.com/boostorg/system/archive/refs/tags/boost-1.86.0.zip';$out='%TEMP%\bsystem.zip';$wc=New-Object Net.WebClient;$wc.DownloadFile($url,$out)}" >nul 2>&1
    powershell -command "Expand-Archive -Path '%TEMP%\bsystem.zip' -DestinationPath '%TEMP%\bsystem_extract' -Force" >nul 2>&1
    if exist "%TEMP%\bsystem_extract" (
        xcopy /E /Y "%TEMP%\bsystem_extract\system-boost-1.86.0\include\*" "%DEPS_DIR%\boost\" >nul 2>&1
        echo   OK
    )
    del "%TEMP%\bsystem.zip" 2>nul
)

echo.
echo ========================================
echo  Dependencies ready!
echo  Includes: external\json.hpp, external\spdlog, external\boost
echo ========================================
echo.