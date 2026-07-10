# План исправления остаточных проблем и реализации ротации логов

## Состояние: 09.07.2026

На основе [`REVIEW.md`](../REVIEW.md), [`LOGGER_DESIGN.md`](../LOGGER_DESIGN.md) и анализа текущей кодовой базы.

---

## Сводка проблем к исправлению

| # | ID | Суть | Приоритет | Оценка |
|---|----|------|-----------|--------|
| 1 | C2 | IPC без аутентификации (TCP localhost:34011) | 🔴 Critical | 3h |
| 2 | M13 | Нет валидации размера IPC-сообщений → OOM | 🟡 Medium | 0.5h |
| 3 | M16 | CompositionRoot.h — мёртвый код | 🟡 Medium | 0.5h |
| 4 | NEW | Ротация логов по расписанию + архивирование | 🟠 High | 4h |
| 5 | NEW | Конфиг ротации только в config.json (не UI) | 🟠 High | вкл. в #4 |

---

## 1. C2 — Аутентификация IPC

### Анализ текущего состояния

```
GUI (IpcClient.cs) ──TCP:34011──► ServiceMain.h (IpcHandler.h)
                                   ↑
                              PipeServer.h (Named Pipe, ACL)
                              уже реализован, но НЕ используется
                              GUI к нему не подключается
```

[`PipeServer.h`](../src/service/TcpRedirectorService/infrastructure/ipc/PipeServer.h) уже имеет:
- ACL через SDDL: `D:(A;;GA;;;SY)(A;;GA;;;BA)` — только SYSTEM + Administrators
- Полноценный JSON-RPC обработчик
- Потокобезопасную отправку (mutex на WriteFile)

[`IpcClient.cs`](../src/gui/TcpRedirectorGUI/Infrastructure/Ipc/IpcClient.cs) использует `TcpClient` на `localhost:34011`.

### План исправления

**Шаг 1. Перевести IpcClient.cs с TCP на Named Pipe**

Файл: [`IpcClient.cs`](../src/gui/TcpRedirectorGUI/Infrastructure/Ipc/IpcClient.cs)

- Заменить `TcpClient` на `NamedPipeClientStream`
- Имя пары: `TcpRedirectorIpc` (как в [`PipeServer.h:169`](../src/service/TcpRedirectorService/infrastructure/ipc/PipeServer.h:169))
- Таймаут подключения: 5 секунд
- Сохранить протокол: `{method, params}` → `{response}\n`

**Шаг 2. Удалить TCP-сокет из ServiceMain.h**

Файл: [`ServiceMain.h`](../src/service/TcpRedirectorService/adapters/driving/ServiceMain.h)

- Убрать создание TCP-сокета на порту 34011
- Оставить только Named Pipe через PipeServer

**Шаг 3. Верификация**

- GUI подключается только через Named Pipe
- Не-администратор не может подключиться (ACL)
- Перезапуск GUI → переподключение работает

### Затрагиваемые файлы

| Файл | Изменение |
|------|-----------|
| [`IpcClient.cs`](../src/gui/TcpRedirectorGUI/Infrastructure/Ipc/IpcClient.cs) | TCP → NamedPipeClientStream |
| [`ServiceMain.h`](../src/service/TcpRedirectorService/adapters/driving/ServiceMain.h) | Удалить TCP accept |
| [`PipeServer.h`](../src/service/TcpRedirectorService/infrastructure/ipc/PipeServer.h) | Без изменений (уже готов) |

---

## 2. M13 — Валидация размера IPC-сообщений

### Анализ

В [`IpcHandler.h:109`](../src/service/TcpRedirectorService/adapters/driving/IpcHandler.h:109):
```cpp
auto j = nlohmann::json::parse(params);  // params — std::string без ограничений
```

Злоумышленник может послать JSON размером 500 MB → `std::bad_alloc` → падение сервиса.

### План исправления

**Шаг 1. Добавить проверку в IpcHandler::Handle()**

Файл: [`IpcHandler.h`](../src/service/TcpRedirectorService/adapters/driving/IpcHandler.h)

```cpp
static constexpr size_t MAX_IPC_MESSAGE_SIZE = 1 * 1024 * 1024; // 1 MB

void Handle(const std::string& method, const std::string& params, std::string& response) {
    if (params.size() > MAX_IPC_MESSAGE_SIZE) {
        response = R"({"status":"error","error":"message_too_large"})";
        return;
    }
    // ... existing code
}
```

**Шаг 2. Добавить проверку в PipeServer.h при чтении**

Файл: [`PipeServer.h`](../src/service/TcpRedirectorService/infrastructure/ipc/PipeServer.h)

