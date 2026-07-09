# Архитектурный анализ TcpRedirector

## 1. Обзор системы

TcpRedirector — сервис перехвата и перенаправления TCP-трафика через HTTP CONNECT прокси. Работает на уровне Network Layer (L3) через драйвер WinDivert, модифицируя DST-адрес пакетов целевых процессов на локальный relay-сервер, который устанавливает туннель к upstream-прокси.

### Ключевые компоненты

```
┌──────────────────────────────────────────────────────────────────┐
│                        TcpRedirector                             │
├───────────────┬──────────────────┬───────────────────────────────┤
│  WinDivert    │  TcpRelayServer  │  GUI (WPF)                    │
│  Capture      │  (localhost:     │                                │
│  (L3 driver)  │   34010)         │  ┌─────────┐ ┌──────────┐    │
│               │                  │  │Settings │ │ Stats    │    │
│  DST-modify   │  HTTP CONNECT →  │  │ViewModel│ │ViewModel │    │
│  target→relay │  upstream proxy  │  └─────────┘ └──────────┘    │
│               │                  │  ┌──────────────────────┐     │
│               │  Auth: Basic /   │  │   ShellViewModel     │     │
│               │  Negotiate(SSPI) │  │   (service lifecycle)│     │
│               │                  │  └──────────────────────┘     │
├───────────────┴──────────────────┴───────────────────────────────┤
│  IPC: TCP socket (localhost:34011) — JSON-RPC style              │
│  Config: %ProgramData%\TcpRedirector\config.json                 │
│  Logs:  %ProgramData%\TcpRedirector\logs\                        │
└──────────────────────────────────────────────────────────────────┘
```

---

## 2. Механизм редиректа (DST-modification)

### 2.1. Принцип работы

В отличие от классического проксирования (где приложение явно настраивается на прокси), TcpRedirector использует **прозрачный перехват** на уровне сетевых пакетов:

```
До модификации:
  Client App ──SYN──► Target Server (original DST)

После модификации:
  Client App ──SYN──► TcpRelayServer:34010  (DST изменён)
  TcpRelayServer ──CONNECT──► Upstream Proxy:3128
  Upstream Proxy ──SYN──► Target Server
```

### 2.2. 4-шаговый цикл CaptureLoop

Файл: [`WinDivertCapture.cpp`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/capture/WinDivertCapture.cpp:289)

```
while (m_running) {
    packet = WinDivertRecv()          // 1. Захват пакета из NDIS
    ParseIpTcpHeaders(packet)         // 2. Парсинг IP/TCP заголовков
    
    if (port in decided_bitmap) {     // 3. Fast path — кэшированное решение
        if (direct_bitmap) → pass
        else → DST → relay
    }
    
    if (relay response) {             // 4. Ответ от relay → restore DST
        RestoreFromRelay()
    }
    
    if (tracked connection) {         // 5. Клиентские данные → DST → relay
        ModifyDstToRelay()
    }
    
    if (untracked outbound) {         // 6. Новое соединение → CheckProcessRule
        action = CheckProcessRule()
        if (PROXY) → Add to ConnectionTable, ModifyDstToRelay
        if (DIRECT) → SetPortDirect, pass
        if (BLOCK) → drop
    }
}
```

### 2.3. Per-port bitmap (кэш решений)

Два битмапа по 65536 бит (8 KB каждый):
- `m_portDecided[2048]` — бит = 1: решение для порта кэшировано
- `m_portDirect[2048]` — бит = 1: DIRECT (пропускать), 0: PROXY/BLOCK

**Три состояния порта:**
| decided | direct | Значение |
|---------|--------|----------|
| 0 | - | Нет кэша → CheckProcessRule |
| 1 | 1 | DIRECT → пропустить без изменений |
| 1 | 0 | PROXY/BLOCK → уже в ConnectionTable |

Thread safety: `InterlockedOr`/`InterlockedAnd` для записи; aligned 32-bit read атомарен на x86/x64.

