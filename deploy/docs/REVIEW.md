# Cross-Review: Config + Logger + StatsCollector

## Проверка на непротиворечивость и скрытые проблемы

---

### 🔴 Проблема 1: Race condition в per-second rate (StatsCollector)

**Файл:** [`STATS_DESIGN.md:67-73`](TcpRedirector/docs/STATS_DESIGN.md:67)

```cpp
struct RateCounter {
    std::atomic<uint64_t> count{0};
    std::chrono::steady_clock::time_point lastReset;  // НЕ АТОМАРНЫЙ!
};
```

**Проблема:** `m_packetsPerSecond.lastReset` — не `std::atomic`, не защищён мьютексом. 
`lastReset` пишется в [`STATS_DESIGN.md:149-159`](TcpRedirector/docs/STATS_DESIGN.md:149) из `OnPacket()`, который вызывается из CaptureLoop.
Если два потока (теоретически, хотя CaptureLoop один) или один поток на 1000-м пакете проверяет `elapsed >= 1000` — **data race**.

Попытка CAS на строке 154-155 не сработает: `m_packetsPerSecond.lastReset` нельзя атомарно сравнить, это `time_point`.

**Решение: вычислять per-second rate дифференциально в GetStats()**

```cpp
// OnPacket — только счётчик, без per-second rate
void OnPacket(const PacketInfo& info) {
    m_totalPackets.fetch_add(1, relaxed);
    // ... остальные счётчики ...
}

// GetStats — вычисление pps по разности
StatsSnapshot GetStats() {
    auto now = steady_clock::now();
    auto elapsed = now - m_lastStatsCall;
    uint64_t currTotal = m_totalPackets.load();
    double pps = (elapsed > 0ms)
        ? (currTotal - m_lastTotalPackets) / elapsed_seconds
        : 0.0;
    m_lastStatsCall = now;           // неатомарный, но вызывается только из GUI-таймера
    m_lastTotalPackets = currTotal;  // один поток-читатель
    // ...
}
```

---

### 🔴 Проблема 2: shared_mutex в CaptureLoop — contention

**Файл:** [`CONFIG_DESIGN.md:262`](TcpRedirector/docs/CONFIG_DESIGN.md:262)

```cpp
// В CaptureLoop на КАЖДЫЙ SYN-пакет:
if (m_configManager->GetConfig().proxy.enabled) { ... }
```

**Проблема:** CaptureLoop вызывает `GetConfig()` до 50 000 раз в секунду.
Каждый вызов захватывает `shared_lock(m_mutex)` — чтение из разделяемой памяти.
На x86 это ~50-200 нс на lock, итого **2.5-10 мс/сек на блокировки**.

Хотя shared_mutex позволяет множественные чтения — overhead есть.

**Решение:** кэшировать `proxy.enabled` в атомарном флаге внутри WinDivertCapture.

```cpp
// В WinDivertCapture.h добавить:
std::atomic<bool> m_proxyEnabled{true};

// В WinDivertCapture::CaptureLoop() читать atomic:
if (isTargetSyn && m_proxyEnabled.load(relaxed)) { ... }

// ConfigManager уведомляет WinDivertCapture при изменении:
m_capture->SetProxyEnabled(cfg.proxy.enabled);
```

---

### 🔴 Проблема 3: Ring buffer overflow + out-of-bounds (Logger)

**Файл:** [`LOGGER_DESIGN.md:121-123`](TcpRedirector/docs/LOGGER_DESIGN.md:121)

```cpp
static constexpr size_t RING_BUFFER_SIZE = 2000;
std::array<LogMessage, RING_BUFFER_SIZE> m_ringBuffer;
std::atomic<size_t> m_ringIndex{0};
```

**Проблема:** `m_ringIndex` инкрементируется бесконечно. Когда он достигает 2000 — запись в `m_ringBuffer[2000]` — выход за границы массива.

В псевдокоде WriterThread (строка 187) нет `% RING_BUFFER_SIZE`.

**Решение:** добавить модуль + копирование, а не перемещение:

```cpp
void AddToRingBuffer(const LogMessage& msg) {
    size_t idx = m_ringIndex.fetch_add(1, relaxed) % RING_BUFFER_SIZE;
    std::lock_guard lock(m_ringMutex);
    m_ringBuffer[idx] = msg;  // копия, а не move
}
```