При чтении из пайпа ограничить буфер 1 MB + 1 байт (для детекта превышения).

### Затрагиваемые файлы

| Файл | Изменение |
|------|-----------|
| [`IpcHandler.h`](../src/service/TcpRedirectorService/adapters/driving/IpcHandler.h) | Проверка `params.size()` |
| [`PipeServer.h`](../src/service/TcpRedirectorService/infrastructure/ipc/PipeServer.h) | Лимит буфера чтения |

---

## 3. M16 — CompositionRoot

### Анализ

[`CompositionRoot.h`](../src/service/TcpRedirectorService/CompositionRoot.h) (126 строк):
- Дублирует логику [`ServiceMain::Initialize()`](../src/service/TcpRedirectorService/adapters/driving/ServiceMain.h)
- Не `#include`'ится ни в одном файле проекта
- Не компилируется в составе `CMakeLists.txt`
- Мёртвый код

### План исправления

**Удалить файл** [`CompositionRoot.h`](../src/service/TcpRedirectorService/CompositionRoot.h).

Причина: ServiceMain.h уже содержит всю логику инициализации. CompositionRoot был создан как заготовка для тестирования, но не используется. При рефакторинге на IOCP (senior level) можно будет создать новый CompositionRoot с поддержкой mock-зависимостей.

### Затрагиваемые файлы

| Файл | Изменение |
|------|-----------|
| [`CompositionRoot.h`](../src/service/TcpRedirectorService/CompositionRoot.h) | Удалить |

---

## 4. Ротация логов по расписанию + автоматическое архивирование

### 4.1 Требования

1. **Ротация по времени**: ежедневно/ежечасно (настраивается в конфиге)
2. **Автоархивирование**: старые логи сжимаются в `.zip`
3. **Очистка**: удаление архивов старше N дней
4. **Конфигурация только в `config.json`** (не в UI)
5. **Отдельный поток в сервисе** с шедулером (просыпается раз в минуту, проверяет need_rotate)
6. **Сохранение существующей ротации по размеру** (защита от мгновенного переполнения)

### 4.2 Архитектура

```
┌──────────────────────────────────────────────────┐
│ Logger                                           │
│  ┌──────────┐  ┌──────────┐  ┌────────────────┐  │
│  │AsyncQueue│─►│WriterThr │─►│ Файл лога      │  │
│  └──────────┘  │          │  │ (текущий)      │  │
│                │          │  └───────┬────────┘  │
│                └──────────┘          │           │
│                                      │ ftell()   │
│                                      ▼           │
│                           ┌──────────────────┐   │
│                           │ SizeRotate()     │   │
│                           │ (существующая)   │   │
│                           │ > maxSizeMB → .1 │   │
│                           └──────────────────┘   │
└──────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────┐
│ LogRotator (новый компонент)                     │
│                                                  │
│  ┌─────────────┐    ┌────────────────────────┐   │
│  │ Scheduler   │───►│ RotateAndArchive()     │   │
│  │ Thread      │    │                        │   │
│  │ (sleep 60s) │    │ 1. fclose + rename     │   │
│  └─────────────┘    │    tcp_redirector.log  │   │
│                     │    → tcp_redirector_   │   │
│                     │      2026-07-09.log    │   │
│                     │ 2. CreateZipArchive()  │   │
│                     │    → logs_2026-07-09   │   │
│                     │      .zip              │   │
│                     │ 3. DeleteOldArchives() │   │
│                     │    > max_age_days      │   │
│                     │ 4. OpenLogFile()       │   │
│                     └────────────────────────┘   │
└──────────────────────────────────────────────────┘
```

### 4.3 Конфигурация (`config.json`)

Добавить секцию `log_rotation`:

```json
{
  "log": {
    "level": 2,
    "file_enabled": true,
    "max_size_mb": 10
  },
  "log_rotation": {
    "enabled": true,
    "schedule": "daily",
    "hour": 3,
    "minute": 0,
    "max_age_days": 30,
    "archive_dir": "C:\\ProgramData\\TcpRedirector\\logs\\archive",
    "compress": true
  }
}
```

| Параметр | Тип | По умолчанию | Описание |
|----------|-----|-------------|----------|
| `enabled` | bool | true | Включить ротацию по расписанию |
| `schedule` | string | "daily" | "hourly" или "daily" |
| `hour` | int | 3 | Час ротации (0-23, для daily) |
| `minute` | int | 0 | Минута ротации (0-59) |
| `max_age_days` | int | 30 | Сколько дней хранить архивы |
| `archive_dir` | string | logs/archive | Директория для архивов |
| `compress` | bool | true | Сжимать старые логи в zip |

