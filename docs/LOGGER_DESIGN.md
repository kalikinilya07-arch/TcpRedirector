# Логирование TcpRedirector

## Архитектура

Файл: [`Logger.h`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/logging/Logger.h), [`Logger.cpp`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/logging/Logger.cpp)

### Компоненты

```
┌─────────────┐     ┌──────────────┐     ┌────────────────┐
│ Callers      │────►│ AsyncQueue   │────►│ WriterThread   │
│ (много       │     │ (lock-free)  │     │ (один поток)   │
│  потоков)    │     │              │     │                │
└─────────────┘     └──────────────┘     ├──► Файл лога   │
                                         ├──► Ring buffer  │
                                         └──► Console      │
```

- **AsyncQueue**: lock-free очередь сообщений между caller-потоками и writer-потоком
- **WriterThread**: выделенный поток для записи в файл (избегает блокировки caller-потоков)
- **Ring buffer**: 2000 последних записей в памяти для GUI (`get_logs`)

### Уровни логирования

| Уровень | Значение | Назначение |
|---------|----------|------------|
| TRACE | 0 | Максимальная детализация (все пакеты) |
| DEBUG | 1 | Отладочная информация |
| INFO | 2 | Штатные события (запуск, остановка) |
| WARN | 3 | Предупреждения |
| ERROR | 4 | Ошибки |

### Формат записи

```
[2026-07-09 19:06:20.146] [INFO ] [service     ] Initializing TcpRedirector Service...
```

- Timestamp с миллисекундами
- Уровень логирования (выровнен до 5 символов)
- Источник (выровнен до 12 символов): `service`, `relay`, `windivert`, `capture`, `ipc`, `config`
- Сообщение

### Ротация логов

**Два уровня ротации:**

1. **Размерная (мгновенная):** при превышении `maxSizeMB` (по умолчанию 10 MB) файл переименовывается с добавлением `.1`, `.2`, и т.д. Максимум 5 файлов.

2. **По расписанию (фоновая):** `LogRotator` — отдельный поток, просыпается раз в 60 секунд, проверяет расписание. Настройки только в [`config.json`](#конфигурация-ротации) (не в UI).

   - Закрывает текущий лог-файл
   - Переименовывает с timestamp: `tcp_redirector_2026-07-10_0300.log`
   - Сжимает в `.zip` через PowerShell `Compress-Archive`
   - Удаляет архивы старше `max_age_days`
   - Открывает новый лог-файл

#### Конфигурация ротации

Секция `log_rotation` в `config.json`:

```json
{
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
| `minute` | int | 0 | Минута ротации |
| `max_age_days` | int | 30 | Срок хранения архивов |
| `archive_dir` | string | logs/archive | Директория для архивов |
| `compress` | bool | true | Сжимать в .zip |

### Потокобезопасность

- `Log()` — lock-free push в очередь
- WriterThread — единственный читатель очереди
- Ring buffer — защищён мьютексом (только для чтения из GUI)

---

## WinDivert debug log

Отдельный файл: `%ProgramData%\TcpRedirector\logs\windivert_debug.log`

Включает TRACE-записи о каждом пакете: `[PROXIED]`, `[MISSED]`, `[DIRECT]`. Включается только при компиляции с `WD_TRACE`. В production-сборке TRACE-записи отключены.