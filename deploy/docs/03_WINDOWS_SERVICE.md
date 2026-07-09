# Windows Service — архитектура и реализация

## 1. Общая структура сервиса

```
TcpRedirectorService.exe
├── ServiceMain.h/.cpp           # Точка входа SCM + консольный режим
├── main.cpp                     # Точка входа: ServiceMain, --install, --uninstall, --console
│
├── adapters/
│   ├── driving/
│   │   ├── ServiceMain.h        # TcpRedirectorService — главный класс жизненного цикла
│   │   ├── IpcHandler.h         # Обработчик IPC-запросов от GUI (Named Pipe)
│   │   └── ProxyEngine.h        # Proxy-движок (bridge mode) — устаревший, не используется
│   │
│   └── driven/
│       └── (резерв)
│
├── domain/
│   ├── entities/
│   │   └── ProxyConfig.h        # Структуры: ProxyConfig, Rule, ConnectionRecord, LogEntry
│   ├── ports/
│   │   ├── ICapture.h           # Порт для захвата пакетов
│   │   ├── IConfigStore.h       # Порт для хранения конфигурации
│   │   ├── IConnectionMonitor.h # Порт мониторинга соединений
│   │   ├── IConnectionTable.h   # Порт таблицы соединений
│   │   ├── IProxyConnector.h    # Порт подключения к прокси
│   │   └── IRelayServer.h       # Порт relay-сервера
│   └── services/
│       ├── RuleEngine.h         # Движок правил маршрутизации
│       └── ConnectionTracker.h  # Отслеживание соединений
│
├── infrastructure/
│   ├── auth/
│   │   ├── auth_sspi.h          # SSPI-аутентификация (Negotiate/Kerberos)
│   │   └── auth_sspi.cpp        # Реализация SSPI
│   ├── capture/
│   │   ├── WinDivertCapture.h   # Захват TCP-пакетов через WinDivert
│   │   └── WinDivertCapture.cpp # Реализация захвата + DST modification
│   ├── config/
│   │   ├── Config.h             # Структура Config
│   │   ├── ConfigManager.h      # Управление конфигурацией
│   │   └── ConfigManager.cpp    # Загрузка/сохранение JSON + DPAPI
│   ├── ipc/
│   │   └── PipeServer.h         # Named Pipe сервер для GUI
│   ├── logging/
│   │   ├── Logger.h             # Асинхронный логгер
│   │   └── Logger.cpp           # Реализация логгера
│   ├── relay/
│   │   ├── TcpRelayServer.h     # Relay-сервер (принимает редиректы, делает CONNECT)
│   │   └── ConnectionTable.h    # Таблица активных соединений
│   └── stats/
│       ├── StatsCollector.h     # Сбор статистики
│       └── StatsCollector.cpp   # Реализация статистики
```

**Разработчик:** Kalikin Iliya

---

## 2. Принцип работы (DST-modification mode)

В отличие от традиционного WFP-редиректа (который описан в устаревшей документации), текущая реализация использует **WinDivert** для перехвата и модификации TCP-пакетов на сетевом уровне (LAYER_NETWORK).

### 2.1. Этапы работы

```
[Приложение]                    [WinDivertCapture]              [TcpRelayServer]            [HTTP Proxy]
     │                                │                              │                          │
     │── SYN (dst:example:443) ──────►│                              │                          │
     │                                │                              │                          │
     │                         1. WinDivert перехватывает SYN        │                          │
     │                         2. Определяет PID по src_port         │                          │
     │                         3. Проверяет правила (RuleEngine)     │                          │
     │                         4. Меняет dst на 127.0.0.1:relayPort  │                          │
     │                                │                              │                          │
     │── SYN (dst:127.0.0.1:relay)───►│                              │                          │
     │                                │────[redirect]───────────────►│                          │
     │                                │                              │                          │
     │                                │                    5. TcpRelayServer принимает          │
     │                                │                    6. Сохраняет оригинальный dst        │
     │                                │                    7. Отправляет CONNECT к прокси       │
     │                                │                              │──CONNECT example:443────►│
     │                                │                              │◄──200 OK─────────────────│
     │                                │                              │                          │
     │                                │                    8. Устанавливает bridge             │
     │◄══data═════════════════════════►│◄═════════════════════════════►════data════════════════►│
```

### 2.2. Ключевые компоненты

| Компонент | Файл | Назначение |
|-----------|------|------------|
| `WinDivertCapture` | `WinDivertCapture.h/.cpp` | Захват TCP-пакетов, определение PID, модификация DST |
| `TcpRelayServer` | `TcpRelayServer.h` | Приём редиректнутых соединений, HTTP CONNECT, bridge |
| `ConnectionTable` | `ConnectionTable.h` | Таблица port->(pid, path, bytes), SRWLock |
| `RuleEngine` | `RuleEngine.h` | Проверка правил (ProcessName, ProcessPath, Global) |
| `ConnectionTracker` | `ConnectionTracker.h` | Отслеживание активных соединений |
| `ConfigManager` | `ConfigManager.h/.cpp` | Загрузка/сохранение config.json |
| `Logger` | `Logger.h/.cpp` | Асинхронное логирование |
| `auth_sspi` | `auth_sspi.h/.cpp` | SSPI-аутентификация (Negotiate/Kerberos) |
| `PipeServer` | `PipeServer.h` | Named Pipe сервер для GUI |

