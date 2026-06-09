# Windows Service — детальная архитектура

## 1. Общая структура сервиса

```
TcpRedirectorService.exe
├── Core
│   ├── ServiceMain              # Точка входа SCM
│   ├── ServiceControlHandler    # Обработчик SCM команд
│   └── ServiceManager           # Управление: init, run, shutdown
├── Communication
│   ├── DriverCommunicator       # IOCTL взаимодействие с драйвером
│   ├── PipeServer               # Named Pipe сервер для GUI
│   └── IpcMessage               # Форматы IPC сообщений
├── Config
│   ├── ConfigManager            # Чтение/запись конфигурации
│   ├── ProxyConfig              # Параметры прокси
│   └── SecretsManager           # DPAPI шифрование
├── Rules
│   ├── RuleEngine               # Управление правилами
│   ├── RuleMatcher              # Сопоставление процесса с правилом
│   └── RuleTypes                # Типы правил (process name, path, global)
├── Proxy
│   ├── ProxyEngine              # Асинхронный proxy engine
│   ├── ProxySession             # Одно прокси-соединение
│   ├── HttpConnectParser        # Parser CONNECT ответа
│   └── TunnelBridge             # Двусторонняя пересылка данных
├── Monitoring
│   ├── ConnectionTracker        # Отслеживание соединений
│   ├── StatisticsCollector      # Сбор статистики (RX/TX)
│   └── ConnectionRecord         # Запись о соединении
└── Logging
    ├── Logger                   # spdlog обёртка
    ├── LogRotator               # Ротация логов
    └── LogLevels                # Уровни: INFO, DEBUG, TRACE
```

## 2. Жизненный цикл сервиса

```
[SCM] Start Service
    │
    ▼
ServiceMain()
    │
    ├── RegisterServiceCtrlHandlerEx()
    ├── SetServiceStatus(SERVICE_RUNNING)
    │
    ├── InitConfigManager()
    │   ├── Load encrypted config from %ProgramData%\TcpRedirector\config.json
    │   └── Decrypt secrets via DPAPI
    │
    ├── InitLogging()
    │   ├── Initialize spdlog
    │   └── Start log rotation thread
    │
    ├── InitDriverCommunicator()
    │   ├── Open \\.\TcpRedirectorDriver
    │   ├── Load driver if not loaded (CreateService + StartService)
    │   ├── Send initial rules to driver
    │   └── Start event listener thread
    │
    ├── InitRuleEngine()
    │   ├── Load rules from config
    │   └── Send rules to driver via IOCTL
    │
    ├── InitProxyEngine()
    │   ├── Create io_context
    │   ├── Initialize connection pool
    │   └── Start async resolver
    │
    ├── InitPipeServer()
    │   ├── Create named pipe \\.\pipe\TcpRedirectorService
    │   └── Start async listener for GUI connections
    │
    ├── MainLoop()
    │   ├── asio::io_context::run()  // async event loop
    │   ├── Driver events processing
    │   ├── Redirect handling
    │   └── GUI IPC handling
    │
    └── OnStop()
        ├── Stop accepting new connections
        ├── Close all active proxy sessions
        ├── Unload driver (optional: keep running)
        ├── Flush logs
        └── SetServiceStatus(SERVICE_STOPPED)

[SCM] Stop Service
```

## 3. Классы и интерфейсы

### 3.1 Core

```cpp
// ServiceMain.h
class ServiceManager {
public:
    static ServiceManager& Instance();
    
    HRESULT Initialize();
    HRESULT Run();
    HRESULT Stop();
    bool IsRunning() const;
    
    // Control handlers
    void OnShutdown();
    void OnSessionChange(DWORD sessionId);
    void OnDeviceEvent(DWORD eventType);
    
private:
    std::atomic<bool> m_running;
    std::unique_ptr<ConfigManager> m_config;
    std::unique_ptr<DriverCommunicator> m_driver;
    std::unique_ptr<RuleEngine> m_rules;
    std::unique_ptr<ProxyEngine> m_proxy;
    std::unique_ptr<PipeServer> m_pipeServer;
    std::unique_ptr<ConnectionTracker> m_tracker;
    std::unique_ptr<spdlog::logger> m_logger;
};
```

