@echo off
echo ========================================
echo   Downloading Npcap SDK
echo ========================================
echo.

cd /d "%~dp0..\.."

set SDK_DIR=external\npcap-sdk
set SDK_ZIP=%TEMP%\npcap-sdk.zip

if exist "%SDK_DIR%\Include\pcap.h" (
    echo Npcap SDK already installed.
    goto :done
)

echo Downloading Npcap SDK...
powershell -Command "& { [Net.ServicePointManager]::SecurityProtocol = 'tls12'; $wc = New-Object Net.WebClient; $wc.DownloadFile('https://npcap.com/dist/npcap-sdk-1.80.zip', '%SDK_ZIP%') }"
if errorlevel 1 (
    echo [FAIL] Download failed. Install manually from https://npcap.com
    pause
    exit /b 1
)

echo Extracting...
mkdir "%SDK_DIR%" 2>nul
powershell -Command "Expand-Archive -Path '%SDK_ZIP%' -DestinationPath '%SDK_DIR%' -Force"
del "%SDK_ZIP%" 2>nul

echo [OK] Npcap SDK installed to %SDK_DIR%

:done
echo.
echo SDK ready. Include: %SDK_DIR%\Include
echo Library: %SDK_DIR%\Lib\x64\wpcap.lib