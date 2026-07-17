# Документация для разработчика — TcpRedirector v1.1.5

## 1. Структура кодовой базы

```
TcpRedirector/
├── src/
│   ├── service/TcpRedirectorService/    # C++ Windows Service
│   │   ├── main.cpp                      # Точка входа сервиса (install/run/uninstall)
│   │   ├── adapters/driving/             # Первичные адаптеры (входящие)
│   │   │   ├── ServiceMain.h             # Жизненный цикл Windows Service (SCM)
│   │   │   └── IpcHandler.h              # JSON-RPC через named pipe (GUI ↔ Service)
│   │   ├── adapters/driven/              # Вторичные адаптеры (исходящие)
│   │   │   └── ProxyEngine.h             # Устаревший прямой коннектор (до relay)
│   │   ├── domain/
│   │   │   ├── ports/                    # Интерфейсы (I*)
│   │   │   │   ├── ICapture.h            # Интерфейс перехвата пакетов
│   │   │   │   ├── IRelayServer.h        # Интерфейс relay-сервера
│   │   │   │   ├── IAuthenticationProvider.h  # Интерфейс аутентификации
│   │   │   │   ├── ILogSink.h            # Интерфейс логирования
│   │   │   │   ├── IConnectionTable.h    # Отслеживание соединений
│   │   │   │   └── IConnectionMonitor.h  # Интерфейс мониторинга
│   │   │   └── entities/
│   │   │       └── ProxyConfig.h         # DTO: ProxyConfig, Rule, LogLevel и др.
│   │   └── infrastructure/               # Реализации
│   │       ├── capture/
│   │       │   └── WinDivertCapture.*    # Интеграция с драйвером WinDivert
│   │       ├── relay/
│   │       │   ├── TcpRelayServer.h      # HTTP CONNECT relay (header-only)
│   │       │   └── ConnectionTable.h     # Хеш-таблица соединений
│   │       ├── auth/
│   │       │   ├── KerberosAgentProvider.* # Kerberos через AuthAgent (pipe IPC)
│   │       │   ├── BasicAuthenticationProvider.* # Basic Auth
│   │       │   └── auth_sspi.*           # SSPI-хелперы (Parse407, Base64)
│   │       ├── config/
│   │       │   ├── Config.h              # Структуры конфигурации
│   │       │   └── ConfigManager.*       # JSON config I/O + DPAPI
│   │       ├── ipc/
│   │       │   └── PipeServer.h          # Named pipe сервер (header-only)
│   │       ├── logging/
│   │       │   ├── Logger.*              # Асинхронный файловый логгер
│   │       │   └── LogRotator.*          # Ротация логов + сжатие
│   │       └── stats/
│   │           └── StatsCollector.*      # Статистика соединений
│   │
│   ├── auth-agent/TcpRedirectorAuthAgent/ # C++ Auth Agent (сессия пользователя)
│   │   ├── main.cpp                       # Named pipe сервер + JSON-RPC dispatch
│   │   ├── SspiEngine.*                   # Управление SSPI-контекстами
│   │   └── ContextStore.h                 # Хранилище контекстов в памяти
│   │
│   └── gui/TcpRedirectorGUI/             # C# WPF GUI
│       ├── Adapters/Driving/Wpf/
│       │   ├── ViewModels/
│       │   │   ├── ShellViewModel.cs      # Логика главного окна
│       │   │   ├── SettingsViewModel.cs   # Логика вкладки настроек
│       │   │   └── StatsViewModel.cs      # Логика вкладки статистики
│       │   └── Controls/
│       │       └── TrafficGraph.cs        # Кастомный график трафика
│       └── Infrastructure/
│           ├── Ipc/IpcClient.cs           # Named pipe клиент (JSON-RPC)
│           └── Config/JsonConfigRepository.cs # Ввод-вывод конфигурации
│
├── external/
│   └── ProxyBridge/                       # Устаревший C-код (только для справки)
│
├── tests/
│   ├── mock_proxy.cpp                     # Mock HTTP-прокси для тестирования
│   ├── packet_generator.cpp               # Генератор TCP-пакетов
│   └── unit/service/RuleEngineTest.cpp    # Тесты сопоставления правил
│
└── docs/
    ├── ADMIN_GUIDE_RU.md / ADMIN_GUIDE_EN.md
    ├── SECURITY_RU.md / SECURITY_EN.md
    ├── ARCHITECTURE_RU.md / ARCHITECTURE_EN.md
    ├── DEVELOPER_RU.md / DEVELOPER_EN.md (этот файл)
    ├── BUILD.md, INSTALL.md, UPGRADE.md, UNINSTALL.md
    └── SMOKE_TEST.md
```