---

## 3. Жизненный цикл сервиса

### 3.1. Запуск

```
main() (main.cpp)
  │
  ├── --install: создание Windows Service (CreateService)
  ├── --uninstall: удаление сервиса (DeleteService)
  ├── --console: запуск в консольном режиме
  └── (без аргументов): StartServiceCtrlDispatcher → ServiceMain
       │
       ▼
ServiceMain()
  │
  ├── RegisterServiceCtrlHandlerEx() — обработчик SCM команд
  ├── SetServiceStatus(SERVICE_START_PENDING)
  │
  ├── TcpRedirectorService::Initialize()
  │   │
  │   ├── Logger::Initialize()                  — инициализация логгера
  │   ├── ConfigManager::Load()                 — загрузка config.json
  │   ├── RuleEngine::SetRules()                — загрузка правил
  │   ├── ConnectionTracker                     — инициализация трекера
  │   ├── ConnectionTable                       — инициализация таблицы
  │   ├── TcpRelayServer::Start()               — запуск relay на порту 34010
  │   ├── WinDivertCapture::Open()              — запуск захвата пакетов
  │   ├── PipeServer::Start()                   — запуск IPC для GUI
  │   └── m_initialized = true
  │
  ├── SetServiceStatus(SERVICE_RUNNING)
  │
  └── TcpRedirectorService::Run()
      │
      └── while (m_running)
            sleep(1s)  — CaptureLoop и Relay работают в своих потоках
```

### 3.2. Остановка

```
SCM: SERVICE_CONTROL_STOP
  │
  ├── SetServiceStatus(SERVICE_STOP_PENDING)
  ├── TcpRedirectorService::Stop()
  │   ├── m_running = false
  │   ├── TcpRelayServer::Stop()
  │   ├── ConnectionTable::Clear()
  │   ├── PipeServer::Stop()
  │   ├── WinDivertCapture::Close()
  │   └── Logger::Shutdown()
  │
  └── SetServiceStatus(SERVICE_STOPPED)
```

---

## 4. Механизм DST modification

### 4.1. WinDivertCapture

**Файл:** [`WinDivertCapture.h`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/capture/WinDivertCapture.h), [`WinDivertCapture.cpp`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/capture/WinDivertCapture.cpp)

Работает на слое `WINDIVERT_LAYER_NETWORK` (перехват всех IP-пакетов).

**Алгоритм CaptureLoop:**
```
while (m_running) {
    WinDivertRecv(packet, &addr, &recvLen)
    HelperParsePacket(...) → IP/TCP заголовки
    
    if (TCP && SYN) {
        pid = FindPidBySourcePort(srcPort)    // через GetExtendedTcpTable
        if (IsTargetProcess(pid))              // проверка по пути процесса
            if (CheckProcessRule(...))         // RuleEngine
                DST modification: адрес → 127.0.0.1:relayPort
                SaveRedirectEvent(srcPort, dstIp, dstPort, pid)
    }
    
    WinDivertSend(packet)                      // отправка (изменённого) пакета
}
```

### 4.2. Битмап-кэш PID

Для оптимизации используется per-port битмап:
- Каждому src_port сопоставляется решение (DIRECT/PROXY/BLOCK)
- PID проверяется только для первого пакета порта
- Последующие пакеты используют кэшированное решение

### 4.3. TcpRelayServer

**Файл:** [`TcpRelayServer.h`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h)

Принимает TCP-соединения на relayPort (34010) от модифицированных пакетов.

**Алгоритм ConnectionHandler:**
```
accept() → новый клиент
  │
  ├── read() — ждём данные от клиента (первые байты после handshake)
  ├── ConnectionTable::Lookup(srcPort) → оригинальный dst:port, PID
  │
  ├── ConnectToProxy()
  │   ├── socket → connect(proxyHost, proxyPort)
  │   ├── if (m_kerberosAuth) SspiNegotiate() → токен Negotiate
  │   ├── Send CONNECT originalHost:originalPort HTTP/1.1
  │   └── Read response
  │
  ├── if (200 OK) → Start bridge loop
  │   ├── recv(client) → send(proxy)
  │   └── recv(proxy) → send(client)
  │
  └── On close → ConnectionTable::UpdateBytes()
```

---

## 5. Аутентификация на прокси

### 5.1. Basic-аутентификация

```cpp
// ProxyConfig.h
struct ProxyConfig {
    bool auth_required = false;   // Требуется ли авторизация
    std::wstring login;           // Логин
    bool has_password = false;    // Есть ли пароль (зашифрован DPAPI)
};
```

В CONNECT-запрос добавляется заголовок:
```
Proxy-Authorization: Basic base64(login:password)
```

### 5.2. Kerberos/Negotiate через SSPI

**Файл:** [`auth_sspi.h`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.h), [`auth_sspi.cpp`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp)