Также: при `fetch_add` к `SIZE_MAX` (18446744073709551615) — переполнение через 10^19 записей.
Практически безопасно (~ 10^8 лет работы), но корректнее использовать `% RING_BUFFER_SIZE`.

---

### 🔴 Проблема 4: Отсутствие механизма уведомлений при изменении конфига

**Файл:** [`CONFIG_DESIGN.md:155`](TcpRedirector/docs/CONFIG_DESIGN.md:155)

```cpp
bool UpdateConfig(const Config& newConfig);  // обновляет и сохраняет
```

**Проблема:** Когда GUI меняет `proxy.host`, `log.level` или `app.exePath` через `UpdateConfig()` — никто не уведомляется.
- WinDivertCapture продолжает использовать старый `m_targetProcessPath`
- Logger продолжает использовать старый log level
- ProxyEngine продолжает коннектиться к старому host

**Решение:** добавить listener-механизм в ConfigManager:

```cpp
using ConfigChangeListener = std::function<void(const Config& oldConfig, const Config& newConfig)>;
uint64_t AddListener(ConfigChangeListener cb);
void RemoveListener(uint64_t id);

// В UpdateConfig():
auto oldCfg = m_config;
m_config = newConfig;
Save();
for (auto& [id, cb] : m_listeners) {
    cb(oldCfg, m_config);
}
```

Пример регистрации в ServiceMain.h:
```cpp
m_configManager->AddListener([this](const Config& oldCfg, const Config& newCfg) {
    if (oldCfg.proxy.host != newCfg.proxy.host ||
        oldCfg.proxy.port != newCfg.proxy.port) {
        m_proxyEngine->Shutdown();
        m_proxyEngine->Initialize(ConfigToProxyConfig(newCfg));
    }
    if (oldCfg.log.level != newCfg.log.level) {
        m_logger->SetLevel(static_cast<LogLevel>(newCfg.log.level));
    }
});
```

---

### 🟡 Проблема 5: exePath и exeName рассогласованы

**Файл:** [`CONFIG_DESIGN.md:10-12`](TcpRedirector/docs/CONFIG_DESIGN.md:10)

```json
{
  "app": {
    "exePath": "C:\\Projects\\china\\police_sec\\TransfersClient.exe",
    "exeName": "TransfersClient.exe"
  }
}
```

**Проблема:** `exeName` дублирует информацию, уже содержащуюся в `exePath`.
Если пользователь (или GUI) изменит только `exePath`, но забудет обновить `exeName` — 
WinDivertCapture будет искать `TransfersClient.exe`, а RuleEngine будет сравнивать с `packet_generator.exe`.

**Решение:** убрать `exeName` из JSON. Вычислять автоматически:

```cpp
// ConfigManager::Load():
m_config.app.exeName = std::filesystem::path(m_config.app.exePath).filename().wstring();
```

Или хранить в ConfigManager как вычисляемое поле (геттер):

```cpp
std::wstring Config::GetExeName() const {
    auto pos = exePath.find_last_of(L'\\');
    return (pos != std::wstring::npos) ? exePath.substr(pos + 1) : exePath;
}
```

---

### 🟡 Проблема 6: UDP-счётчик в StatsCollector никогда не сработает

**Файл:** [`STATS_DESIGN.md:54-55`](TcpRedirector/docs/STATS_DESIGN.md:54)

```cpp
std::atomic<uint64_t> m_tcpPackets{0};
std::atomic<uint64_t> m_udpPackets{0};
```

**Проблема:** CaptureLoop в [`WinDivertCapture.cpp:177`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/capture/WinDivertCapture.cpp:177) обрабатывает ТОЛЬКО TCP:

```cpp
if (ipHdr && tcpHdr) { ... }
```

UDP заголовки игнорируются. `m_udpPackets` всегда будет 0.
Это вводит пользователя в заблуждение.

**Решение:** либо убрать `m_udpPackets`, либо добавить UDP-обработку в CaptureLoop.
Я рекомендую **убрать** — это dead code. При необходимости UDP-редирект добавится позже отдельным PR.

---

### 🟡 Проблема 7: Нет единой точки создания директорий