## 2. Сборка из исходников

### 2.1. Требования

| Инструмент | Версия | Путь |
|------------|--------|------|
| Visual Studio 2022+ | 18.x | `C:\Program Files\Microsoft Visual Studio\18\Community\` |
| .NET SDK | 9.0 | `dotnet` в PATH |
| Windows SDK | 10.0+ | Включён в VS |

### 2.2. C++ Сервис

```bat
cd src\service\TcpRedirectorService
MSBuild TcpRedirectorService.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Build /m
:: Результат: build\service\x64\Release\TcpRedirectorService.exe
```

### 2.3. C++ AuthAgent

```bat
cd src\auth-agent\TcpRedirectorAuthAgent
MSBuild TcpRedirectorAuthAgent.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Build /m
:: Результат: build\auth-agent\x64\Release\TcpRedirectorAuthAgent.exe
```

### 2.4. C# GUI

```bat
cd src\gui\TcpRedirectorGUI
dotnet build -c Release
:: Результат: bin\Release\net9.0-windows\TcpRedirectorGUI.exe
```

## 3. Ключевые детали реализации

### 3.1. Интеграция с WinDivert

**Файл**: [`WinDivertCapture.h`](../src/service/TcpRedirectorService/infrastructure/capture/WinDivertCapture.h)

WinDivert загружается через **Late Binding** (без import library):

```cpp
// Динамическая загрузка
HMODULE hDivert = LoadLibraryW(L"WinDivert.dll");
pWinDivertOpen = (WinDivertOpenFn)GetProcAddress(hDivert, "WinDivertOpen");
```

**Фильтр**: `outbound && tcp && tcp.Syn && !tcp.Ack`

Перехватываются только SYN-пакеты для определения оригинального назначения. После редиректа relay-сервер обрабатывает полный TCP-поток. Это позволяет избежать обработки каждого пакета в режиме ядра.

**Определение PID**: для каждого перехваченного SYN-пакета определяется PID процесса через `WinDivertHelperParsePacket` + кэш PID WinDivert. PID кэшируется для последующих пакетов с того же исходного порта.

### 3.2. Таблица соединений

**Файл**: [`ConnectionTable.h`](../src/service/TcpRedirectorService/infrastructure/relay/ConnectionTable.h)

Таблица соединений связывает перехваченные SYN-пакеты с установленными relay-соединениями:

```
Ключ: (src_port, src_ip) → Значение: (orig_dest_ip, orig_dest_port, proxy_config_id)
```

- Хеш-таблица с разрешением коллизий через связный список
- SRWLock для потокобезопасности
- **Известная проблема**: составной ключ добавлен (v1.1.1) для предотвращения коллизий при совпадении эфемерных портов

### 3.3. SSPI Negotiate Auth (устаревший)

**Файл**: [`auth_sspi.cpp`](../src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp)

Устаревшая реализация SSPI используется тестовыми прокси-скриптами и AuthAgent:

```cpp
// Ключевые шаги:
1. AcquireCredentialsHandle()    → учётные данные
2. InitializeSecurityContext()   → токен + контекст
3. Base64 кодирование токена     → "Negotiate <token>"
4. Парсинг 407 challenge         → извлечение токена сервера
5. InitializeSecurityContext()   → продолжение с challenge
```

**Флаги SSPI**: `ISC_REQ_CONFIDENTIALITY | ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT | ISC_REQ_ALLOCATE_MEMORY`

### 3.4. Named Pipe IPC

Оба IPC-канала используют **message-mode** named pipes с протоколом JSON-RPC 2.0:

```
Запрос:  {"jsonrpc":"2.0","method":"create_context","params":{...},"id":1}
Ответ:   {"jsonrpc":"2.0","result":{...},"id":1}
```

**Сервисный Pipe** (`\\.\pipe\TcpRedirectorService`):
- Сервер: `PipeServer.h` — создаёт pipe с `EVERYONE` (GENERIC_READ)
- Клиент: `IpcClient.cs` (GUI, C#)
- Методы: `get_config`, `set_config`, `get_rules`, `set_rules`, `get_connections`, `get_logs`, `get_stats`, `ping`, `start_capture`, `stop_capture`, `reload_config`, `set_log_level`

**AuthAgent Pipe** (`\\.\pipe\TcpRedirectorAuth`):
- Сервер: AuthAgent `main.cpp` — создаёт pipe с `EVERYONE` (полный доступ), **без sharing**
- Клиент: `KerberosAgentProvider.cpp` — сервис подключается как клиент
- Методы: `hello`, `create_context`, `continue_context`, `close_context`, `ping`

### 3.5. Двунаправленный релей (Bridge)

**Файл**: [`TcpRelayServer.h`](../src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h), строки 572-710

Релей использует две функции `OneWayRelay` (UP и DN) для двунаправленной передачи данных:

```cpp
struct RelayPair {
    SOCKET sock_client;   // Клиентский сокет
    SOCKET sock_proxy;    // Прокси-сокет
    LONG refs;            // Счётчик ссылок (2 → 0)
};
```

**Подсчёт ссылок**:
- `pair->refs` начинается с 2 (одна для UP, одна для DN)
- Каждый `OneWayRelay` декрементирует `refs` через `InterlockedDecrement()` при завершении
- Когда `refs` достигает 0, сокеты закрываются и пара удаляется
- **Исправление M1 (half-close)**: при recv=0 или ошибке вызывается `shutdown(to, SD_SEND)` — соседний поток может продолжать приём, пока его собственный recv не вернёт 0

**Опции сокетов** (v1.1.5):
- Буфер 4 МБ (`SO_RCVBUF`, `SO_SNDBUF`)
- `TCP_NODELAY` = 1 (отключение алгоритма Нейгла)
- `SO_KEEPALIVE` = 1 (после установки туннеля)
- Таймаут сброшен в 0 после CONNECT 200 (нет SO_RCVTIMEO в фазе данных)

### 3.6. Синхронизация Pipe I/O (исправление v1.1.5)

**Файл**: [`KerberosAgentProvider.cpp`](../src/service/TcpRedirectorService/infrastructure/auth/KerberosAgentProvider.cpp), строка 415

**Проблема**: `ConnectionHandler` запускается в новом потоке для каждого соединения. Несколько потоков могли одновременно вызывать `CreateContext()`, все вызывая `SendMessage()`/`ReadMessage()` на **одном** хендле named pipe без синхронизации. Это вызывало перемешивание сообщений — поток A писал запрос, поток B писал до того как A прочитал, затем A читал ответ B. Пострадавшие потоки навсегда зависали в `ReadMessage()`.

**Исправление**: `m_pipeMutex` заменён с `std::mutex` на `std::recursive_mutex`, и в `Call()` добавлен `lock_guard`:

```cpp
nlohmann::json KerberosAgentProvider::Call(const std::string& method,
                                            const nlohmann::json& params) {
    std::lock_guard<std::recursive_mutex> lock(m_pipeMutex);  // ← НОВОЕ
    // ... SendMessage + ReadMessage
}
```

**Почему recursive_mutex**: `ConnectToAgent()` захватывает `m_pipeMutex`, затем вызывает `Call("hello", ...)`, который теперь тоже его захватывает. Без рекурсии это привело бы к дедлоку.

### 3.7. Автозапуск AuthAgent

**Файл**: [`KerberosAgentProvider.cpp`](../src/service/TcpRedirectorService/infrastructure/auth/KerberosAgentProvider.cpp), строка 283

Сервис (работающий как `LocalSystem`) запускает AuthAgent в сессии пользователя:

```cpp
1. WTSGetActiveConsoleSessionId()     → ID сессии
2. WTSQueryUserToken(sessionId)       → токен пользователя
3. DuplicateTokenEx()                 → первичный токен (мин. права)
4. CreateEnvironmentBlock()           → окружение пользователя
5. CreateProcessAsUserW()             → запуск AuthAgent.exe
```

**Ограничение частоты**: кулдаун запуска 10 секунд, плюс атомарный флаг `s_launchInProgress`.

### 3.8. DPAPI-шифрование пароля

**Файл**: [`ConfigManager.cpp`](../src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp)

```cpp
// Шифрование: открытый текст → base64(DPAPI_encrypted)
CryptProtectData(plaintext, entropy) → base64

