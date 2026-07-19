@echo off
REM ---------------------------------------------------------------------------
REM run.bat - запуск заглушки прокси-сервера с имитацией Kerberos (Windows).
REM
REM По умолчанию слушает 127.0.0.1:8888 в режиме accept-any (пропускает любую
REM аутентификацию). Все аргументы после run.bat передаются в stub_proxy.py.
REM
REM Примеры:
REM   run.bat
REM   run.bat --port 3128 --auth-mode challenge
REM   run.bat --config config.example.json
REM   run.bat --log-format json --reveal-secrets
REM ---------------------------------------------------------------------------

setlocal
cd /d "%~dp0"

REM Определяем интерпретатор Python (py launcher или python из PATH).
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 stub_proxy.py %*
) else (
    python stub_proxy.py %*
)

endlocal
