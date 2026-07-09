# Security Review: TcpRedirector

## Состояние: после аудита и исправлений (ветка `fix/audit-review-fixes`)

---

## 1. Исправленные уязвимости

### 🔴 CRITICAL

| ID | Уязвимость | Исправление | Файл |
|----|-----------|-------------|------|
| C1 | Named Pipe без security descriptor — любой пользователь управляет SYSTEM-сервисом | Добавлен SECURITY_DESCRIPTOR: доступ только SYSTEM + Administrators | `PipeServer.h` |
| H1 | Захардкоженный пароль `:proxy_pass` вместо настроенного | Используется пароль из конфига через DPAPI | `TcpRelayServer.h:339` |
| H2 | Base64 декодер трактует padding `=` как данные — ломает SSPI токены и DPAPI пароль | `D['='] = 0xFF` — padding пропускается | `ConfigManager.cpp:489` |
| H7 | `Initialize()` всегда возвращает `true` — сервис рапортует RUNNING при отказе | Возвращает `false` при ошибке WinDivert/Capture | `ServiceMain.h:129-134` |

### 🟠 HIGH

| ID | Уязвимость | Исправление | Файл |
|----|-----------|-------------|------|
| H3 | Таймаут 30с утекает из CONNECT в фазу данных | После CONNECT: SO_RCVTIMEO/SO_SNDTIMEO=0 + SO_KEEPALIVE | `TcpRelayServer.h` |
| H4 | Потоки relay не отслеживаются, WSACleanup при живых потоках | m_activePairs atomic + ожидание до 5с в Stop() | `TcpRelayServer.h` |
| H5 | Краш по null-указателю функции при неудаче WinDivertOpen | Close() проверяет m_api.Shutdown/Close != nullptr | `WinDivertCapture.cpp:171-176` |
| H6 | PID определяется на КАЖДЫЙ untracked-пакет (TCP table scan) | PID cache с TTL 30 секунд | `WinDivertCapture.h:157-164` |

### 🟡 MEDIUM

| ID | Уязвимость | Исправление | Файл |
|----|-----------|-------------|------|
| M1 | Потеря данных при half-close (SD_BOTH) | shutdown(SD_SEND) — graceful close | `TcpRelayServer.h` |
| M2 | CONNECT успех определяется подстрокой `"200"` где угодно | Проверка `HTTP/1.x 200` в начале ответа | `TcpRelayServer.h` |
| M3 | ConnectionTable ключуется только по src_port — коллизии | Compound key: (src_port, orig_dest_ip) | `ConnectionTable.h` |
| M4 | `getenv("ProgramData")` возвращает nullptr → UB | Fallback на `C:\ProgramData\...` | `ServiceMain.h:39-43` |
| M5 | Нет try/catch вокруг Initialize()/Run() → terminate | Оборачивание в try/catch | `main.cpp:35,73` |
| M6 | Порча не-ASCII через `wstring(s.begin(), s.end())` (28 мест) | Utf8ToWide/WideToUtf8 через MultiByteToWideChar CP_UTF8 | 4 файла |
| M7 | PipeServer: гонка при конкурентной записи | m_pipeMutex для WriteFile | `PipeServer.h` |
| M8 | ConnectionTracker: рекурсивный shared_mutex → латентный дедлок | Исправлена логика захвата | `ConnectionTracker.h` |
| M9 | Конфиг: нет атомарного сохранения | Write to temp → rename | `ConfigManager.cpp` |
| M10 | SCM-обработчик с неверной сигнатурой | Исправлена сигнатура HandlerEx | `main.cpp:38` |
| M11 | Неверный порядок остановки: WinDivert закрывается последним | Исправлен порядок в Stop() | `ServiceMain.h` |
| M12 | Process handle leak + DPAPI флаги mismatch | Dispose() + CRYPTPROTECT_UI_FORBIDDEN | `ServiceController.cs`, `ConfigManager.cpp` |

---

## 2. Остаточные проблемы (требуют исправления)

### 🔴 CRITICAL

**C2. Отсутствие аутентификации IPC**

Файлы: [`IpcHandler.h`](TcpRedirector/src/service/TcpRedirectorService/adapters/driving/IpcHandler.h), [`IpcClient.cs`](TcpRedirector/src/gui/TcpRedirectorGUI/Infrastructure/Ipc/IpcClient.cs)