// Расшифровка: base64(DPAPI_encrypted) → открытый текст
CryptUnprotectData(base64_decode(input), entropy) → plaintext
```

Энтропия — фиксированная байтовая строка, предотвращающая атаки по радужным таблицам на DPAPI.

### 3.9. Атомарная запись конфигурации

```cpp
// Последовательность сохранения:
1. Запись в config.json.tmp
2. Flush + закрытие временного файла
3. ReplaceFile(tmp, config.json)  // атомарное переименование на NTFS
```

Предотвращает повреждение конфига при отключении питания или аварийном завершении.

## 4. Модель многопоточности

```
Главный поток
├── Цикл захвата WinDivert (1 поток, блокирующий WinDivertRecv)
├── AcceptLoop (1 поток, select() на слушающих сокетах)
│   └── ConnectionHandler (1 поток на соединение, CreateThread)
│       ├── OneWayRelay DN (выполняется в потоке ConnectionHandler)
│       └── OneWayRelay UP (1 поток, CreateThread)
├── KeepaliveLoop (1 поток, ping AuthAgent каждые 15с)
├── Logger worker (1 поток, асинхронная обработка очереди)
├── LogRotator (1 поток, периодическая проверка ротации)
└── IPC PipeServer (цикл приёма + поток на клиента)
```

**Заметки по потокобезопасности**:
- `KerberosAgentProvider::Call()` теперь потокобезопасен (сериализован через `m_pipeMutex`)
- `ConnectionTable` защищён `SRWLOCK`
- `RelayPair::refs` использует `InterlockedDecrement` (lock-free)
- `m_activePairs`, `m_totalRxBytes`, `m_totalTxBytes` — `std::atomic`
- `ConfigManager::Load()/Save()` защищены `shared_mutex` (много читателей, один писатель)

## 5. Тестирование

### 5.1. Тестовые прокси-скрипты

Расположены в `test-proxy/` (соседняя директория):

- `test_proxy.py` — простой HTTP-прокси (без аутентификации)
- `test_proxy_negotiate.py` — Negotiate-auth прокси (SSPI)
- `test_proxy_direct.py` — прямой прокси (без редиректа)
- `test_proxy_https.py` — тестирование HTTPS

Запуск: `python test_proxy_negotiate.py [port]`

### 5.2. Mock Proxy (C++)

`tests/mock_proxy.cpp` — автономный TCP-прокси для интеграционного тестирования. Подключается к реальным upstream-серверам.

### 5.3. Модульные тесты

```
tests/unit/service/RuleEngineTest.cpp

