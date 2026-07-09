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

При превышении `maxSizeMB` (по умолчанию 10 MB) файл переименовывается с добавлением `.1`, `.2`, и т.д. Максимум 5 файлов ротации.

### Потокобезопасность

- `Log()` — lock-free push в очередь
- WriterThread — единственный читатель очереди
- Ring buffer — защищён мьютексом (только для чтения из GUI)

---

## WinDivert debug log

Отдельный файл: `%ProgramData%\TcpRedirector\logs\windivert_debug.log`

Включает TRACE-записи о каждом пакете: `[PROXIED]`, `[MISSED]`, `[DIRECT]`. Включается только при компиляции с `WD_TRACE`. В production-сборке TRACE-записи отключены.