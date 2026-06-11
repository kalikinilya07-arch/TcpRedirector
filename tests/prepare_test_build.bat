@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM  prepare_test_build.bat — подготовка тестовой сборки TcpRedirector
REM  
REM  Модифицирует ServiceMain.h для тестовых целей:
REM    - Прокси: 127.0.0.1:8888
REM    - Правило: packet_generator.exe -> Proxy
REM  
REM  Собирает программу, копирует .exe в tests/
REM  Восстанавливает оригинальный ServiceMain.h
REM ============================================================
cd /d "%~dp0"

set SERVICE_DIR=%~dp0..\src\service\TcpRedirectorService
set SERVICE_H=%SERVICE_DIR%\adapters\driving\ServiceMain.h
set BACKUP_H=%SERVICE_DIR%\adapters\driving\ServiceMain.h.backup
set VCPROJ=%SERVICE_DIR%\TcpRedirectorService.vcxproj
set OUT_EXE=%~dp0TcpRedirectorService_test.exe

echo ============================================================
echo   PREPARE TEST BUILD — TcpRedirector
echo ============================================================
echo.

REM -------------------------------------------------------
REM  Шаг 1: Проверка наличия исходников
REM -------------------------------------------------------
if not exist "%SERVICE_H%" (
    echo [ОШИБКА] ServiceMain.h не найден: %SERVICE_H%
    pause
    exit /b 1
)
echo [OK] ServiceMain.h: %SERVICE_H%

REM -------------------------------------------------------
REM  Шаг 2: Backup оригинального ServiceMain.h
REM -------------------------------------------------------
echo.
echo [ШАГ 2/6] Создание backup'а...
copy /Y "%SERVICE_H%" "%BACKUP_H%" > nul
if !errorlevel! equ 0 (
    echo [OK] Backup создан: %BACKUP_H%
) else (
    echo [ОШИБКА] Не удалось создать backup!
    pause
    exit /b 1
)

REM -------------------------------------------------------
REM  Шаг 3: Модификация ServiceMain.h для теста
REM -------------------------------------------------------
echo.
echo [ШАГ 3/6] Модификация ServiceMain.h...

REM Используем PowerShell для точной замены строк (кодировка UTF-8)
powershell -Command ^
    "function Patch-File { $content = Get-Content '%SERVICE_H%' -Raw; " ^
    "$content = $content -replace 'hardcoded\.port = 3128;', 'hardcoded.port = 8888;'; " ^
    "$content = $content -replace 'hardcoded\.host = L\"127\.0\.0\.1\";', 'hardcoded.host = L\"127.0.0.1\";'; " ^
    "$content = $content -replace 'transfersRule\.pattern = L\"C:\\\\Projects\\\\china\\\\police_sec\\\\TransfersClient\.exe\";', 'transfersRule.pattern = L\"packet_generator.exe\";'; " ^
    "$content = $content -replace 'transfersRule\.description = L\"TransfersClient\";', 'transfersRule.description = L\"packet_generator for test\";'; " ^
    "$content = $content -replace 'transfersRule\.id = \"transfers-client\";', 'transfersRule.id = \"packet-gen-test\";'; " ^
    "Set-Content '%SERVICE_H%' -Value $content -Encoding UTF8; " ^
    "Write-Host '[OK] ServiceMain.h модифицирован'" ^
    ""

if !errorlevel! neq 0 (
    echo [ОШИБКА] PowerShell-модификация не удалась. Восстанавливаю backup...
    copy /Y "%BACKUP_H%" "%SERVICE_H%" > nul
    pause
    exit /b 1
)

REM Проверка что замена сработала
findstr "hardcoded.port = 8888" "%SERVICE_H%" > nul
if !errorlevel! equ 0 (
    echo [OK] Порт изменён: 3128 -^> 8888
) else (
    echo [WARN] Не удалось проверить замену порта (возможно кодировка)
    echo        Продолжаем сборку...
)

findstr "packet_generator.exe" "%SERVICE_H%" > nul
if !errorlevel! equ 0 (
    echo [OK] Правило изменено: TransfersClient.exe -^> packet_generator.exe
) else (
    echo [WARN] Не удалось проверить замену правила
)