### 3.2 Communication

```cpp
// DriverCommunicator.h
class DriverCommunicator {
public:
    DriverCommunicator();
    ~DriverCommunicator();
    
    HRESULT Open();
    void Close();
    
    // Send rules to driver cache
    HRESULT UpdateRules(const std::vector<Rule>& rules);
    
    // Get pending redirects (blocking call with event)
    std::vector<RedirectEvent> GetPendingRedirects(DWORD timeoutMs);
    
    // Acknowledge redirect processed
    HRESULT AckRedirect(UINT64 redirectId);
    
    // Get driver statistics
    DriverStats GetStats();
    
    // Request process info from driver
    ProcessInfo QueryProcessInfo(HANDLE pid);
    
    // Event handle for waiting
    HANDLE GetEventHandle() const;
    
private:
    HANDLE m_hDevice;
    HANDLE m_hEvent;
    OVERLAPPED m_overlapped;
    
    HRESULT SendIoctl(DWORD ioctlCode, LPVOID inBuf, DWORD inSize,
                      LPVOID outBuf, DWORD outSize, DWORD* bytesReturned);
};
```

```cpp
// PipeServer.h
class PipeServer {
public:
    explicit PipeServer(boost::asio::io_context& ioContext);
    
    HRESULT Start(const std::wstring& pipeName);
    void Stop();
    
    // Send connection list to GUI
    void SendConnections(const std::vector<ConnectionRecord>& connections);
    
    // Send log entries to GUI
    void SendLogs(const std::vector<LogEntry>& logs);
    
    // Send config to GUI
    void SendConfig(const ProxyConfig& config);
    
    // Send rules to GUI
    void SendRules(const std::vector<Rule>& rules);
    
    // Send stats to GUI
    void SendStats(const ServiceStats& stats);
    
private:
    boost::asio::io_context& m_ioContext;
    std::unique_ptr<boost::asio::windows::stream_handle> m_pipe;
    
    void StartAsyncAccept();
    void HandleRequest(const IpcMessage& request, IpcMessage& response);
};
```

### 3.3 Config

```cpp
// ConfigManager.h
class ConfigManager {
public:
    ConfigManager();
    
    HRESULT Load();
    HRESULT Save();
    
    ProxyConfig GetProxyConfig() const;
    HRESULT SetProxyConfig(const ProxyConfig& config);
    
    std::vector<Rule> GetRules() const;
    HRESULT SetRules(const std::vector<Rule>& rules);
    
    LogSettings GetLogSettings() const;
    HRESULT SetLogSettings(const LogSettings& settings);
    
    std::filesystem::path GetConfigPath() const;
    
private:
    ProxyConfig m_proxyConfig;
    std::vector<Rule> m_rules;
    LogSettings m_logSettings;
    std::filesystem::path m_configPath;
    
    HRESULT LoadFromFile();
    HRESULT SaveToFile();
};
```

```cpp
// SecretsManager.h
class SecretsManager {
public:
    // Encrypt password using DPAPI
    static std::vector<BYTE> Encrypt(const std::wstring& plaintext);
    
    // Decrypt password using DPAPI
    static std::wstring Decrypt(const std::vector<BYTE>& ciphertext);
    
private:
    static DATA_BLOB ToDataBlob(const std::vector<BYTE>& data);
    static std::vector<BYTE> FromDataBlob(const DATA_BLOB& blob);
};
```

### 3.4 Rules

