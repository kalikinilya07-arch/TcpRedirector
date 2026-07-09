# Проектирование системы конфигурации

## 1. Структура Config

### 1.1. Настройки приложения (AppSettings)

```json
{
  "app": {
    "exePath": "C:\\Projects\\china\\police_sec\\TransfersClient.exe",
    "exeName": "TransfersClient.exe"
  }
}
```

- **`exePath`** — полный путь к EXE, трафик которого перехватываем.
  Используется в `WinDivertCapture::IsTargetProcess()` и `FindTargetPid()` для сравнения через `_wcsicmp`.
- **`exeName`** — извлекается автоматически из `exePath` (последний компонент после `\\`).
  Используется в фильтре WinDivert и в `RuleEngine::ShouldRedirect()`.

### 1.2. Настройки прокси (ProxySettings)

```json
{
  "proxy": {
    "host": "127.0.0.1",
    "port": 3128,
    "enabled": true
  }
}
```

- **`host`** — адрес HTTP-прокси (IPv4 или hostname).
- **`port`** — порт HTTP-прокси.
- **`enabled`** — если `false`, трафик НЕ перенаправляется на прокси.
  Пакеты просто пропускаются через WinDivert без редиректа (режим мониторинга).

Используется в `ProxyEngine::ConnectToProxy()`:
- `gethostbyname(proxy_host)` → `connect(m_proxySocket, &proxy_addr)`
- `SendConnectRequest()` → `CONNECT original_host:original_port HTTP/1.1`

### 1.3. Настройки авторизации (AuthSettings)

```json
{
  "auth": {
    "enabled": false,
    "username": "",
    "encryptedPassword": "",
    "kerberos": false
  }
}
```

- **`enabled`** — если `true`, в CONNECT-запрос добавляется `Proxy-Authorization: Basic ...`.
- **`username`** — открытый текст.
- **`encryptedPassword`** — пароль, зашифрованный через **DPAPI** (`CryptProtectData`).
  При загрузке расшифровывается в `plainPassword` (in-memory only, никогда не пишется в JSON).
- **`kerberos`** — если `true`, `username`/`encryptedPassword` игнорируются.
  Вместо Basic-авторизации используется Negotiate/Kerberos (перспективно).

Используется в `ProxySession::SendConnectRequest()`:
```cpp
if (m_config.auth_required && m_config.has_password) {
    connect_request += "Proxy-Authorization: Basic " + Base64Encode(credentials);
}
```

### 1.4. Настройки логирования (LogSettings)

```json
{
  "log": {
    "level": 2,
    "fileEnabled": true,
    "maxSizeMB": 10
  }
}
```