IPC использует TCP сокет на `localhost:34011` без какой-либо аутентификации. Любой процесс на локальной машине может прочитать конфигурацию (включая зашифрованный пароль), изменить правила фильтрации, перенаправить трафик на свой прокси для MITM.

**Рекомендация:** перейти на Named Pipes с ACL (уже частично реализовано в PipeServer.h) либо добавить challenge-response аутентификацию при подключении.

### 🟠 HIGH

**H8. Однопоточный блокирующий pipe-сервер без таймаутов → локальный DoS**

Файл: [`PipeServer.h`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/ipc/PipeServer.h)

`ConnectNamedPipe` — блокирующий вызов. Злоумышленник может открыть pipe-соединения и не закрывать их — сервис зависнет.

**Рекомендация:** использовать `OVERLAPPED` I/O с таймаутом либо полностью перейти на TCP сокет с `select()`/`poll()`.

### 🟡 MEDIUM

**M13. IPC сообщения не валидируются на размер**

Файл: [`IpcHandler.h:108`](TcpRedirector/src/service/TcpRedirectorService/adapters/driving/IpcHandler.h:108)

`nlohmann::json::parse(params)` без проверки размера. Гигантский JSON → OOM или краш.

**Рекомендация:** ограничить размер входящего JSON (1 MB).

**M14. Нет rate limiting на IPC**

Файл: [`IpcHandler.h`](TcpRedirector/src/service/TcpRedirectorService/adapters/driving/IpcHandler.h)

Спам IPC-запросами → DoS через исчерпание CPU/памяти.

**Рекомендация:** rate limiting (10 запросов/сек на соединение).

**M15. Логирование конфиденциальных данных**

Файл: [`WinDivertCapture.cpp:379-385`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/capture/WinDivertCapture.cpp:379)

IP-адреса и порты назначения пишутся в лог: `[PROXIED] chrome.exe srcPort=%u 173.194.222.138:443 ...`

**Рекомендация:** режим логирования без IP-адресов или маскирование.

**M16. CompositionRoot не используется**

Файлы: [`CompositionRoot.h`](TcpRedirector/src/service/TcpRedirectorService/CompositionRoot.h), [`ServiceMain.h`](TcpRedirector/src/service/TcpRedirectorService/adapters/driving/ServiceMain.h)

`CompositionRoot::CreateFromConfig()` дублирует логику `ServiceMain::Initialize()`. CompositionRoot — мёртвый код.

**Рекомендация:** удалить CompositionRoot или перевести ServiceMain на его использование.

### 🟢 LOW

| ID | Проблема | Рекомендация |
|----|----------|--------------|
| L1 | `localtime` не потокобезопасен в Logger | `localtime_s` |
| L2 | Ring buffer в Logger не ограничивает размер записи | Truncation для длинных сообщений |
| L3 | Debug логи WinDivert пишутся синхронно | Перенести в асинхронный Logger |
| L4 | Нет обработки `WM_QUERYENDSESSION` в GUI | Сохранять настройки перед shutdown |
| L5 | `printf` в режиме `--console` | Заменить на Logger |

---

## 3. Статус контролов ИБ

| Контрол | Статус |
|---------|--------|
| Аутентификация IPC | ❌ Нет (C2) |
| Авторизация IPC | ⚠️ Pipe ACL (C1 fix), но используется TCP |
| Шифрование в покое (пароль) | ✅ DPAPI |
| Валидация ввода (IPC) | ❌ Нет (M13) |
| Rate limiting (IPC) | ❌ Нет (M14) |
| ASLR/DEP/CFG | ⚠️ Нужно проверить флаги vcxproj |
| Логирование | ⚠️ IP-адреса в логах (M15) |
| Graceful degradation | ⚠️ При ошибке WinDivert — сервис останавливается |

---

## 4. Приоритеты исправления

1. **C2** — Аутентификация IPC
2. **M13** — Валидация размера IPC-сообщений
3. **H8** — Таймауты pipe-сервера
4. **M14** — Rate limiting IPC
5. **M15** — Санитизация логов
6. **M16** — CompositionRoot
7. L1-L5 — Косметические улучшения