### 2.4. ConnectionTable

Файл: [`ConnectionTable.h`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/relay/ConnectionTable.h:38)

Ключ: **(src_port, orig_dest_ip)** — предотвращает коллизии при повторном использовании эфемерных портов разными целевыми соединениями.

```cpp
struct ConnectionEntry {
    uint16_t src_port;
    uint32_t orig_dest_ip;
    uint16_t orig_dest_port;
    uint32_t proxy_config_id;
    // ...
};
```

---

## 3. TcpRelayServer — HTTP CONNECT туннель

### 3.1. Жизненный цикл соединения

Файл: [`TcpRelayServer.h`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h)

```
1. Accept() клиента (localhost:34010)
2. Извлечь orig_dest из ConnectionTable по src_port
3. Connect() к upstream прокси
4. Отправить CONNECT orig_host:orig_port HTTP/1.1
   + опционально Proxy-Authorization: Basic/Negotiate
5. Дождаться HTTP/1.x 200
6. Bridge: client ↔ proxy (bidirectional relay)
7. При закрытии: shutdown(SD_SEND) для half-close
```

### 3.2. Аутентификация

Файл: [`TcpRelayServer.h`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h:337)

```cpp
if (m_proxyAuthRequired && !m_kerberosAuth) {
    // Basic Auth: Base64(login:password)
    connect_req += "Proxy-Authorization: Basic " + Base64Encode(basic);
    
} else if (m_kerberosAuth && sspiAvailable) {
    // Negotiate/Kerberos через SSPI
    SspiNegotiate(ctx, "", sspiToken, MakeSpn(proxyHost));
    connect_req += "Proxy-Authorization: Negotiate " + sspiToken;
}
```

**Kerberos и Basic — взаимоисключающие.** При включении Kerberos GUI авто-включает Auth Required. При выключении Auth Required GUI авто-выключает Kerberos.

#### SSPI Negotiate (Kerberos/NTLM)

Файл: [`auth_sspi.cpp`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp:46)

```
1. AcquireCredentialsHandle("Negotiate") — получает Kerberos ticket или NTLM creds
2. InitializeSecurityContext → токен
3. Отправка: Proxy-Authorization: Negotiate <base64 token>
4. Если прокси отвечает 407 + Proxy-Authenticate: Negotiate <challenge>:
   → Извлечь challenge (Base64 decode)
   → InitializeSecurityContext(challenge) → ответный токен
   → Повторный CONNECT с новым токеном
```

**Ограничение:** максимум 3 раунда SSPI (исходный + 2 перезапроса).

### 3.3. Управление памятью потоков

- **m_activePairs** (atomic) — счётчик активных relay-пар
- При Stop(): ожидание до 5 секунд пока m_activePairs не станет 0
- Только после этого вызывается WSACleanup()
- Предотвращает use-after-free сокетов при остановке сервиса

### 3.4. Half-close (M1 fix)

При закрытии одной стороны используется `shutdown(SD_SEND)` вместо `shutdown(SD_BOTH)`:
- Данные, ещё находящиеся в полёте от противоположной стороны, не теряются
- Соответствует TCP graceful close

---

## 4. Аутентификация и хранение секретов

### 4.1. DPAPI шифрование пароля

Файл: [`ConfigManager.cpp`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp:428)

```
Шифрование:
  plain_password → CryptProtectData(
      CRYPTPROTECT_UI_FORBIDDEN,  // не показывать UI
      entropy: "TcpRedirectorProxyPassword"
  ) → Base64 → config.json

Дешифрование:
  config.json → Base64 decode → CryptUnprotectData → plain_password (in-memory)
```

**Флаг `CRYPTPROTECT_UI_FORBIDDEN`**: пароль привязан к учётной записи пользователя, выполнившего шифрование. Сервис (SYSTEM) и GUI (пользователь) должны использовать один и тот же флаг для совместимости.

### 4.2. Base64 (H2 fix)