```cpp
// RuleEngine.h
enum class RuleAction {
    Proxy,    // Перенаправлять через прокси
    Direct,   // Пропускать (не редиректить)
    Block     // Блокировать
};

enum class RuleType {
    ProcessName,  // По имени процесса (chrome.exe)
    ProcessPath,  // По полному пути (C:\...\app.exe)
    Global        // Глобальное правило (*)
};

struct Rule {
    std::string id;             // UUID
    RuleType type;
    RuleAction action;
    std::wstring pattern;       // Wildcard-шаблон
    std::wstring description;   // Описание
    int priority;               // Приоритет (меньше = выше)
    bool enabled;               // Включено/выключено
    DateTime created;
    DateTime modified;
};

class RuleEngine {
public:
    RuleEngine();
    
    HRESULT AddRule(const Rule& rule);
    HRESULT RemoveRule(const std::string& ruleId);
    HRESULT UpdateRule(const Rule& rule);
    HRESULT ReorderRules(const std::vector<std::string>& ruleIds);
    
    std::vector<Rule> GetRules() const;
    
    // Match process against rules, returns the best match
    RuleAction Match(const ProcessInfo& processInfo) const;
    
    // Check if process should be redirected
    bool ShouldRedirect(const ProcessInfo& processInfo) const;
    
private:
    std::vector<Rule> m_rules;
    mutable std::shared_mutex m_mutex;
    
    bool MatchProcessName(const Rule& rule, const std::wstring& processName) const;
    bool MatchProcessPath(const Rule& rule, const std::wstring& processPath) const;
    bool WildcardMatch(const std::wstring& pattern, const std::wstring& text) const;
};
```

### 3.5 Proxy Engine

```cpp
// ProxyEngine.h
class ProxyEngine {
public:
    explicit ProxyEngine(boost::asio::io_context& ioContext);
    ~ProxyEngine();
    
    HRESULT Initialize(const ProxyConfig& config);
    void Shutdown();
    
    // Handle a new redirect - create proxy tunnel
    HRESULT HandleRedirect(const RedirectEvent& redirectEvent,
                           std::function<void(RedirectResult)> callback);
    
    // Close session
    HRESULT CloseSession(uint64_t sessionId);
    
    ProxyStats GetStats() const;
    
private:
    boost::asio::io_context& m_ioContext;
    ProxyConfig m_config;
    std::unordered_map<uint64_t, std::shared_ptr<ProxySession>> m_sessions;
    mutable std::shared_mutex m_mutex;
    
    std::shared_ptr<ProxySession> CreateSession(const RedirectEvent& redirect);
};
```

```cpp
// ProxySession.h
class ProxySession : public std::enable_shared_from_this<ProxySession> {
public:
    ProxySession(boost::asio::io_context& ioContext,
                 const ProxyConfig& proxyConfig,
                 const RedirectEvent& redirect);
    ~ProxySession();
    
    HRESULT Start(std::function<void(RedirectResult)> callback);
    HRESULT Close();
    
    // Statistics
    uint64_t GetRxBytes() const;
    uint64_t GetTxBytes() const;
    std::chrono::milliseconds GetDuration() const;
    ConnectionState GetState() const;
    
private:
    // TCP сокеты
    boost::asio::ip::tcp::socket m_localSocket;    // сокет от редиректа
    boost::asio::ip::tcp::socket m_proxySocket;    // сокет к прокси
    
    // Буферы
    std::array<char, 65536> m_localBuffer;
    std::array<char, 65536> m_proxyBuffer;
    
    // Данные редиректа
    RedirectEvent m_redirect;
    ProxyConfig m_config;
    
    // Статистика
    std::atomic<uint64_t> m_rxBytes{0};
    std::atomic<uint64_t> m_txBytes{0};
    std::chrono::steady_clock::time_point m_startTime;
    ConnectionState m_state;
    
    // Этапы
    enum class Stage {
        Resolving,
        ConnectingToProxy,
        SendingConnect,
        WaitingResponse,
        TunnelEstablished,
        Closed,
        Error
    };
    Stage m_stage;
    
    // Шаги установки туннеля
    void ResolveProxy();
    void ConnectToProxy(const boost::asio::ip::tcp::resolver::results_type& endpoints);
    void SendConnectRequest();
    void ReadConnectResponse();
    void ParseConnectResponse(const boost::system::error_code& ec, size_t bytesRead);
    void StartBridging();  // Двусторонняя пересылка
    
    // Bridging
    void ReadFromLocal();
    void WriteToProxy(size_t bytesRead);
    void ReadFromProxy();
    void WriteToLocal(size_t bytesRead);
    
    // Обработка ошибок
    void HandleError(const boost::system::error_code& ec, const std::string& context);
};
```