cd tests/build2
ctest -C Release
```

### 5.4. Smoke-тест

См. [`SMOKE_TEST.md`](SMOKE_TEST.md) для полной процедуры тестирования.

## 6. Советы по отладке

### 6.1. Консольный режим

```bat
TcpRedirectorService.exe --console
```
Запускает сервис как консольное приложение вместо Windows Service — удобно для отладки через printf и IDE.

### 6.2. Уровни логирования

Установите `"level": 0` в `config.json` для трассировочного логирования, затем:

```bat
tail -f "%ProgramData%\TcpRedirector\logs\tcp_redirector.log"
```

### 6.3. Отладка WinDivert

Включите отладочный лог WinDivert:
```bat
set WINDIVERT_DEBUG=1
TcpRedirectorService.exe --console
```
Вывод идёт в `%ProgramData%\TcpRedirector\logs\windivert_debug.log`.

### 6.4. Отладка AuthAgent

Запустите AuthAgent вручную в консоли:
```bat
TcpRedirectorAuthAgent.exe
```
Выводит диагностику в `%ProgramData%\TcpRedirector\logs\auth_agent.log` и stderr.

### 6.5. Инспекция Named Pipe

```bat
:: Список всех пайпов TcpRedirector
dir \\.\pipe\TcpRedirector*