Дешифрование помечает padding `'='` как `0xFF` и пропускает эти байты:
```cpp
D['='] = 0xFF;  // padding must be skipped, not decoded as data
```

---

## 5. Конфигурация

### 5.1. Структура config.json

Файл: `%ProgramData%\TcpRedirector\config.json`

```json
{
  "app":  { "exePath": "C:\\...\\chrome.exe" },
  "proxy": { "host": "127.0.0.1", "port": 3128, "enabled": true },
  "auth": {
    "enabled": false,
    "username": "",
    "encryptedPassword": "",
    "kerberos": false
  },
  "log": { "level": 2, "fileEnabled": true, "maxSizeMB": 10 },
  "stats": { "updateIntervalMs": 2000 },
  "rules": [
    { "id": "...", "type": 0, "action": 1, "pattern": "chrome.exe", ... }
  ]
}
```

### 5.2. RuleEngine

Файл: `domain/services/RuleEngine.h`

- **ProcessName**: сравнение по имени файла (поддерживает `*`)
- **ProcessPath**: сравнение по полному пути
- **Global**: применяется ко всем процессам
- **Приоритет**: меньше = выше (правило с priority=0 проверяется первым)
- **Действия**: Proxy (1), Direct (0), Block (2)

### 5.3. ConfigManager

Файл: [`ConfigManager.cpp`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp)

- **Потокобезопасность**: `std::shared_mutex` — multiple readers, exclusive writer
- **Сохранение**: атомарная запись (write to temp → rename)
- **Валидация**: при загрузке проверяет наличие всех секций, подставляет defaults
- **DPAPI**: пароль шифруется при записи, расшифровывается в in-memory `plain_password`

---

## 6. IPC (межпроцессное взаимодействие)

### 6.1. Протокол

Файлы: [`IpcHandler.h`](TcpRedirector/src/service/TcpRedirectorService/adapters/driving/IpcHandler.h), [`IpcClient.cs`](TcpRedirector/src/gui/TcpRedirectorGUI/Infrastructure/Ipc/IpcClient.cs)

Транспорт: **TCP socket (localhost:34011)**

**Методы:**
| Метод | Направление | Описание |
|-------|-------------|----------|
| `get_config` | GUI → Service | Получить текущую конфигурацию |
| `set_config` | GUI → Service | Обновить конфигурацию |
| `get_rules` | GUI → Service | Получить правила фильтрации |
| `set_rules` | GUI → Service | Обновить правила |
| `get_connections` | GUI → Service | Получить активные соединения |
| `get_logs` | GUI → Service | Получить логи (ring buffer) |
| `get_stats` | GUI → Service | Получить статистику |
| `get_service_status` | GUI → Service | Статус сервиса |
| `set_log_level` | GUI → Service | Изменить уровень логирования |

Формат: JSON-RPC style — `{"method":"...", "params":{...}}` → `{"status":"success", "data":{...}}`

### 6.2. PipeServer (устаревший, заменён на TcpIpcServer)

Файл: [`PipeServer.h`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/ipc/PipeServer.h)

- M7 fix: WriteFile ответов защищён мьютексом `m_pipeMutex`
- C1 fix: Pipe создаётся с security descriptor (только SYSTEM + Administrators)

---

## 7. Логирование и статистика

### 7.1. Logger

Файл: [`Logger.h`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/logging/Logger.h)

- **Асинхронный**: очередь сообщений + worker thread
- **Ring buffer**: 2000 последних записей для GUI
- **Файловый лог**: `%ProgramData%\TcpRedirector\logs\service.log`
- **Ротация**: при превышении `maxSizeMB`
- **Уровни**: TRACE=0, DEBUG=1, INFO=2, WARN=3, ERROR=4

### 7.2. StatsCollector

Файл: [`StatsCollector.h`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/stats/StatsCollector.h)

- **Счётчики**: packets, bytes (RX/TX), connections, redirects, errors
- **Все счётчики — `std::atomic`**, lock-free
- **Per-second rate**: вычисляется дифференциально в `GetStats()`

