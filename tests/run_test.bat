@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

echo ============================================================
echo  TcpRedirector Test Runner
echo  Изолированные тестовые заглушки
echo ============================================================
echo.

:: Определение директорий
set TESTS_DIR=%~dp0
set BUILD_DIR=%TESTS_DIR%build

:: Компиляция, если нужно
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

:: Проверка наличия компилятора
where cl.exe >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [TEST] Компилятор MSVC cl.exe не найден в PATH.
    echo [TEST] Запустите 'Developer Command Prompt for VS 2022' или укажите путь к cl.exe.
    echo [TEST] Пробуем найти через vswhere...
    
    where vswhere >nul 2>&1
    if !ERRORLEVEL! equ 0 (
        for /f "tokens=*" %%i in ('vswhere -latest -find "**\Hostx64\x64\cl.exe"') do set CL_PATH=%%i
        if defined CL_PATH (
            echo [TEST] Найден: !CL_PATH!
            set "CC=!CL_PATH!"
        ) else (
            echo [TEST] vswhere не нашёл cl.exe. Укажите путь вручную.
            exit /b 1
        )
    ) else (
        echo [TEST] vswhere не найден. Установите Visual Studio Build Tools или VS 2022.
        exit /b 1
    )
) else (
    set "CC=cl.exe"
)

echo [TEST] Компилятор: %CC%
echo.

:: Компиляция packet_generator
echo [TEST] Компиляция packet_generator.exe...
%CC% "%TESTS_DIR%packet_generator.cpp" /Fe"%BUILD_DIR%\packet_generator.exe" /link ws2_32.lib /nologo
if %ERRORLEVEL% neq 0 (
    echo [TEST] ОШИБКА: Компиляция packet_generator не удалась
    exit /b 1
)
echo [TEST] packet_generator.exe создан.

:: Компиляция mock_proxy
echo [TEST] Компиляция mock_proxy.exe...
%CC% "%TESTS_DIR%mock_proxy.cpp" /Fe"%BUILD_DIR%\mock_proxy.exe" /link ws2_32.lib /nologo
if %ERRORLEVEL% neq 0 (
    echo [TEST] ОШИБКА: Компиляция mock_proxy не удалась
    exit /b 1
)
echo [TEST] mock_proxy.exe создан.
echo.

:: Выбор теста
if "%1"=="" (
    echo Выберите тест:
    echo   1 - TCP тест: mock_proxy :3128 - packet_generator :3128
    echo   2 - UDP тест: mock_proxy :3128 (UDP) - packet_generator :3128 (UDP)
    echo   3 - TCP-цикл: 10 пакетов на 3128
    echo   4 - TCP-цикл: 50 пакетов на 3128
    echo   5 - Прямое TCP-соединение: packet_generator на 127.0.0.1:9999 (без mock_proxy)
    echo   6 - Тест с внешним TcpRedirectorService на 127.0.0.1:3128
    echo   7 - Все тесты последовательно
    echo.
    set /p TEST_CHOICE="Введите номер теста (1-7): "
) else (
    set TEST_CHOICE=%1
)

echo.
echo [TEST] Запуск теста #%TEST_CHOICE%...
echo.

if "%TEST_CHOICE%"=="1" goto :test_tcp
if "%TEST_CHOICE%"=="2" goto :test_udp
if "%TEST_CHOICE%"=="3" goto :test_tcp_10
if "%TEST_CHOICE%"=="4" goto :test_tcp_50
if "%TEST_CHOICE%"=="5" goto :test_tcp_direct
if "%TEST_CHOICE%"=="6" goto :test_external_proxy
if "%TEST_CHOICE%"=="7" goto :test_all
echo [TEST] Неверный выбор: %TEST_CHOICE%
exit /b 1

:: ==========================================
:: Тест 1: TCP mock_proxy + packet_generator
:: ==========================================
:test_tcp
echo ---- Тест 1: TCP через mock_proxy :3128 ----

:: Запуск mock_proxy в фоне
start "mock_proxy" /B "%BUILD_DIR%\mock_proxy.exe" --port 3128 --protocol tcp
echo [TEST] mock_proxy запущен на TCP :3128 (PID: !ERRORLEVEL!)
:: Даём время на старт
timeout /t 1 /nobreak >nul

:: Запуск packet_generator
echo [TEST] Отправка TCP-пакета на 127.0.0.1:3128...
"%BUILD_DIR%\packet_generator.exe" --dest_ip 127.0.0.1 --dest_port 3128 --protocol tcp --count 1

:: Останов mock_proxy
echo [TEST] Останов mock_proxy...
taskkill /f /im mock_proxy.exe >nul 2>&1
timeout /t 1 /nobreak >nul
goto :end

:: ==========================================
:: Тест 2: UDP mock_proxy + packet_generator
:: ==========================================
:test_udp
echo ---- Тест 2: UDP через mock_proxy :3128 ----

