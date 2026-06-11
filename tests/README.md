# TcpRedirector — Изолированные тестовые заглушки

Директория `tests/` содержит автономные утилиты для тестирования TCP/UDP
соединений без зависимостей от основного проекта TcpRedirector.

## Состав

| Файл | Назначение |
|------|-----------|
| `packet_generator.cpp` | Генерирует TCP/UDP пакеты на указанный IP:port |
| `mock_proxy.cpp` | Минимальный TCP/UDP сервер, логирующий входящие данные |
| `run_test.bat` | Компиляция и запуск всех тестов |

## Как использовать

### 1. Быстрый запуск

```cmd
# Из папки tests/
run_test.bat
```

Батник предложит выбрать тест (1-7). Для автоматического выбора:

```cmd
run_test.bat 1    # TCP: mock_proxy :3128 + packet_generator
run_test.bat 2    # UDP: mock_proxy :3128 + packet_generator
run_test.bat 6    # Проверка через TcpRedirectorService
```

### 2. Интеграционное тестирование с TcpRedirectorService

В отдельном терминале:

```cmd
# Терминал 1: тестовый прокси
cd build\service
mock_proxy.exe --port 3128 --protocol tcp

# Терминал 2: TcpRedirectorService
cd build\service
TcpRedirectorService.exe --console
```

### 3. Раздельная компиляция руками

```cmd
# Developer Command Prompt for VS 2022
cl.exe packet_generator.cpp /Fe:build\packet_generator.exe /link ws2_32.lib
cl.exe mock_proxy.cpp /Fe:build\mock_proxy.exe /link ws2_32.lib