```cpp
// HttpConnectParser.h
class HttpConnectParser {
public:
    enum class State {
        ReadingStatusLine,
        ReadingHeaders,
        Complete,
        Error
    };
    
    // Парсинг ответа на CONNECT запрос
    struct ConnectResponse {
        int statusCode;
        std::string reasonPhrase;
        std::map<std::string, std::string> headers;
        bool success;  // statusCode == 200
        std::string errorMessage;
    };
    
    static ConnectResponse Parse(const std::vector<char>& data, size_t size);
    
private:
    static bool ParseStatusLine(const std::string& line, int& code, std::string& reason);
};
```

### 3.6 Monitoring

```cpp
// ConnectionTracker.h
struct ConnectionRecord {
    uint64_t id;                    // Уникальный ID соединения
    HANDLE pid;                     // PID процесса
    std::wstring processName;       // Имя процесса
    std::wstring processPath;       // Полный путь процесса
    std::wstring destinationHost;   // Имя хоста (если доступно)
    boost::asio::ip::address destinationIp;    // IP назначения
    uint16_t destinationPort;       // Порт назначения
    std::chrono::steady_clock::time_point startTime;
    std::chrono::milliseconds duration;
    uint64_t rxBytes;
    uint64_t txBytes;
    bool proxyEnabled;
    ConnectionState state;
};

enum class ConnectionState {
    Redirecting,
    ConnectingToProxy,
    TunnelEstablished,
    Closing,
    Closed,
    Error
};

class ConnectionTracker {
public:
    ConnectionTracker();
    
    void AddConnection(const ConnectionRecord& record);
    void UpdateConnection(uint64_t id, const ConnectionRecord& updates);
    void RemoveConnection(uint64_t id);
    
    std::vector<ConnectionRecord> GetActiveConnections() const;
    std::vector<ConnectionRecord> GetConnectionsByProcess(HANDLE pid) const;
    ConnectionRecord GetConnection(uint64_t id) const;
    
    // Filtering
    std::vector<ConnectionRecord> Search(const std::wstring& query) const;
    std::vector<ConnectionRecord> FilterByProcess(const std::wstring& processName) const;
    
    ServiceStats GetAggregatedStats() const;
    
private:
    std::unordered_map<uint64_t, ConnectionRecord> m_connections;
    mutable std::shared_mutex m_mutex;
    uint64_t m_nextId{1};
};
```

### 3.7 Logging

```cpp
// Logger.h
enum class LogLevel {
    TRACE = 0,
    DEBUG = 1,
    INFO = 2,
    WARN = 3,
    ERROR = 4,
    OFF = 5
};

struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level;
    std::string logger;
    std::string message;
    std::string threadName;
};

class Logger {
public:
    static void Initialize(const std::filesystem::path& logDir, LogLevel level);
    static void Shutdown();
    
    static void SetLevel(LogLevel level);
    static LogLevel GetLevel();
    
    // Access to recent log entries for GUI
    static std::vector<LogEntry> GetRecentEntries(size_t maxCount);
    
    static std::shared_ptr<spdlog::logger> GetLogger(const std::string& name);
    
private:
    static std::shared_ptr<spdlog::logger> m_coreLogger;
    static std::vector<LogEntry> m_recentEntries;
    static std::mutex m_recentMutex;
    static std::filesystem::path m_logDir;
};

// LogRotator.h
class LogRotator {
public:
    LogRotator(const std::filesystem::path& logDir,
               size_t maxFileSizeMB = 50,
               size_t maxFiles = 10,
               int maxAgeDays = 30);
    
    void Start();
    void Stop();
    
private:
    std::filesystem::path m_logDir;
    size_t m_maxFileSizeMB;
    size_t m_maxFiles;
    int m_maxAgeDays;
    std::atomic<bool> m_running;
    std::thread m_cleanupThread;
    
    void CleanupLoop();
    void DeleteOldLogs();
    void EnforceMaxFileCount();
};
```

## 4. IPC протокол Service ↔ GUI

### Named Pipe: `\\.\pipe\TcpRedirectorService`

### Формат сообщений (JSON)