### 4.4 Структура [`Config.h`](../src/service/TcpRedirectorService/infrastructure/config/Config.h)

Добавить структуру:

```cpp
struct LogRotationSettings {
    bool        enabled = true;
    std::string schedule = "daily";   // "hourly" | "daily"
    int         hour = 3;             // 0-23 (для daily)
    int         minute = 0;           // 0-59
    int         max_age_days = 30;
    std::string archive_dir;          // если пусто — logs/archive
    bool        compress = true;
};
```

Добавить поле в `Config`:
```cpp
LogRotationSettings log_rotation;
```

### 4.5 Новый файл: [`LogRotator.h`](../src/service/TcpRedirectorService/infrastructure/logging/LogRotator.h)

```cpp
#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <filesystem>
#include <functional>

namespace tcp_redirector {
namespace infrastructure {

//
// LogRotator — фоновая ротация логов по расписанию.
//
// Запускается как отдельный поток внутри сервиса.
// Просыпается раз в 60 секунд, проверяет need_rotate.
// При срабатывании:
//   1. Закрывает текущий лог-файл (через callback)
//   2. Переименовывает с добавлением даты
//   3. Сжимает старые логи в zip (если compress=true)
//   4. Удаляет архивы старше max_age_days
//   5. Открывает новый лог-файл (через callback)
//
class LogRotator {
public:
    struct Settings {
        bool enabled = true;
        std::string schedule = "daily";  // "hourly" | "daily"
        int hour = 3;
        int minute = 0;
        int max_age_days = 30;
        std::filesystem::path archive_dir;
        bool compress = true;
    };

    // Callback: закрыть текущий лог-файл
    using CloseLogCallback = std::function<void()>;
    // Callback: открыть новый лог-файл
    using OpenLogCallback = std::function<bool()>;

    LogRotator() = default;
    ~LogRotator() { Stop(); }

    bool Start(const Settings& settings,
               const std::filesystem::path& log_dir,
               const std::filesystem::path& log_path,
               CloseLogCallback close_log,
               OpenLogCallback open_log);

    void Stop();

private:
    void SchedulerThread();
    bool NeedRotate() const;
    void RotateAndArchive();
    void CreateZipArchive(const std::filesystem::path& source,
                          const std::filesystem::path& dest_zip);
    void DeleteOldArchives();

    Settings m_settings;
    std::filesystem::path m_logDir;
    std::filesystem::path m_logPath;
    CloseLogCallback m_closeLog;
    OpenLogCallback m_openLog;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::chrono::system_clock::time_point m_lastRotate;
    mutable std::mutex m_mutex;
};

} // namespace infrastructure
} // namespace tcp_redirector
```

### 4.6 Новый файл: [`LogRotator.cpp`](../src/service/TcpRedirectorService/infrastructure/logging/LogRotator.cpp)

Ключевая логика:

```
SchedulerThread():
  while (m_running):
    sleep(60s)
    if (NeedRotate()):
      RotateAndArchive()

NeedRotate():
  if (!m_settings.enabled) return false
  now = system_clock::now()
  if (now - m_lastRotate < 1min) return false  // защита от повторных

  if (schedule == "hourly"):
    return now.hour != m_lastRotate.hour
  else:  // daily
    return (now.hour == m_settings.hour &&
            now.minute == m_settings.minute &&
            now.day != m_lastRotate.day)

RotateAndArchive():
  1. m_closeLog()                    // fclose текущего
  2. timestamp = YYYY-MM-DD_HHMM
  3. rename(log → log.timestamp)     // переименовать
  4. if (compress):
       zip_path = archive_dir / logs_timestamp.zip
       CreateZipArchive(renamed_log, zip_path)
       remove(renamed_log)
  5. DeleteOldArchives()             // удалить zip старше max_age_days
  6. m_openLog()                     // открыть новый
  7. m_lastRotate = now

CreateZipArchive():
  Использовать Windows API:
  - CreateFile + DEFLATE compression
  - Или shell API: IShellDispatch::CopyHere
  - Или встроенный мини-zip (zlib/miniz)

DeleteOldArchives():
  for each *.zip in archive_dir:
    if (age > max_age_days):
      remove(zip)
```

### 4.7 Интеграция в [`Logger`](../src/service/TcpRedirectorService/infrastructure/logging/Logger.h)

Добавить:
- Поле `std::unique_ptr<LogRotator> m_rotator`
- Метод `void EnableScheduledRotation(const LogRotator::Settings& settings)`
- В `Initialize()` — создание LogRotator (если настройки переданы)

