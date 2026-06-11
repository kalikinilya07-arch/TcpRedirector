# Проектирование системы логирования (Logger)

## 1. Структура LogMessage

```cpp
struct LogMessage {
    std::chrono::system_clock::time_point timestamp;  // когда произошло событие
    LogLevel level;                                    // ERROR, WARN, INFO, DEBUG
    std::string logger;                                // имя компонента: "capture", "proxy", "service", "ipc"
    std::string message;                               // тело сообщения
    std::string file;                                  // опционально: __FILE__
    int line;                                          // опционально: __LINE__
    std::string function;                              // опционально: __FUNCTION__
};
```

### LogLevel (enum)

| Значение | Число | Описание |
|----------|-------|----------|
| `Error` | 1 | Фатальные ошибки — падение, невозможность открыть WinDivert |
| `Warn` | 2 | Предупреждения — не фатально, но стоит обратить внимание |
| `Info` | 3 | Информационные сообщения — запуск, остановка, редиректы |
| `Debug` | 4 | Отладочные сообщения — каждый пакет, детали CONNECT |

---

## 2. Класс Logger

### 2.1. Синглтон vs инстанс

**Решение: НЕ синглтон, а инстанс, владеемый TcpRedirectorService.**

Причины:
- Тестируемость: синглтон нельзя замокать в unit-тестах
- Инверсия зависимостей: Logger реализует интерфейс `ILogSink`, который внедряется в Capture/Proxy через конструктор
- Чистая hexagonal: Capture не знает про Logger, он знает про `ILogSink`

```cpp
class Logger : public domain::ports::ILogSink {
public:
    // --- Инициализация ---
    bool Initialize(const std::filesystem::path& log_dir,
                    LogLevel level = LogLevel::Info,
                    size_t max_file_size_mb = 10,
                    size_t max_files = 5);

    void Shutdown();

    // --- ILogSink interface ---
    void Log(LogLevel level, const std::string& logger,
             const std::string& message) override;
    void Log(LogLevel level, const std::string& logger,
             const std::string& message,
             const std::string& file, int line,
             const std::string& function) override;

    // --- Настройки ---
    void SetLevel(LogLevel level);
    LogLevel GetLevel() const;

    // --- Подписка GUI ---
    using ListenerCallback = std::function<void(const LogMessage&)>;
    uint64_t RegisterListener(ListenerCallback callback);
    void UnregisterListener(uint64_t listener_id);

    // --- Получение последних записей (для IPC get_logs) ---
    std::vector<LogMessage> GetRecentEntries(size_t count = 500) const;

private:
    // ...
};
```

### 2.2. Интерфейс ILogSink (domain port)

```cpp
namespace domain::ports {

class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void Log(LogLevel level, const std::string& logger,
                     const std::string& message) = 0;
    virtual void SetLevel(LogLevel level) = 0;
    virtual LogLevel GetLevel() const = 0;
};

} // namespace domain::ports
```

### 2.3. Макросы для удобства

```cpp
#define LOG_ERROR(logger, msg)   m_logSink->Log(LogLevel::Error,   logger, msg)
#define LOG_WARN(logger, msg)    m_logSink->Log(LogLevel::Warn,    logger, msg)
#define LOG_INFO(logger, msg)    m_logSink->Log(LogLevel::Info,    logger, msg)
#define LOG_DEBUG(logger, msg)   m_logSink->Log(LogLevel::Debug,   logger, msg)
```

---

## 3. Асинхронная очередь

### 3.1. Структура

```cpp
class Logger {
private:
    struct AsyncQueue {
        std::queue<LogMessage> queue;
        mutable std::mutex mutex;
        std::condition_variable cv;
    };

    AsyncQueue m_asyncQueue;
    std::thread m_writerThread;
    std::atomic<bool> m_running{false};
    
    // Ring buffer для последних N записей (для GUI get_logs)
    static constexpr size_t RING_BUFFER_SIZE = 2000;
    std::array<LogMessage, RING_BUFFER_SIZE> m_ringBuffer;
    std::atomic<size_t> m_ringIndex{0};
    mutable std::mutex m_ringMutex;
};
```

### 3.2. Алгоритм работы

