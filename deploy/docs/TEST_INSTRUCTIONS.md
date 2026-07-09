# Инструкция по тестированию — полные пути и пошаговая последовательность

## Полные пути

| Что | Путь |
|-----|------|
| **service exe** | `C:\Users\user\Desktop\TcpRedirector\src\service\TcpRedirectorService\build\service\x64\Release\TcpRedirectorService.exe` |
| **config.json** | `C:\ProgramData\TcpRedirector\config.json` |
| **лог-файл** | `C:\ProgramData\TcpRedirector\logs\tcp_redirector.log` |
| **test tools** | `C:\Users\user\Desktop\TcpRedirector\tests\` |
| **packet_generator** | `C:\Users\user\Desktop\TcpRedirector\tests\packet_generator.exe` |
| **mock_proxy** | `C:\Users\user\Desktop\TcpRedirector\tests\mock_proxy.exe` |
| **WinDivert.dll** | `C:\Users\user\Desktop\TcpRedirector\build\service\WinDivert.dll` |

---

## Полноценный ручной тест (пошагово)

### Шаг 0: Подготовка config.json

Создать (или проверить, что создался) `C:\ProgramData\TcpRedirector\config.json`:

```json
{
    "app": {
        "exePath": "C:\\Projects\\china\\police_sec\\TransfersClient.exe"
    },
    "proxy": {
        "host": "127.0.0.1",
        "port": 8888,
        "enabled": true
    },
    "auth": {
        "enabled": false,
        "username": "",
        "encryptedPassword": "",
        "kerberos": false
    },
    "log": {
        "level": 3,
        "fileEnabled": true,
        "maxSizeMB": 10
    },
    "stats": {
        "updateIntervalMs": 2000
    }
}
```

> Уровень лога `3` = DEBUG — увидите все детали.

---

### Шаг 1: Компиляция тестовых инструментов

Открыть **обычный cmd** (не администратор):

```cmd
cd C:\Users\user\Desktop\TcpRedirector\tests
g++ packet_generator.cpp -o packet_generator.exe -lws2_32
g++ mock_proxy.cpp -o mock_proxy.exe -lws2_32
```

---

### Шаг 2: Тест connectivity (TCP, без WinDivert)

**Терминал 1** — запустить mock_proxy:

```cmd
cd C:\Users\user\Desktop\TcpRedirector\tests
start "mock_proxy" /B mock_proxy.exe --port 8888 --protocol tcp --log proxy_log.txt
```

**Терминал 2** — запустить packet_generator:

```cmd
cd C:\Users\user\Desktop\TcpRedirector\tests
packet_generator.exe --dest_ip 127.0.0.1 --dest_port 8888 --count 5 --protocol tcp
```

Ожидаемый вывод packet_generator:
```
[TCP] Sent 45 bytes -> 127.0.0.1:8888
[TCP] Recv 53 bytes: HTTP/1.1 200 Connection established
```

**Проверка:**

```cmd
type C:\Users\user\Desktop\TcpRedirector\tests\proxy_log.txt
```

Должен быть непустой — значит TCP connectivity работает.

**Остановить mock_proxy:**

```cmd
taskkill /f /im mock_proxy.exe > nul 2>&1
```

---

### Шаг 3: Проверка WinDivert.dll

Проверить, что WinDivert.dll есть:

```cmd
dir C:\Users\user\Desktop\TcpRedirector\build\service\WinDivert.dll
```

Если файла нет — скопировать из папки с WinDivert SDK:
```cmd
copy "C:\path\to\WinDivert.dll" C:\Users\user\Desktop\TcpRedirector\build\service\
```

---

### Шаг 4: Полный интеграционный тест (администратор)

**Открыть cmd.exe от имени администратора.**

```cmd
cd C:\Users\user\Desktop\TcpRedirector\tests

:: Проверить, что все .exe есть
dir packet_generator.exe
dir mock_proxy.exe
dir ..\src\service\TcpRedirectorService\build\service\x64\Release\TcpRedirectorService.exe

:: Запустить полный тест
run_integration_test.bat
```

Ожидаемый вывод:
```
========================================
    Integration Test
========================================
[INFO] packet_generator.exe found
[INFO] mock_proxy.exe found
[INFO] TcpRedirectorService.exe found
[INFO] WinDivert.dll found
[INFO] Running as ADMIN

--- Phase 1: connectivity test ---
Phase 1: PASS

--- Phase 2: full cycle (WinDivert) ---
[INFO] Starting mock_proxy on port 8888...
[INFO] Starting TcpRedirectorService...
[INFO] Running packet_generator...
[INFO] Stopping services...
Phase 2: PASS

========================================
    RESULT: ALL TESTS PASSED
========================================
```

Если Phase 2 не проходит — проверьте:
1. WinDivert.dll рядом с .exe
2. WinDivert64.sys установлен (`net start WinDivert`)
3. Запуск от администратора

---

### Шаг 5: Проверка логов ConfigManager

После запуска проверить, что config.json прочитался:

```cmd
type C:\ProgramData\TcpRedirector\logs\tcp_redirector.log
```

Ожидаемые строки:
```
[INFO ] [service    ] Proxy set from config: 127.0.0.1:8888
[INFO ] [service    ] Rule set from config for: TransfersClient.exe, proxy=enabled
[INFO ] [service    ] Target process set from config: C:\Projects\china\police_sec\TransfersClient.exe
[INFO ] [service    ] Proxy engine initialized
[INFO ] [service    ] WinDivert capture started
[INFO ] [service    ] TcpRedirector Service initialized successfully
```

---

### Шаг 6: Остановка и очистка

```cmd
:: Остановить TcpRedirectorService (если запущен как сервис)
net stop TcpRedirectorService

:: Или если в консольном режиме — нажать Ctrl+C

:: Убить процессы
taskkill /f /im TcpRedirectorService.exe > nul 2>&1
taskkill /f /im mock_proxy.exe > nul 2>&1

:: Почистить тестовые логи (опционально)
del C:\Users\user\Desktop\TcpRedirector\tests\proxy_log.txt 2>nul
```

---

## Быстрый smoke-тест (на одном терминале)

```cmd
:: 1. Собрать тестовые утилиты
cd C:\Users\user\Desktop\TcpRedirector\tests
g++ packet_generator.cpp -o packet_generator.exe -lws2_32
g++ mock_proxy.cpp -o mock_proxy.exe -lws2_32

:: 2. Запустить mock
start "mock" /B mock_proxy.exe --port 8888 --protocol tcp --log proxy_log.txt
timeout /t 1 > nul

:: 3. Запустить packet_generator
packet_generator.exe --dest_ip 127.0.0.1 --dest_port 8888 --count 1 --protocol tcp >nul

:: 4. Проверить
findstr "PACKET" proxy_log.txt && echo connectivity OK || echo connectivity FAIL

:: 5. Остановить
taskkill /f /im mock_proxy.exe > nul 2>&1
```

## Разовые unit тесты (Catch2)

```cmd
cd C:\Users\user\Desktop\TcpRedirector\tests\build
ctest --output-on-failure
```