---

## 8. GUI (WPF, .NET 9.0)

### 8.1. Архитектура

- **ShellViewModel**: управление сервисом (start/stop/auto-connect)
- **SettingsViewModel**: конфигурация прокси, правил, аутентификации
- **StatsViewModel**: отображение статистики
- **IpcClient**: TCP-клиент для связи с сервисом
- **JsonConfigRepository**: чтение/запись config.json
- **ServiceController**: управление Windows-сервисом через SCM

### 8.2. Auto-start

При запуске GUI:
1. Пытается подключиться к уже работающему сервису
2. Если сервис не запущен — находит `TcpRedirectorService.exe`, запускает в режиме `--console`
3. Подключается и начинает polling статистики

### 8.3. Аутентификация в GUI

Kerberos и Basic — взаимоисключающие:
- `OnKerberosEnabledChanged(true)` → `AuthRequired = true`
- `OnAuthRequiredChanged(false)` → `KerberosEnabled = false`

---

## 9. Модель угроз и требования ИБ

### 9.1. Доверенные границы

```
┌──────────────┐     ┌──────────────┐     ┌─────────────────┐
│ Целевое      │     │ TcpRedirector│     │ Upstream Proxy  │
│ приложение   │────►│ Service      │────►│ (внешний)       │
│ (chrome.exe) │ LAN │ (SYSTEM)     │ LAN │                 │
└──────────────┘     └──────────────┘     └─────────────────┘
                            ▲
                     ┌──────┴──────┐
                     │ GUI (user)  │
                     │ IPC (TCP)   │
                     └─────────────┘
```

### 9.2. Векторы атак

| Вектор | Риск | Митигация | Статус |
|--------|------|-----------|--------|
| Локальный пользователь подключается к IPC и меняет конфиг | HIGH | Pipe SD (C1 fix): только SYSTEM + Admins | ✅ |
| Перехват пароля в памяти сервиса | MEDIUM | DPAPI + пароль в памяти только при использовании | ✅ |
| Подделка CONNECT-ответа от прокси | MEDIUM | M2 fix: проверка `HTTP/1.x 200` в начале ответа | ✅ |
| WinDivert driver не установлен | HIGH | EnsureDriverRunning() — автостарт | ✅ |
| Краш сервиса при отсутствии драйвера | HIGH | H5 fix: проверка указателей перед вызовом | ✅ |
| Утечка данных через half-close | MEDIUM | M1 fix: shutdown(SD_SEND) вместо SD_BOTH | ✅ |
| Повторное использование портов — смешение трафика | HIGH | M3 fix: compound key (port, ip) | ✅ |
| PID lookup per packet — DoS через нагрузку CPU | MEDIUM | H6 fix: PID cache 30s TTL | ✅ |
| Recursive lock в ConnectionTracker | HIGH | M8 fix: убран рекурсивный захват | ✅ |
| Process handle leak | LOW | M12 fix: Dispose() в finally | ✅ |

### 9.3. Остаточные риски

| Риск | Описание | Рекомендация |
|------|----------|--------------|
| Отсутствие mutual TLS | IPC между GUI и сервисом без шифрования | Добавить TLS или перейти на Named Pipes с шифрованием |
| Отсутствие аутентификации IPC | Любой процесс на localhost:34011 может отправлять команды | Добавить аутентификацию (токен/сертификат) |
| WinDivert — сторонний драйвер | Потенциальная несовместимость с обновлениями Windows | Мониторить обновления WinDivert |
| Self-contained GUI | EXE + DLL содержат весь .NET runtime — большой вектор атаки | Использовать framework-dependent publish |
| Нет ASLR/CFG для сервиса | C++ сервис без /DYNAMICBASE и /GUARD:CF | Включить в vcxproj |
| Логи могут содержать конфиденциальные данные | IP-адреса и порты пишутся в лог | Добавить санитизацию логов |