```json
// Запрос от GUI
{
    "type": "request",
    "id": "req-001",
    "method": "get_config",
    "params": {}
}

// Ответ сервиса
{
    "type": "response",
    "id": "req-001",
    "status": "success",
    "data": {
        "proxy": {
            "host": "proxy.example.com",
            "port": 3128,
            "auth_required": true,
            "has_password": true
        },
        "rules": [...],
        "log_level": "INFO"
    }
}

// Push-уведомление (сервис → GUI)
{
    "type": "push",
    "event": "connection_updated",
    "data": {
        "connections": [...]
    }
}

// Сообщение об ошибке
{
    "type": "response",
    "id": "req-001",
    "status": "error",
    "error": {
        "code": "PROXY_CONNECT_ERROR",
        "message": "Failed to connect to proxy server"
    }
}
```

### Методы IPC API

| Метод | Направление | Описание |
|-------|-------------|----------|
| `get_config` | GUI→Service | Получить конфигурацию |
| `set_config` | GUI→Service | Обновить конфигурацию |
| `get_rules` | GUI→Service | Получить правила |
| `add_rule` | GUI→Service | Добавить правило |
| `update_rule` | GUI→Service | Обновить правило |
| `delete_rule` | GUI→Service | Удалить правило |
| `reorder_rules` | GUI→Service | Изменить порядок правил |
| `get_connections` | GUI→Service | Получить список соединений |
| `get_logs` | GUI→Service | Получить логи |
| `set_log_level` | GUI→Service | Установить уровень логирования |
| `get_stats` | GUI→Service | Получить статистику |
| `service_start` | GUI→Service | Запустить сервис (SCM) |
| `service_stop` | GUI→Service | Остановить сервис |
| `service_restart` | GUI→Service | Перезапустить сервис |
| `service_status` | GUI→Service | Получить статус сервиса |
| `push:connections` | Service→GUI | Обновление списка соединений |
| `push:stats` | Service→GUI | Обновление статистики |
| `push:log` | Service→GUI | Новая запись лога |
| `push:status` | Service→GUI | Изменение статуса |

## 5. Обработка ошибок в сервисе

### Прокси недоступен

```
1. ProxySession::ConnectToProxy() → error (connection refused / timeout)
2. Close redirect socket
3. Log error: DEBUG "Proxy connection failed for PID XXXX, host:port"
4. Notify GUI via push event
5. Connection marked as "Error" in tracker
6. Redirect socket is closed → application sees connection failure
```

### Разрыв соединения с прокси

```
1. ProxySession detects TCP disconnect from proxy
2. Close redirect socket (app sees connection lost)
3. Close proxy socket
4. Update stats in ConnectionTracker
5. Remove from active connections
6. Log: DEBUG "Tunnel closed: PID XXXX, duration Xs"
```

### Таймаут соединения

```
1. Timer set for: connect timeout (10s), CONNECT response (30s), idle (300s)
2. On timeout: close both sockets
3. Log: TRACE "Connection timeout: PID XXXX"
4. Update ConnectionTracker
```

### Ошибка валидации правил

```
1. GUI sends invalid rule (empty pattern, invalid path)
2. Service validates and returns error response
3. Rule not applied
4. Log: INFO "Invalid rule rejected: ..."
```

## 6. Boost.Asio конфигурация

```cpp
// io_context с несколькими потоками
boost::asio::io_context m_ioContext;
auto m_workGuard = boost::asio::make_work_guard(m_ioContext);
std::vector<std::thread> m_workerThreads;

// Количество потоков: std::thread::hardware_concurrency()
// Для MVP: 4 потока достаточно

// Настройки сокетов
m_localSocket.set_option(boost::asio::ip::tcp::no_delay(true));
m_proxySocket.set_option(boost::asio::ip::tcp::no_delay(true));

// Размер буфера: 64KB на поток
m_localSocket.set_option(boost::asio::socket_base::send_buffer_size(65536));
m_localSocket.set_option(boost::asio::socket_base::receive_buffer_size(65536));
m_proxySocket.set_option(boost::asio::socket_base::send_buffer_size(65536));
m_proxySocket.set_option(boost::asio::socket_base::receive_buffer_size(65536));

// Таймауты
m_localSocket.async_wait(boost::asio::ip::tcp::socket::wait_read,
    boost::bind(&ProxySession::ReadFromLocal, shared_from_this(), _1));