:: Проверка безопасности пайпа
icacls \\.\pipe\TcpRedirectorAuth
```

## 7. Типичные ошибки

### 7.1. Перемешивание сообщений в Pipe (ИСПРАВЛЕНО в v1.1.5)

Никогда не вызывайте `WriteFile`/`ReadFile` на одном named pipe из нескольких потоков без сериализации. Message-mode пайпы доставляют полные сообщения, но конкурентные записи перемешиваются на уровне байтов.

### 7.2. FlushFileBuffers на Message Pipe (ИСПРАВЛЕНО в v1.1.5)

`FlushFileBuffers()` не нужен на message-mode named pipes — `WriteFile` уже гарантирует границу сообщения. Вызов только добавляет задержку.

### 7.3. TCP_NODELAY (ИСПРАВЛЕНО в v1.1.5)

Без `TCP_NODELAY` алгоритм Нейгла задерживает мелкие пакеты (TLS handshake, HTTP запросы) до 200 мс. Всегда устанавливайте `TCP_NODELAY` на сокетах релея.

### 7.4. Выделение буфера SSPI

`InitializeSecurityContext` может вернуть `SEC_E_BUFFER_TOO_SMALL`, если выходной буфер слишком мал. Всегда используйте `SecBufferDesc` с достаточным размером буфера и обрабатывайте повтор.

### 7.5. Фильтр WinDivert

Фильтр `outbound && tcp && tcp.Syn && !tcp.Ack` перехватывает только SYN. Если изменить фильтр для перехвата всех пакетов, НЕОБХОДИМО также обрабатывать полный конечный автомат TCP — это значительно более сложная задача.

### 7.6. Область действия DPAPI

Шифрование DPAPI привязано к учётной записи пользователя. Пароль, зашифрованный GUI (область пользователя), НЕ МОЖЕТ быть расшифрован сервисом (область SYSTEM) и наоборот. Оба должны использовать одну и ту же область.

## 8. История версий

Подробности см. в [`CHANGELOG.md`](../CHANGELOG.md). Ключевые вехи:

| Версия | Изменения |
|--------|-----------|
| v1.1.0 | Гексагональная архитектура, Kerberos-аутентификация, GUI |
| v1.1.1 | Исправление SSPI-буфера, WarmUp(), логирование бриджей, диагностика ConnectionHandler |
| v1.1.3 | Упреждающий запуск AuthAgent |
| v1.1.4 | Пересоздание AuthProvider при reload конфига |
| v1.1.5 | TCP_NODELAY, recursive_mutex для pipe I/O, удалён FlushFileBuffers, try-catch в StartBridge |