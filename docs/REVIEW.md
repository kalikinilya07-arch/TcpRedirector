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

## 2. Остаточные проблемы

### 🔴 C2. Отсутствие аутентификации IPC

TCP сокет `localhost:34011` без аутентификации — любой локальный процесс может управлять сервисом.

**Рекомендация:** перейти на Named Pipes с ACL (уже реализовано в PipeServer.h) либо добавить challenge-response.

### 🟡 M13. Нет валидации размера IPC-сообщений

`nlohmann::json::parse(params)` без ограничений — гигантский JSON → OOM.

**Рекомендация:** лимит 1 MB на входящее сообщение.

### 🟡 M16. CompositionRoot не используется

Дублирует `ServiceMain::Initialize()`, мёртвый код.

**Рекомендация:** удалить или интегрировать.

---

## 3. Статус контролов ИБ

| Контрол | Статус |
|---------|--------|
| Аутентификация IPC | ❌ (C2) |
| Шифрование в покое (пароль) | ✅ DPAPI |
| Валидация ввода (IPC) | ❌ (M13) |
| ASLR/DEP/CFG | ⚠️ проверить флаги |
| Graceful degradation | ⚠️ при ошибке WinDivert — остановка |

---

## 4. Приоритеты

1. **C2** — Аутентификация IPC
2. **M13** — Валидация IPC-сообщений
3. **M16** — CompositionRoot