start "mock_proxy" /B "%BUILD_DIR%\mock_proxy.exe" --port 3128 --protocol udp
echo [TEST] mock_proxy запущен на UDP :3128
timeout /t 1 /nobreak >nul

"%BUILD_DIR%\packet_generator.exe" --dest_ip 127.0.0.1 --dest_port 3128 --protocol udp --count 1

taskkill /f /im mock_proxy.exe >nul 2>&1
timeout /t 1 /nobreak >nul
goto :end

:: ==========================================
:: Тест 3: TCP 10 пакетов
:: ==========================================
:test_tcp_10
echo ---- Тест 3: TCP 10 пакетов через mock_proxy :3128 ----

start "mock_proxy" /B "%BUILD_DIR%\mock_proxy.exe" --port 3128 --protocol tcp
timeout /t 1 /nobreak >nul

"%BUILD_DIR%\packet_generator.exe" --dest_ip 127.0.0.1 --dest_port 3128 --protocol tcp --count 10

taskkill /f /im mock_proxy.exe >nul 2>&1
timeout /t 1 /nobreak >nul
goto :end

:: ==========================================
:: Тест 4: TCP 50 пакетов
:: ==========================================
:test_tcp_50
echo ---- Тест 4: TCP 50 пакетов через mock_proxy :3128 ----

start "mock_proxy" /B "%BUILD_DIR%\mock_proxy.exe" --port 3128 --protocol tcp
timeout /t 1 /nobreak >nul

"%BUILD_DIR%\packet_generator.exe" --dest_ip 127.0.0.1 --dest_port 3128 --protocol tcp --count 50

taskkill /f /im mock_proxy.exe >nul 2>&1
timeout /t 1 /nobreak >nul
goto :end

:: ==========================================
:: Тест 5: Прямое TCP-соединение (без proxy)
:: ==========================================
:test_tcp_direct
echo ---- Тест 5: Прямое TCP-соединение на 127.0.0.1:9999 (без mock_proxy) ----
echo [TEST] Этот тест проверит, что packet_generator корректно сообщает
echo [TEST] WSAECONNREFUSED, когда порт 9999 не слушает.
echo.

"%BUILD_DIR%\packet_generator.exe" --dest_ip 127.0.0.1 --dest_port 9999 --protocol tcp --count 1

echo.
echo [TEST] Если вы видите сообщение "TCP-соединение отклонено" — packet_generator работает верно.
timeout /t 2 /nobreak >nul
goto :end

:: ==========================================
:: Тест 6: Внешний TcpRedirectorService
:: ==========================================
:test_external_proxy
echo ---- Тест 6: Проверка через TcpRedirectorService на 127.0.0.1:3128 ----
echo [TEST] Убедитесь, что TcpRedirectorService запущен (в режиме --console).
echo [TEST] packet_generator отправит пакеты, которые должен перехватить
echo [TEST] WinDivert и перенаправить на прокси 127.0.0.1:3128.
echo.

echo [TEST] Шаг 1: Запускаем mock_proxy на :3128, чтобы симулировать прокси-сервер
start "mock_proxy" /B "%BUILD_DIR%\mock_proxy.exe" --port 3128 --protocol tcp
timeout /t 1 /nobreak >nul

echo [TEST] Шаг 2: Отправляем 3 TCP-пакета на 127.0.0.1:3128
"%BUILD_DIR%\packet_generator.exe" --dest_ip 127.0.0.1 --dest_port 3128 --protocol tcp --count 3

echo [TEST] Шаг 3: Ожидание обработки...
timeout /t 2 /nobreak >nul

echo [TEST] Шаг 4: Отправляем ещё 5 UDP-пакетов на 127.0.0.1:3128
"%BUILD_DIR%\packet_generator.exe" --dest_ip 127.0.0.1 --dest_port 3128 --protocol udp --count 5

taskkill /f /im mock_proxy.exe >nul 2>&1
timeout /t 1 /nobreak >nul
goto :end

:: ==========================================
:: Тест 7: Все тесты последовательно
:: ==========================================
:test_all
echo ---- Тест 7: Все тесты последовательно ----
echo.

call :test_tcp
echo --------------------------------
call :test_udp
echo --------------------------------
call :test_tcp_10
echo --------------------------------
call :test_tcp_direct
echo --------------------------------
call :test_external_proxy
echo --------------------------------
echo [TEST] Все тесты выполнены.
goto :end

:: ==========================================
:: Завершение
:: ==========================================
:end
echo.
echo [TEST] Завершено. Проверьте вывод выше на наличие ошибок.
echo [TEST] mock_proxy логи пишет в stdout.
echo.
echo [TEST] Следующий шаг: запустите TcpRedirectorService.exe --console
echo [TEST] в build\service\ параллельно с mock_proxy.exe --port 3128,
echo [TEST] затем packet_generator.exe от имени TransfersClient.exe.
endlocal