- **`level`**: `0=ERROR`, `1=WARN`, `2=INFO`, `3=DEBUG`.
- **`fileEnabled`** — если `true`, лог пишется в `%ProgramData%\TcpRedirector\logs\`.
- **`maxSizeMB`** — максимальный размер файла лога. При превышении — ротация (`.1`, `.2`, ...).

Используется в `Logger::Initialize()` и `Logger::SetLevel()`.

### 1.5. Настройки статистики (StatsSettings)

```json
{
  "stats": {
    "updateIntervalMs": 2000
  }
}
```

- **`updateIntervalMs`** — как часто GUI обновляет статистику (активные соединения, RX/TX bytes).
  Используется в `IpcHandler::GetStats()` и GUI-таймере.

### 1.6. Полный пример config.json

```json
{
  "app": {
    "exePath": "C:\\Projects\\china\\police_sec\\TransfersClient.exe",
    "exeName": "TransfersClient.exe"
  },
  "proxy": {
    "host": "127.0.0.1",
    "port": 3128,
    "enabled": true
  },
  "auth": {
    "enabled": false,
    "username": "",
    "encryptedPassword": "",
    "kerberos": false
  },
  "log": {
    "level": 2,
    "fileEnabled": true,
    "maxSizeMB": 10
  },
  "stats": {
    "updateIntervalMs": 2000
  }
}
```

---

## 2. ConfigManager — класс управления конфигурацией

### 2.1. Конструктор / Инициализация

```
ConfigManager()
```

- **Путь к файлу**: `%ProgramData%\TcpRedirector\config\config.json`
  - Используется `GetEnvironmentVariableW(L"ProgramData", ...)` — уже есть в `WinDivertCapture.cpp:79`.
  - Создаётся директория `%ProgramData%\TcpRedirector\config\`, если не существует.
- **Загрузка**: если файл существует — читает и парсит JSON.
  Если не существует — создаёт с **значениями по умолчанию** (текущие hardcoded значения).
- **Валидация**: при загрузке проверяет, что все поля присутствуют.
  Если какого-то поля нет — подставляет значение по умолчанию (версионирование конфига).

### 2.2. Методы

| Метод | Сигнатура | Описание |
|-------|-----------|----------|
| `Load` | `bool Load()` | Загружает config.json с диска. Возвращает `false`, если файл не найден (создаёт дефолтный). |
| `Save` | `bool Save()` | Сохраняет текущую конфигурацию в config.json. |
| `GetConfig` | `Config GetConfig() const` | Возвращает **копию** текущей конфигурации (потокобезопасно). |
| `UpdateConfig` | `bool UpdateConfig(const Config& newConfig)` | Обновляет конфигурацию, шифрует пароль (если изменился), сохраняет на диск. |
| `GetPlainPassword` | `std::wstring GetPlainPassword() const` | Возвращает расшифрованный пароль (in-memory). |
| `SetPassword` | `void SetPassword(const std::wstring& plainPassword)` | Шифрует пароль через DPAPI и сохраняет в `m_config.auth.encryptedPassword`. |

### 2.3. Потокобезопасность

- **Примитив**: `mutable std::shared_mutex m_mutex`
- **Чтение** (`GetConfig`, `GetPlainPassword`): `std::shared_lock` — multiple readers.
- **Запись** (`UpdateConfig`, `SetPassword`, `Load`, `Save`): `std::unique_lock` — exclusive writer.
- **Почему shared_mutex**: конфигурация читается **часто** (каждый пакет в CaptureLoop проверяет `proxy.enabled`, каждый CONNECT читает `proxy.host`/`port`), но пишется **редко** (только когда GUI меняет настройки).

### 2.4. Шифрование пароля (DPAPI)

**Алгоритм:**

```
Шифрование (SetPassword):
  1. plainPassword (std::wstring) → DATA_BLOB pbIn = { (BYTE*)plainPassword.data(), size }
  2. CryptProtectData(&pbIn, L"TcpRedirectorProxyPassword", NULL, NULL, NULL, CRYPTPROTECT_LOCAL_MACHINE, &pbOut)
  3. encryptedPassword = Base64Encode(pbOut.pbData, pbOut.cbData)
  4. LocalFree(pbOut.pbData)

Дешифрование (GetPlainPassword):
  1. encryptedPassword (Base64) → decode → DATA_BLOB pbIn
  2. CryptUnprotectData(&pbIn, NULL, NULL, NULL, NULL, 0, &pbOut)
  3. plainPassword = std::wstring((wchar_t*)pbOut.pbData, pbOut.cbData / sizeof(wchar_t))
  4. LocalFree(pbOut.pbData)
```

- **Флаг `CRYPTPROTECT_LOCAL_MACHINE`**: любой пользователь на той же машине может расшифровать.
  Это правильно для сервиса, который работает под `SYSTEM`, а GUI под пользователем.
- **Base64** — уже есть в `ProxyEngine.h:229-255` (функция `Base64Encode`), можно переиспользовать.
- **Пароль хранится в JSON только в зашифрованном виде.**
  В памяти (`m_plainPassword`) — только при явном запросе через `GetPlainPassword()`.

---

## 3. Замена существующего хардкода

### 3.1. WinDivertCapture.h — замена m_targetProcessPath

**Было (хардкод):**
```cpp
std::wstring m_targetProcessPath = L"C:\\Projects\\china\\police_sec\\TransfersClient.exe";
```

**Стало (из конфига):**
```cpp
// В WinDivertCapture::Open() или новом методе Init():
auto cfg = m_configManager->GetConfig();
m_targetProcessPath = cfg.app.exePath;
m_targetProcessName = cfg.app.exeName;
```

- `m_targetProcessPath` остаётся для `IsTargetProcess()` (сравнение полного пути).
- Добавляется `m_targetProcessName` для `FindTargetPid()` (поиск по имени процесса через `Process32FirstW` + `_wcsicmp(name, exeName)`).

### 3.2. ServiceMain.h — замена hardcoded proxy config

**Было (хардкод в Initialize()):**
```cpp
domain::ProxyConfig hardcoded;
hardcoded.host = L"127.0.0.1";
hardcoded.port = 8888;
m_configManager->SetProxyConfig(hardcoded);
```

**Стало (из конфига):**
```cpp
auto cfg = m_configManager->GetConfig();
domain::ProxyConfig proxyCfg;
proxyCfg.host = std::wstring(cfg.proxy.host.begin(), cfg.proxy.host.end());
proxyCfg.port = cfg.proxy.port;
proxyCfg.auth_required = cfg.auth.enabled;
if (cfg.auth.enabled) {
    proxyCfg.login = std::wstring(cfg.auth.username.begin(), cfg.auth.username.end());
    proxyCfg.has_password = !cfg.auth.encryptedPassword.empty();
}
m_configManager->SetProxyConfig(proxyCfg);
```

### 3.3. ServiceMain.h — замена hardcoded rules

**Было (хардкод правила):**
```cpp
domain::Rule transfersRule;
transfersRule.pattern = L"packet_generator.exe";
transfersRule.type = domain::RuleType::ProcessName;
```

**Стало (из конфига):**
```cpp
auto cfg = m_configManager->GetConfig();
domain::Rule rule;
rule.pattern = cfg.app.exeName;  // "TransfersClient.exe"
rule.type = domain::RuleType::ProcessName;
rule.action = cfg.proxy.enabled 
    ? domain::RuleAction::Proxy 
    : domain::RuleAction::Bypass;
