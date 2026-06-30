# Анализ: Named Pipe vs TCP Socket для IPC

## Текущая ситуация

Сервис логирует `IPC server started`, но пайп `\\.\pipe\TcpRedirectorService` недоступен:
- PowerShell: `Connect()` → таймаут
- GUI: `ConnectAsync(2000)` → таймаут → "disconnected"

При этом WinDivert-захват и relay работают (трафик идёт).

## Причина молчаливого отказа неизвестна

[`PipeServer.h:183-186`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/ipc/PipeServer.h:183) — при `CreateNamedPipeW` → `INVALID_HANDLE_VALUE` поток молча засыпает на 1с и ретраит. Никакого лога. Возможные причины отказа:
- `FILE_FLAG_FIRST_PIPE_INSTANCE` конфликтует с зомби-пайпом от предыдущего краша
- SDDL `ConvertStringSecurityDescriptorToSecurityDescriptorW` вернул `nullptr` → пайп создаётся с default security (должен работать)
- `PIPE_UNLIMITED_INSTANCES` + `FILE_FLAG_FIRST_PIPE_INSTANCE` — странная комбинация

---

## Вариант A: Починить Named Pipe (рекомендуется)

### Плюсы
- Код уже написан (C++ PipeServer + C# IpcClient)
- Минимум изменений — только диагностика + возможный фикс одной строки
- Windows-native, нет портов, нет фаервола

### План
1. Добавить `m_logSink->Log(Error, ...)` в PipeServer при `CreateNamedPipeW == INVALID_HANDLE_VALUE` — увидим `GetLastError()`
2. Добавить лог при `ConnectNamedPipe` с неожиданным кодом ошибки
3. Убрать `FILE_FLAG_FIRST_PIPE_INSTANCE` после первого создания (или всегда использовать без него)
4. Пересобрать, запустить, прочитать лог → корневая причина будет видна

### Время
~15 минут на изменения + сборку

---

## Вариант B: Перейти на TCP socket (localhost)

### Архитектура

```
Сервис (C++)                          GUI (C#)
┌──────────────────┐                  ┌──────────────────┐
│ TcpListener      │                  │ TcpClient        │
│ 127.0.0.1:34011  │ ←── TCP/IP ──→  │ ConnectAsync()   │
│ JSON request/rsp │                  │ JSON request/rsp │
└──────────────────┘                  └──────────────────┘
```

### C++ сторона (замена PipeServer)
```cpp
// Простой однопоточный TCP listener
SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
sockaddr_in addr = { AF_INET, htons(34011), inet_addr("127.0.0.1") };
bind(sock, &addr, sizeof(addr));
listen(sock, 1);
while (running) {
    SOCKET client = accept(sock, nullptr, nullptr);
    // read JSON line, process, write response, close
}
```

### C# сторона (замена IpcClient)
```csharp
// Замена NamedPipeClientStream на TcpClient
var tcp = new TcpClient();
await tcp.ConnectAsync("127.0.0.1", 34011);
var stream = tcp.GetStream();
// Write JSON, Read JSON
```

### Плюсы
- **Диагностика тривиальна**: `telnet 127.0.0.1 34011` → сразу видно, жив сервис или нет
- Нет магии Windows named pipes (SDDL, `FILE_FLAG_FIRST_PIPE_INSTANCE`, `ConnectNamedPipe` quirks)
- Порт жёстко зафиксирован — никаких гонок
- `netstat -ano | findstr 34011` покажет, слушает ли сервис

### Минусы
- ~100 строк C++ кода написать заново
- ~50 строк C# кода переписать
- Порт 34011 должен быть свободен (можно сделать настраиваемым)
- Любой локальный пользователь может подключиться (для localhost-сервиса это приемлемо)

### Время
~30-45 минут на написание и отладку

---

## Сравнение

| Критерий | Named Pipe (fix) | TCP Socket |
|----------|-----------------|------------|
| Объём изменений | ~10 строк | ~150 строк |
| Диагностика | Сложная (нужны спец. инструменты) | Тривиальная (telnet, netstat, curl) |
| Надёжность | Зависит от Windows API quirks | Стандартный TCP |
| Безопасность | Админы + SYSTEM (через SDDL) | Любой локальный пользователь |
| Блокировки | Возможны зомби-пайпы | Порт может быть занят |

---

## Рекомендация

**Вариант A** — сначала добавить диагностику в PipeServer и понять корневую причину. С высокой вероятностью это мелкий баг (например, `FILE_FLAG_FIRST_PIPE_INSTANCE` при повторном создании пайпа). 10 строк лога решат проблему за 15 минут.

Если после диагностики окажется, что named pipe принципиально не работает в данном окружении — тогда **Вариант B** как fallback.