```
[Поток A: CaptureLoop]          [Поток B: ProxySession]       [Поток C: WriterThread]
         │                              │                              │
         │ LOG_INFO("capture", ...)      │ LOG_WARN("proxy", ...)       │
         │                              │                              │
         ▼                              ▼                              │
    m_asyncQueue.mutex.lock()      m_asyncQueue.mutex.lock()           │
    queue.push(msg)                queue.push(msg)                     │
    m_asyncQueue.mutex.unlock()    m_asyncQueue.mutex.unlock()         │
    m_asyncQueue.cv.notify_one()   m_asyncQueue.cv.notify_one()        │
         │                              │                              │
         │                              │         wait(cv, queue не пуста)
         │                              │              │
         │                              │              ▼
         │                              │         mutex.lock()
         │                              │         pop all → local vector
         │                              │         mutex.unlock()
         │                              │              │
         │                              │              ▼
         │                              │         for each msg:
         │                              │             format → fwrite → fflush
         │                              │             if size > maxSize → rotate
         │                              │              │
         │                              │              ▼
         │                              │         notify_listeners(msg)
         │                              │              │
         │                              │              ▼
         │                              │         ringBuffer[ringIndex++] = msg
```

### 3.3. Почему std::queue + mutex, а не lock-free

- **Lock-free** (например, `moodycamel::ConcurrentQueue`) — быстрее, но:
  - Внешняя зависимость (нужно тащить заголовки)
  - Сложнее отладка
  - Для логов производительность не критична (логи — редкие события относительно CaptureLoop)
- **std::queue + mutex** — просто, надёжно, достаточно быстро
  - WriterThread забирает ВСЕ сообщения разом (batch pop), минимизируя блокировку

### 3.4. Batch pop (псевдокод)

```cpp
void Logger::WriterThread() {
    while (m_running) {
        std::vector<LogMessage> batch;
        {
            std::unique_lock lock(m_asyncQueue.mutex);
            m_asyncQueue.cv.wait(lock, [this]{
                return !m_asyncQueue.queue.empty() || !m_running;
            });
            if (!m_running) break;
            // Swap — O(1), блокировка на микросекунды
            batch.swap(m_asyncQueue.queue);
        }
        for (const auto& msg : batch) {
            WriteToFile(msg);
            NotifyListeners(msg);
            AddToRingBuffer(msg);
        }
    }
}
```

---

## 4. Ротация лог-файлов

### 4.1. Алгоритм

```
Проверка после каждой записи:
  if (currentFileSize >= maxFileSize) {
      // 1. Закрыть текущий файл
      fclose(m_file);
      
      // 2. Сдвинуть старые файлы: app.log.4 → удалить
      //                          app.log.3 → app.log.4
      //                          app.log.2 → app.log.3
      //                          app.log.1 → app.log.2
      //                          app.log   → app.log.1
      for (int i = maxFiles - 1; i > 0; i--) {
          std::filesystem::rename(logDir / (name + "." + i), 
                                  logDir / (name + "." + (i+1)));
      }
      std::filesystem::rename(logDir / name, 
                              logDir / (name + ".1"));
      
      // 3. Создать новый файл
      m_file = _wfopen(logPath, L"a");
      m_currentSize = 0;
  }
```

### 4.2. Параметры

| Параметр | Дефолт | Описание |
|----------|--------|----------|
| `max_file_size_mb` | 10 | Максимальный размер файла до ротации |
| `max_files` | 5 | Сколько старых файлов хранить (app.log.1 — app.log.5) |

---

## 5. Формат лога

### 5.1. В файле

```
[2026-06-11 14:30:45.123] [INFO ] [service    ] TcpRedirector Service initialized
[2026-06-11 14:30:45.456] [INFO ] [capture    ] WinDivert capture started
[2026-06-11 14:30:46.001] [DEBUG] [capture    ] [PROXIED] #1 SYN 192.168.1.5:49152 -> 10.0.0.1:443 [S----] len=0
[2026-06-11 14:30:46.002] [INFO ] [redirect   ] Redirected: TransfersClient.exe (1234) -> 167772161:443
[2026-06-11 14:30:46.100] [ERROR] [proxy      ] connect: Failed to connect to proxy: 10061
```

Формат строки:
```
[YYYY-MM-DD HH:MM:SS.mmm] [LEVEL ] [component  ] message
```

- Уровень — 5 символов, выровнен вправо
- Компонент — 12 символов, выровнен влево
- Сообщение — свободный текст

### 5.2. В консоли (цветной вывод)

| Уровень | Цвет |
|---------|------|
| ERROR | Красный (0x0C) |
| WARN | Жёлтый (0x0E) |
| INFO | Белый (0x0F) |
| DEBUG | Серый (0x08) |

Используется `SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color)`.

---

## 6. Подписка GUI (Listener)

### 6.1. Механизм

```cpp
// В GUI (C# WPF):
// Через IPC-канал: таймер каждые 2 секунды вызывает get_logs
// ИЛИ через Named Pipe с callback (push-модель)

// В C++ сервисе:
uint64_t Logger::RegisterListener(ListenerCallback callback) {
    std::lock_guard lock(m_listenersMutex);
    uint64_t id = ++m_nextListenerId;
    m_listeners[id] = std::move(callback);
    return id;
}
```

### 6.2. Два варианта доставки логов в GUI