Включается флагом `kerberos_auth = true` в ProxyConfig.

**Алгоритм:**
```
1. AcquireCredentialsHandle("Negotiate") — учётные данные текущего пользователя
2. InitializeSecurityContext — первый токен (base64)
3. CONNECT + Proxy-Authorization: Negotiate <token>
4. Если 407 → Parse407Challenge → InitializeSecurityContext(challenge)
5. Повторный CONNECT с новым токеном
6. 200 OK → tunnel established
```

**Используемые флаги SSPI:**
- `ISC_REQ_CONFIDENTIALITY` — шифрование
- `ISC_REQ_REPLAY_DETECT` — защита от повторов
- `ISC_REQ_SEQUENCE_DETECT` — защита последовательности
- `ISC_REQ_DELEGATE` — делегирование учётных данных

---

## 6. Таблица соединений (ConnectionTable)

**Файл:** [`ConnectionTable.h`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/relay/ConnectionTable.h)

Хранит маппинг: `src_port → {pid, process_path, bytes_up, bytes_down}`

**Потокобезопасность:** SRWLock (читатели не блокируют друг друга)

**Хранение:** Хэш-таблица с колл-аут на 65536 элементов (по числу портов).

| Метод | Назначение |
|-------|------------|
| `Add(srcPort, pid, procPath)` | Добавить запись о соединении |
| `Lookup(srcPort)` | Найти запись по порту |
| `Remove(srcPort)` | Удалить запись |
| `AddBytes(srcPort, up, down)` | Обновить счётчики байт |
| `GetInfo(srcPort)` | Получить полную информацию |
| `GetSnapshot()` | Получить список всех записей |
| `Clear()` | Очистить таблицу |

---

## 7. Конфигурация

**Файл:** [`ConfigManager.h`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/config/ConfigManager.h), [`Config.h`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/config/Config.h)

**Путь:** `%ProgramData%\TcpRedirector\config.json`

**Структура:**
```json
{
    "app": {
        "exePath": "C:\\path\\to\\target.exe"
    },
    "proxy": {
        "host": "127.0.0.1",
        "port": 3128,
        "enabled": true
    },
    "auth": {
        "enabled": false,
        "username": "",
        "encryptedPassword": "",
        "kerberos": false
    },
    "log": {
        "level": 2,
        "fileEnabled": true,
        "maxSizeMB": 10
    }
}
```

- **DPAPI** для шифрования пароля (`CryptProtectData` с `CRYPTPROTECT_LOCAL_MACHINE`)
- **shared_mutex** для потокобезопасного доступа

---

## 8. Логирование

**Файл:** [`Logger.h`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/logging/Logger.h), [`Logger.cpp`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/logging/Logger.cpp)

**Архитектура:**
- Асинхронная очередь (`std::queue + mutex + condition_variable`)
- Отдельный WriterThread для записи в файл
- Ring buffer на 2000 записей для IPC (get_logs)
- Ротация по размеру файла
- Цветной вывод в консоль

**Уровни:** Error(0), Warn(1), Info(2), Debug(3), Trace(4)

---

## 9. IPC с GUI

**Файл:** [`PipeServer.h`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/ipc/PipeServer.h), [`IpcHandler.h`](TcpRedirector/src/service/TcpRedirectorService/adapters/driving/IpcHandler.h)

**Транспорт:** Named Pipe `\\.\pipe\TcpRedirectorService`

**Формат:** JSON (запрос/ответ)

**Поддерживаемые методы:**
| Метод | Назначение |
|-------|------------|
| `get_config` | Получить конфигурацию |
| `set_config` | Обновить конфигурацию |
| `get_rules` | Получить правила |
| `set_rules` | Обновить правила |
| `get_connections` | Активные соединения |
| `get_logs` | Последние записи лога |
| `get_stats` | Статистика |
| `get_status` | Статус сервиса |
| `service_stop` | Остановка сервиса |

---

## 10. Сборка и запуск

### 10.1. Требования

| Инструмент | Назначение |
|-----------|------------|
| Visual Studio 2022 | C++ компилятор (MSVC v143) |
| Windows SDK 10.0.26100+ | Windows API |
| WinDivert 2.2.2 | Перехват пакетов (WinDivert.dll + WinDivert64.sys) |

### 10.2. Сборка

```bash
msbuild TcpRedirectorService.vcxproj /p:Configuration=Release /p:Platform=x64
```

### 10.3. Установка

```bash
# Установка сервиса
TcpRedirectorService.exe --install

# Запуск сервиса
net start TcpRedirectorService

# Консольный режим (для отладки)
TcpRedirectorService.exe --console

# Удаление сервиса
net stop TcpRedirectorService
TcpRedirectorService.exe --uninstall
```

### 10.4. Файлы для работы

```
TcpRedirectorService.exe
WinDivert.dll
WinDivert64.sys
```

Все три файла должны быть в одной директории. WinDivert64.sys копируется в систему при первом запуске.

---

## 11. Разработчик

**Автор и разработчик проекта:** Kalikin Iliya

По всем вопросам и предложениям обращаться к автору проекта.