### 4.8 Интеграция в [`ConfigManager`](../src/service/TcpRedirectorService/infrastructure/config/ConfigManager.h)

- Добавить парсинг секции `log_rotation` в `JsonToConfig()`
- Добавить сохранение `log_rotation` в `ConfigToJson()`

### 4.9 Интеграция в [`ServiceMain.h`](../src/service/TcpRedirectorService/adapters/driving/ServiceMain.h)

- После `Logger::Initialize()` вызвать `EnableScheduledRotation()` с настройками из конфига

### 4.10 Сжатие в ZIP

Выбор реализации:
- **Вариант A**: `miniz` (single-header, public domain, ~10KB кода) — встроить в проект
- **Вариант B**: Windows `compressapi.dll` (`CreateCompressor`, `Compress`) — только сжатие, без zip-контейнера
- **Вариант В (рекомендуемый)**: `miniz` — даёт полноценный `.zip` с правильной структурой, не требует внешних зависимостей

Файл: скачать [`miniz.h`](https://github.com/richgel999/miniz/blob/master/miniz.h) (1 файл, ~300KB) → положить в `external/miniz/`

### Затрагиваемые файлы

| Файл | Изменение |
|------|-----------|
| [`Config.h`](../src/service/TcpRedirectorService/infrastructure/config/Config.h) | + `LogRotationSettings` |
| [`ConfigManager.h`](../src/service/TcpRedirectorService/infrastructure/config/ConfigManager.h) | + парсинг `log_rotation` |
| [`ConfigManager.cpp`](../src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp) | + парсинг/сохранение `log_rotation` |
| [`LogRotator.h`](../src/service/TcpRedirectorService/infrastructure/logging/LogRotator.h) | **НОВЫЙ** |
| [`LogRotator.cpp`](../src/service/TcpRedirectorService/infrastructure/logging/LogRotator.cpp) | **НОВЫЙ** |
| [`Logger.h`](../src/service/TcpRedirectorService/infrastructure/logging/Logger.h) | + `m_rotator`, `EnableScheduledRotation()` |
| [`Logger.cpp`](../src/service/TcpRedirectorService/infrastructure/logging/Logger.cpp) | + инициализация ротатора |
| [`ServiceMain.h`](../src/service/TcpRedirectorService/adapters/driving/ServiceMain.h) | + вызов `EnableScheduledRotation()` |
| `external/miniz/miniz.h` | **НОВЫЙ** (single-header lib) |
| `CMakeLists.txt` | + `LogRotator.cpp` в сборку |
| `config.json` (ProgramData) | + секция `log_rotation` |

---

## 5. Порядок выполнения

```
Phase 1 (1h):  M13 + M16
  ├── M13: Валидация размера IPC-сообщений
  └── M16: Удалить CompositionRoot.h

Phase 2 (3h):  C2 — IPC аутентификация
  ├── IpcClient.cs: TCP → NamedPipeClientStream
  ├── ServiceMain.h: убрать TCP accept
  └── Тестирование: GUI ↔ Service через Named Pipe

Phase 3 (4h):  Ротация логов + архивирование
  ├── Config.h: +LogRotationSettings
  ├── ConfigManager: парсинг log_rotation
  ├── external/miniz/miniz.h
  ├── LogRotator.h + LogRotator.cpp
  ├── Logger: интеграция LogRotator
  ├── ServiceMain: вызов EnableScheduledRotation()
  ├── CMakeLists.txt: +LogRotator.cpp
  └── config.json: +log_rotation секция

Phase 4 (0.5h): Обновление документации
  ├── REVIEW.md: отметить C2, M13, M16 как исправленные
  ├── LOGGER_DESIGN.md: +секция Scheduled Rotation
  └── CONFIG_DESIGN.md: +log_rotation секция
```

**Итого: ~8.5 часов**

---

## 6. Верификация

### C2
- [ ] GUI подключается только через Named Pipe
- [ ] Не-администратор получает Access Denied
- [ ] Переподключение GUI работает

### M13
- [ ] Сообщение > 1 MB → `message_too_large`
- [ ] Нормальные сообщения проходят

### M16
- [ ] CompositionRoot.h удалён
- [ ] Проект компилируется без ошибок

### Ротация
- [ ] Ежечасная/ежедневная ротация срабатывает по расписанию
- [ ] Старые логи сжимаются в .zip
- [ ] Архивы старше max_age_days удаляются
- [ ] Размерная ротация продолжает работать параллельно
- [ ] При отсутствии секции `log_rotation` — используются defaults
- [ ] Ручной триггер ротации через SetConfig не требуется