**Файлы:**
- [`CONFIG_DESIGN.md:140`](TcpRedirector/docs/CONFIG_DESIGN.md:140): `%ProgramData%\TcpRedirector\config\` (создаётся ConfigManager)
- [`LOGGER_DESIGN.md:43`](TcpRedirector/docs/LOGGER_DESIGN.md:43): `%ProgramData%\TcpRedirector\logs\` (создаётся Logger.Initialize())
- [`WinDivertCapture.cpp:83`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/capture/WinDivertCapture.cpp:83): `%ProgramData%\TcpRedirector\logs\windivert_debug.log` (создаётся WinDivertCapture::Open())

**Проблема:** Три разных компонента создают одну и ту же директорию `%ProgramData%\TcpRedirector\logs\`.
Если один из них не запущен — лог-файлы не создадутся.

**Решение:** Вынести создание структуры директорий в TcpRedirectorService::Initialize():

```cpp
// ServiceMain.h:
bool CreateDirectories() {
    auto progData = std::filesystem::path(getenv("ProgramData")) / "TcpRedirector";
    std::filesystem::create_directories(progData / "config");
    std::filesystem::create_directories(progData / "logs");
    return true;
}
```

---

## Итоговая таблица проблем

| # | Серьёзность | Компонент | Проблема | Решение |
|---|-------------|-----------|----------|---------|
| 1 | 🔴 CRITICAL | StatsCollector | Race condition в per-second rate (data race) | Вычислять дифференциально в GetStats |
| 2 | 🔴 CRITICAL | Config + Capture | shared_lock в CaptureLoop — contention 50k pps | Атомарный флаг m_proxyEnabled |
| 3 | 🔴 CRITICAL | Logger | Ring buffer out-of-bounds без `% SIZE` | `fetch_add % RING_BUFFER_SIZE` |
| 4 | 🟡 IMPORTANT | ConfigManager | Нет уведомлений при изменении конфига | Listener callback-механизм |
| 5 | 🟡 IMPORTANT | Config | exeName дублирует exePath | Вычислять автоматически |
| 6 | 🟢 MINOR | StatsCollector | UDP-счётчик всегда 0 (dead code) | Убрать m_udpPackets |
| 7 | 🟢 MINOR | ServiceMain | Нет единой точки создания директорий | CreateDirectories() в Initialize |

---

## Cross-cutting: как три компонента соединяются

```
TcpRedirectorService::Initialize()
│
├── create_directories()
│
├── ConfigManager::Load()              ← читает config.json
│   │
│   ├── GetConfig().app.exePath        → WinDivertCapture.SetTarget(exePath)
│   ├── GetConfig().proxy.*            → ProxyEngine.Initialize(proxyCfg)
│   ├── GetConfig().log.*              → Logger.Initialize(level, maxSize)
│   └── AddListener(callback)          → уведомление при изменениях
│
├── Logger::Initialize(...)            ← асинхронный WriterThread
│   │
│   ├── ILogSink* → WinDivertCapture   ← на каждый пакет (замена LOG макроса)
│   └── ILogSink* → ProxyEngine        ← на ошибки CONNECT
│
├── WinDivertCapture::Open()           ← запуск CaptureLoop
│   │
│   ├── StatsCollector.OnPacket()      ← для каждого пакета (lock-free)
│   └── ILogSink->Log()                ← heartbeat + ошибки
│
├── ProxyEngine::CreateSession()
│   │
│   ├── ILogSink->Log()                ← ошибки CONNECT
│   └── StatsCollector?                ← нет, статистика редиректов уже в CaptureLoop
│
└── IpcHandler
    ├── ConfigManager::GetConfig()     ← get_config
    ├── ConfigManager::UpdateConfig()  ← set_config (триггерит listener'ы)
    ├── Logger::GetRecentEntries()     ← get_logs
    ├── StatsCollector::GetStats()     ← get_stats
    └── StatsCollector::Reset()        ← reset_stats
```

---

## Рекомендуемый порядок исправления

1. 🔴 **Per-second rate** — убрать из OnPacket, вычислить в GetStats
2. 🔴 **Ring buffer** — добавить `% RING_BUFFER_SIZE` 
3. 🔴 **shared_lock в CaptureLoop** — атомарный флаг m_proxyEnabled
4. 🟡 **Listener механизм** — ConfigChangeListener
5. 🟡 **exeName** — вычислять автоматически
6. 🟢 **UDP счётчик** — убрать
7. 🟢 **CreateDirectories** — единая точка