# Конфигурация TcpRedirector

## 1. Структура config.json

Файл: `%ProgramData%\TcpRedirector\config.json`

```json
{
  "app": {
    "exePath": "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe"
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
  },
  "rules": [
    {
      "id": "uuid",
      "type": 1,
      "action": 1,
      "pattern": "chrome.exe",
      "description": "Google Chrome",
      "priority": 0,
      "enabled": true
    }
  ]
}
```

### 1.1. `app` — целевое приложение

| Поле | Тип | Описание |
|------|-----|----------|
| `exePath` | string | Полный путь к EXE-файлу целевого процесса. Используется в `WinDivertCapture::SetTargetProcess()` и `RuleEngine`. |

`exeName` извлекается автоматически как последний компонент пути (геттер `Config::GetExeName()`).

### 1.2. `proxy` — upstream прокси

| Поле | Тип | По умолчанию | Описание |
|------|-----|-------------|----------|
| `host` | string | `127.0.0.1` | Адрес HTTP-прокси |
| `port` | uint16 | `3128` | Порт HTTP-прокси |
| `enabled` | bool | `true` | Перенаправлять ли трафик на прокси |

### 1.3. `auth` — аутентификация на прокси

| Поле | Тип | По умолчанию | Описание |
|------|-----|-------------|----------|
| `enabled` | bool | `false` | Требуется ли аутентификация |
| `username` | string | `""` | Логин для Basic Auth |
| `encryptedPassword` | string | `""` | Пароль, зашифрованный DPAPI (Base64) |
| `kerberos` | bool | `false` | Использовать Negotiate/Kerberos вместо Basic |

**Kerberos и Basic — взаимоисключающие.** При включении Kerberos GUI авто-включает `enabled`. При выключении `enabled` GUI авто-выключает Kerberos.

### 1.4. `log` — логирование

| Поле | Тип | По умолчанию | Описание |
|------|-----|-------------|----------|
| `level` | int | `2` | 0=TRACE, 1=DEBUG, 2=INFO, 3=WARN, 4=ERROR |
| `fileEnabled` | bool | `true` | Писать лог в файл |
| `maxSizeMB` | int | `10` | Максимальный размер до ротации |

### 1.5. `rules` — правила фильтрации

| Поле | Тип | Описание |
|------|-----|----------|
| `id` | string | UUID правила |
| `type` | int | 0=ProcessName, 1=ProcessPath, 2=Global |
| `action` | int | 0=Direct, 1=Proxy, 2=Block |
| `pattern` | string | Шаблон (имя или путь, поддерживает `*`) |
| `priority` | int | Меньше = выше приоритет |
| `enabled` | bool | Активно ли правило |

---

## 2. ConfigManager

Файл: [`ConfigManager.cpp`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp)

### Потокобезопасность

- `std::shared_mutex` — multiple readers, exclusive writer
- `GetConfig()` — shared_lock (вызывается часто из CaptureLoop)
- `UpdateConfig()` — unique_lock (вызывается редко при сохранении из GUI)

### DPAPI шифрование

Пароль хранится в `encryptedPassword` в формате Base64(DATA_BLOB). Используется `CryptProtectData` с флагом `CRYPTPROTECT_UI_FORBIDDEN` и энтропией `"TcpRedirectorProxyPassword"`. Расшифрованный пароль хранится только в памяти (`plain_password`), никогда не записывается в JSON.

### Атомарное сохранение

При сохранении конфигурации используется схема write-to-temp → rename для предотвращения повреждения файла при сбое.

---

## 3. GUI ↔ Service синхронизация

GUI пишет config.json напрямую (через `JsonConfigRepository`) и опционально отправляет изменения в сервис через IPC (`set_config`). Сервис читает config.json при старте.

**GUI — единственный writer config.json. Сервис — только reader.**