REM -------------------------------------------------------
REM  Шаг 4: Поиск MSBuild
REM -------------------------------------------------------
echo.
echo [ШАГ 4/6] Сборка проекта...

set MSBUILD=
where msbuild.exe > tmp_msbuild.txt 2>nul
set /p MSBUILD=<tmp_msbuild.txt
del tmp_msbuild.txt 2>nul

if "!MSBUILD!"=="" (
    echo [ОШИБКА] MSBuild не найден! Установите Visual Studio Build Tools.
    echo          Восстанавливаю оригинал...
    copy /Y "%BACKUP_H%" "%SERVICE_H%" > nul
    echo [OK] Оригинал восстановлен
    pause
    exit /b 1
)
echo [OK] MSBuild: !MSBUILD!

REM -------------------------------------------------------
REM  Шаг 5: Сборка через MSBuild
REM -------------------------------------------------------
echo [СБОРКА] Запуск MSBuild...
echo         Это может занять 1-2 минуты...
echo.

echo         Конфигурация: Release | x64
echo         Выход: build\service\x64\Release\TcpRedirectorService.exe
echo.

"!MSBUILD!" "%VCPROJ%" /p:Configuration=Release /p:Platform=x64 /t:Rebuild /nologo /v:q
set BUILD_RESULT=!errorlevel!

if !BUILD_RESULT! neq 0 (
    echo.
    echo [ОШИБКА] Сборка не удалась (код: !BUILD_RESULT!).
    echo          Возможно нужно запустить из "Developer Command Prompt for VS 2022".
    echo          Восстанавливаю оригинал...
    copy /Y "%BACKUP_H%" "%SERVICE_H%" > nul
    echo [OK] Оригинал восстановлен
    pause
    exit /b 1
)

echo [OK] Сборка завершена успешно

REM -------------------------------------------------------
REM  Шаг 6: Копирование .exe в tests/
REM -------------------------------------------------------
echo.
echo [ШАГ 6/6] Копирование .exe...

set BUILT_EXE=%~dp0..\build\service\x64\Release\TcpRedirectorService.exe

if not exist "%BUILT_EXE%" (
    echo [WARN] %BUILT_EXE% не найден, ищу альтернативы...
    dir /b "%~dp0..\build\service\*TcpRedirectorService*.exe" > tmp_exe.txt 2>nul
    for /f "tokens=*" %%F in (tmp_exe.txt) do set BUILT_EXE=%%F
    del tmp_exe.txt 2>nul
)

if exist "!BUILT_EXE!" (
    copy /Y "!BUILT_EXE!" "%OUT_EXE%" > nul
    if !errorlevel! equ 0 (
        echo [OK] Скопирован: "!BUILT_EXE!" -^> "%OUT_EXE%"
    ) else (
        echo [WARN] Не удалось скопировать "!BUILT_EXE!"
    )
) else (
    echo [WARN] Собранный .exe не найден
    echo        Ищите в: %~dp0..\build\service\
)

REM -------------------------------------------------------
REM  Восстановление оригинального ServiceMain.h
REM -------------------------------------------------------
echo.
echo [ВОССТАНОВЛЕНИЕ] Возвращаем оригинальный ServiceMain.h...
copy /Y "%BACKUP_H%" "%SERVICE_H%" > nul
if !errorlevel! equ 0 (
    del "%BACKUP_H%" > nul 2>&1
    echo [OK] Оригинал восстановлен, backup удалён
) else (
    echo [WARN] Не удалось восстановить оригинал!
    echo        Ручное восстановление: copy "%BACKUP_H%" "%SERVICE_H%"
)

echo.
echo ============================================================
echo   ГОТОВО!
echo ============================================================
echo.
echo   Тестовая сборка: %OUT_EXE%
echo   Порт прокси:      8888
echo   Правило:          packet_generator.exe -^> Proxy
echo   Для запуска:      run_integration_test.bat
echo ============================================================

endlocal