```

### 3.4. CaptureLoop — проверка proxy.enabled

**В CaptureLoop** ([`WinDivertCapture.cpp:219`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/capture/WinDivertCapture.cpp:219)):

```cpp
if (isTargetSyn) {
    if (m_configManager->GetConfig().proxy.enabled) {
        // создать RedirectEvent → будет редирект на прокси
    } else {
        // НЕ создавать RedirectEvent — просто пропустить пакет
        // (режим мониторинга: логируем, но не блокируем)
    }
}
```

---

## 4. Схема взаимодействия

```
                    ┌──────────────────────┐
                    │    config.json        │
                    │  (%ProgramData%/.../) │
                    └──────┬───────────────┘
                           │ Load() / Save()
                           ▼
              ┌────────────────────────┐
              │     ConfigManager      │
              │  (shared_mutex защита) │
              └──┬──────┬──────┬──────┘
                 │      │      │
        GetConfig()│      │      │GetPlainPassword()
                 ▼      ▼      ▼
         ┌────────┐ ┌────────┐ ┌──────────┐
         │WinDivert│ │Proxy   │ │IpcHandler│
         │Capture  │ │Engine  │ │(GUI IPC) │
         └────────┘ └────────┘ └──────────┘
              │           │           │
              │    ConnectToProxy():  │
              │    host, port, auth   │
              ▼           ▼           ▼
         CaptureLoop()  CONNECT   get_config/
         фильтр по      запрос    set_config
         exeName                 (GUI)
```

---

## 5. Миграция: что меняется в каждом файле

| Файл | Что меняется |
|------|-------------|
| **НОВЫЙ: `infrastructure/config/ConfigManager.h`** | Весь класс ConfigManager + структура Config |
| **НОВЫЙ: `infrastructure/config/ConfigManager.cpp`** | Load/Save/DPAPI/потокобезопасность |
| `WinDivertCapture.h` | Убрать `m_targetProcessPath` hardcoded, добавить `m_configManager` pointer |
| `WinDivertCapture.cpp` | `Open()` читает exePath из конфига; `CaptureLoop()` проверяет `proxy.enabled` |
| `ServiceMain.h` | Убрать 3 блока hardcoded (прокси, правило, путь); читать из ConfigManager |
| `ProxyEngine.h` | `ConnectToProxy()` использует `auth.enabled` + `GetPlainPassword()` |
| `IpcHandler.h` | `GetConfig()`/`SetConfig()` работают через ConfigManager, а не ConfigManager->GetProxyConfig() |

---

## 6. Резюме

- **Config** — плоская структура с 5 секциями (app, proxy, auth, log, stats).
- **ConfigManager** — потокобезопасный класс с `shared_mutex`, загружает/сохраняет JSON.
- **DPAPI** — `CryptProtectData` с `CRYPTOPROTECT_LOCAL_MACHINE` для кросс-аккаунт доступа.
- **Миграция** — 3 hardcoded блока в ServiceMain.h + 1 в WinDivertCapture.h заменяются на вызовы `GetConfig()`.
- **Новые файлы**: `ConfigManager.h`, `ConfigManager.cpp`.
- **Изменяемые файлы**: `WinDivertCapture.h/.cpp`, `ServiceMain.h`, `ProxyEngine.h`, `IpcHandler.h`.
- **НЕ ТРОГАЕМ**: `CaptureLoop()`, `BridgeLoop()`, `FindPidBySourcePort()`, `IsTargetProcess()`.