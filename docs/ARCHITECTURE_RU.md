# Архитектура приложения — TcpRedirector v1.1.5

## 1. Общая архитектура

TcpRedirector реализует паттерн «Гексагональная архитектура» (Ports & Adapters) с разделением на слои:

```
┌─────────────────────────────────────────────────────────┐
│                    GUI (WPF .NET 9.0)                    │
│  Adapters/Driving/Wpf/                                  │
│    ├── ViewModels/  (MVVM: ShellViewModel, SettingsVM)  │
│    └── Controls/    (TrafficGraph)                      │
│  Infrastructure/                                         │
│    ├── Ipc/         (IpcClient → named pipe)            │
│    └── Config/      (JsonConfigRepository)              │
├─────────────────────────────────────────────────────────┤
│              C++ Service (TcpRedirectorService.exe)      │
│                                                          │
│  ┌─ adapters/driving/ ─────────────────────────────┐    │
│  │  ServiceMain.h     (Windows Service lifecycle)   │    │
│  │  IpcHandler.h      (JSON-RPC over named pipe)    │    │
│  └─────────────────────────────────────────────────┘    │
│                                                          │
│  ┌─ domain/ports/ ─────────────────────────────────┐    │
│  │  ICapture.h            (packet capture interface)│    │
│  │  IRelayServer.h        (relay server interface)  │    │
│  │  IAuthenticationProvider.h  (auth interface)     │    │
│  │  ILogSink.h            (logging interface)       │    │
│  │  IConnectionMonitor.h  (monitoring interface)    │    │
│  └─────────────────────────────────────────────────┘    │
│                                                          │
│  ┌─ infrastructure/ ───────────────────────────────┐    │
│  │  capture/WinDivertCapture  (WinDivert driver)    │    │
│  │  relay/TcpRelayServer      (HTTP CONNECT relay)  │    │
│  │  auth/KerberosAgentProvider (Kerberos via pipe)  │    │
│  │  auth/BasicAuthProvider     (Basic auth)         │    │
│  │  config/ConfigManager       (config.json I/O)    │    │
│  │  ipc/PipeServer             (named pipe server)  │    │
│  │  logging/Logger             (async file logger)  │    │
│  │  stats/StatsCollector       (connection stats)   │    │
│  └─────────────────────────────────────────────────┘    │
│                                                          │
│  ┌─ adapters/driven/ ──────────────────────────────┐    │
│  │  ProxyEngine.h    (legacy direct proxy connector)│    │
│  └─────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│           AuthAgent (TcpRedirectorAuthAgent.exe)         │
│  SspiEngine     (SSPI InitializeSecurityContext)        │
│  ContextStore   (in-memory context storage, TTL=5min)  │
│  Named Pipe     (\\.\pipe\TcpRedirectorAuth)            │
└─────────────────────────────────────────────────────────┘
```

## 2. Поток данных

### 2.1. Перехват пакета

```
Приложение (chrome.exe)
  │ TCP SYN → 1.2.3.4:443
  ▼
WinDivert (ядро)
  │ перехватывает исходящий TCP-пакет
  │ определяет PID процесса
  │ проверяет правила (CheckProcessRule)
  ▼
Перенаправление на 127.0.0.1:34010
```

### 2.2. Обработка соединения

```
Клиентское приложение
  │ TCP connect → 127.0.0.1:34010
  ▼
AcceptLoop (TcpRelayServer)
  │ accept() → client_sock
  ▼
HandleNewConnection
  │ поиск в ConnectionTable по src_port
  │ создание RelayContext
  ▼
ConnectionHandler (новый поток)
  │ connect() → upstream proxy
  │ HTTP CONNECT → прокси
  │ ├─ Basic Auth (один запрос)
  │ └─ Negotiate Auth (CreateContext → 407 → ContinueContext → 200)
  ▼
StartBridge (два потока)
  ├─ OneWayRelay UP   (client → proxy)  [RX bytes]
  └─ OneWayRelay DN   (proxy → client)  [TX bytes]
```

### 2.3. Поток аутентификации Kerberos

```
TcpRelayServer (SYSTEM)
  │ CreateContext("HTTP/proxy")
  ▼
KerberosAgentProvider
  │ Call("create_context", {spn})
  ▼
Named Pipe (\\.\pipe\TcpRedirectorAuth)
  ▼
AuthAgent (User Session)
  │ SspiEngine::CreateContext()
  │ InitializeSecurityContext() → SSPI
  │ Возвращает: token (base64), context_id
  ▼
TcpRelayServer
  │ CONNECT + "Proxy-Authorization: Negotiate <token>"
  ▼
Upstream Proxy
  │ 407 Proxy Auth Required
  │ "Proxy-Authenticate: Negotiate <challenge>"
  ▼
TcpRelayServer
  │ ContinueContext(context_id, challenge)
  ▼
AuthAgent
  │ InitializeSecurityContext(challenge) → SSPI
  │ Возвращает: final_token
  ▼
TcpRelayServer
  │ CONNECT + "Proxy-Authorization: Negotiate <final_token>"
  ▼
Upstream Proxy
  │ 200 Connection Established ✓
```

## 3. Ключевые компоненты

### 3.1. WinDivertCapture

**Файл**: [`WinDivertCapture.h`](../src/service/TcpRedirectorService/infrastructure/capture/WinDivertCapture.h)