**Вариант A: Pull (рекомендуемый, уже реализован)**
- GUI каждые 2 секунды вызывает `get_logs` через IPC
- `IpcHandler::GetLogs()` → `Logger::GetRecentEntries(500)`
- Ring buffer хранит последние 2000 записей
- **Плюс**: не требует изменений в IPC-протоколе, уже работает
- **Минус**: задержка до 2 секунд

**Вариант B: Push (перспективный)**
- Logger вызывает callback listener'а в момент записи
- Listener отправляет лог через отдельный Named Pipe в GUI
- **Плюс**: реальное время
- **Минус**: нужно менять IPC-протокол, сложнее

**Решение: Оба варианта.**
- Pull — основной (уже есть `get_logs`)
- Push — опционально, через `RegisterListener` (например, для реального времени во вкладке "Логи")

---

## 7. Потокобезопасность — матрица

| Метод | Защита | Пояснение |
|-------|--------|-----------|
| `Log()` | `m_asyncQueue.mutex` | Быстрый push в очередь |
| `SetLevel()` | `std::atomic<LogLevel>` | Чтение без блокировки |
| `GetLevel()` | `std::atomic<LogLevel>::load()` | lock-free |
| `RegisterListener()` | `m_listenersMutex` | Редко вызывается |
| `GetRecentEntries()` | `m_ringMutex` | Копия из ring buffer |
| `WriterThread()` | `m_asyncQueue.mutex` + `cv` | Batch pop |

---

## 8. Схема интеграции с существующим кодом

```
                    ┌──────────────────────┐
                    │    config.json        │
                    │  log.level = 2        │
                    │  log.fileEnabled=true │
                    │  log.maxSizeMB=10     │
                    └──────┬───────────────┘
                           │ Load()
                           ▼
              ┌────────────────────────┐
              │     ConfigManager      │
              │  GetConfig().log.*     │
              └──────────┬─────────────┘
                         │ log.level, log.fileEnabled, log.maxSizeMB
                         ▼
              ┌────────────────────────┐
              │       Logger           │
              │  ┌──────────────────┐  │
              │  │ AsyncQueue       │  │ ← CaptureLoop пишет сюда
              │  │ (queue + mutex)  │  │ ← ProxySession пишет сюда
              │  └────────┬─────────┘  │ ← IpcHandler пишет сюда
              │           │            │
              │           ▼            │
              │  ┌──────────────────┐  │
              │  │ WriterThread     │  │ ← отдельный поток
              │  │ → fwrite(file)   │  │
              │  │ → rotate if full │  │
              │  │ → notify GUI     │  │
              │  │ → ring buffer    │  │
              │  └──────────────────┘  │
              └────────────────────────┘
                         │
                         │ GetRecentEntries()
                         ▼
              ┌────────────────────────┐
              │     IpcHandler         │
              │  get_logs → JSON       │
              └──────────┬─────────────┘
                         │ Named Pipe
                         ▼
              ┌────────────────────────┐
              │   GUI (C# WPF)         │
              │  таймер 2s → get_logs  │
              └────────────────────────┘
```

---

## 9. Миграция: что меняется

| Файл | Изменение |
|------|-----------|
| **НОВЫЙ: `infrastructure/logging/Logger.h`** | Полный класс Logger с асинхронной очередью |
| **НОВЫЙ: `infrastructure/logging/Logger.cpp`** | Реализация WriterThread, ротация, DPAPI не нужна |
| `domain/ports/ILogSink.h` | Уже существует — интерфейс |
| `WinDivertCapture.cpp` | Заменить `LOG(...)` макрос на `m_logSink->Log(...)` |
| `ProxyEngine.h` | Добавить `ILogSink*` для логирования ошибок CONNECT |
| `ServiceMain.h` | `m_logger->Info(...)` остаётся, но Logger теперь асинхронный |
| `IpcHandler.h` | `GetLogs()` → `m_logger->GetRecentEntries(500)` |
| `ConfigManager` | Читать `log.level`, `log.fileEnabled`, `log.maxSizeMB` |

---

## 10. Резюме

- **LogMessage** — структура с timestamp, level, logger, message, file, line, function
- **Logger** — не синглтон, а инстанс, внедряемый через `ILogSink` port
- **Асинхронность** — `std::queue + mutex + condition_variable` + отдельный WriterThread
- **Batch pop** — WriterThread забирает все сообщения разом (swap), блокировка на микросекунды
- **Ротация** — при превышении maxSizeMB: сдвиг .N → .N+1, удаление самого старого
- **Ring buffer** — 2000 последних записей для `get_logs` (IPC pull)
- **Listener** — callback-подписка для push-доставки в GUI (опционально)
- **Формат** — `[timestamp] [LEVEL] [component] message` + цветной вывод в консоль