- Загружает `WinDivert.dll` динамически (Late Binding)
- Фильтр: `outbound && tcp && tcp.Syn && !tcp.Ack` (только SYN-пакеты)
- Для каждого SYN: определение PID процесса, проверка правил
- При совпадении правила: редирект на `127.0.0.1:34010`
- Кэш PID (PID → имя процесса) для ускорения

### 3.2. TcpRelayServer

**Файл**: [`TcpRelayServer.h`](../src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h)

- Слушает `127.0.0.1:34010` (и [::1]:34010 для IPv6)
- `AcceptLoop`: главный поток приёма соединений
- `ConnectionHandler`: поток на соединение — подключается к upstream прокси, отправляет HTTP CONNECT, запускает bridge
- `StartBridge`: создаёт два потока `OneWayRelay` (UP/DN) для двунаправленной передачи данных
- Поддержка ретраев при 502/504 (до 3 попыток)
- `TCP_NODELAY` на всех сокетах (v1.1.5)

### 3.3. KerberosAgentProvider

**Файл**: [`KerberosAgentProvider.h`](../src/service/TcpRedirectorService/infrastructure/auth/KerberosAgentProvider.h)

- Подключается к AuthAgent через `\\.\pipe\TcpRedirectorAuth`
- JSON-RPC протокол: `create_context`, `continue_context`, `close_context`, `ping`
- Автоматический запуск агента через `CreateProcessAsUserW()` (WTS API)
- Keepalive: ping каждые 15 секунд
- **v1.1.5**: `std::recursive_mutex` защищает pipe I/O от гонок между потоками
- **v1.1.3**: `WarmUp()` — упреждающий запуск агента при старте сервиса

### 3.4. AuthAgent

**Файл**: [`SspiEngine.cpp`](../src/auth-agent/TcpRedirectorAuthAgent/SspiEngine.cpp)

- Отдельный процесс в пользовательской сессии (не SYSTEM)
- Использует SSPI (`InitializeSecurityContext`) для получения Kerberos/Negotiate токенов
- Хранилище контекстов в памяти с TTL 5 минут
- Named pipe сервер: `\\.\pipe\TcpRedirectorAuth`

### 3.5. IPC (Межпроцессное взаимодействие)

```
GUI ←→ Service:  \\.\pipe\TcpRedirectorService  (JSON-RPC)
Service ←→ Agent: \\.\pipe\TcpRedirectorAuth     (JSON-RPC)
```

### 3.6. GUI (WPF)

**Файлы**: `ShellViewModel.cs`, `SettingsViewModel.cs`

- MVVM паттерн (CommunityToolkit.Mvvm)
- Единственный писатель `config.json`
- Отображает: статус сервиса, статистику, график трафика, логи
- Позволяет управлять правилами и настройками аутентификации

## 4. Многопоточность

| Компонент | Модель | Примечание |
|-----------|--------|------------|
| AcceptLoop | 1 поток | select() по двум сокетам |
| ConnectionHandler | 1 поток на соединение | CreateThread() |
| OneWayRelay (UP) | 1 поток на мост | Блокирующий recv/send |
| OneWayRelay (DN) | 1 поток на мост | Выполняется в ConnectionHandler |
| KeepaliveLoop | 1 поток | Пинг AuthAgent |
| Logger | Очередь + 1 поток | Асинхронная запись |
| WinDivert | 1 поток | Блокирующий recv |

**Синхронизация:**
- `m_pipeMutex` (recursive) — защита pipe I/O в KerberosAgentProvider
- `pair->refs` (interlocked) — подсчёт ссылок RelayPair
- `m_activePairs` (atomic) — отслеживание активных мостов
- `m_connected` (atomic) — статус подключения к AuthAgent

## 5. Конфигурация

Файл: `%ProgramData%\TcpRedirector\config.json`

```json
{
  "proxy": { "host": "...", "port": 3128 },
  "auth": { "enabled": true, "kerberos": true, "authMode": "KerberosOnly" },
  "log": { "level": 2, "directory": "...", "max_file_size_mb": 10 },
  "log_rotation": { "max_files": 5, "compress": true },
  "rules": [ { "type": "process", "action": "proxy", "pattern": "chrome.exe" } ]
}
```

## 6. Поток сборки

```
C++ Service:    MSBuild /p:Configuration=Release /p:Platform=x64
                → TcpRedirectorService.exe

C++ AuthAgent:  MSBuild /p:Configuration=Release /p:Platform=x64
                → TcpRedirectorAuthAgent.exe

C# GUI:         dotnet build -c Release
                → TcpRedirectorGUI.exe + dependencies/

Пакет:          ZIP архив с bin/, docs/, config.json
```

## 7. Зависимости

| Зависимость | Версия | Назначение |
|-------------|--------|------------|
| WinDivert | 2.2 | Перехват пакетов (ядро) |
| nlohmann/json | 3.x | JSON-парсинг (header-only) |
| .NET 9.0 | 9.0.x | Рантайм для GUI |
| CommunityToolkit.Mvvm | 8.x | MVVM для GUI |
| Windows SDK | 10.0+ | Win32 API, SSPI, WTS |

## 8. История изменений архитектуры

| Версия | Изменение |
|--------|-----------|
| v1.1.0 | Гексагональная архитектура, Kerberos-аутентификация |
| v1.1.1 | Исправление SSPI-буфера, WarmUp(), логирование бриджей |
| v1.1.3 | Упреждающий запуск AuthAgent (WarmUp) |
| v1.1.4 | Пересоздание AuthProvider при reload конфига |
| v1.1.5 | TCP_NODELAY, recursive_mutex для pipe, try-catch в StartBridge |