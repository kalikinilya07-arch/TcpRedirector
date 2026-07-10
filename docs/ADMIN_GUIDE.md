# Руководство администратора — TcpRedirector

**Версия:** 1.0.0  
**Дата:** 10.07.2026  
**Ветка:** `fix/remediation-phase1-4`

---

## Содержание

1. [Обзор системы](#1-обзор-системы)
2. [Требования](#2-требования)
3. [Установка](#3-установка)
4. [Конфигурация](#4-конфигурация)
5. [Настройка архивирования логов](#5-настройка-архивирования-логов)
6. [Аутентификация на прокси](#6-аутентификация-на-прокси)
7. [Проверка работоспособности](#7-проверка-работоспособности)
8. [Управление сервисом](#8-управление-сервисом)
9. [Диагностика и логи](#9-диагностика-и-логи)
10. [Безопасность](#10-безопасность)
11. [Устранение неполадок](#11-устранение-неполадок)
12. [Обновление](#12-обновление)

---

## 1. Обзор системы

TcpRedirector — сервис прозрачного перехвата и перенаправления TCP-трафика через HTTP CONNECT прокси. Работает на уровне L3 (Network Layer) через драйвер WinDivert.

### Как это работает

```
Целевое приложение ──SYN──► TcpRedirector (WinDivert) ──DST modified──► TcpRelayServer:34010
                                                                              │
                                                                     HTTP CONNECT
                                                                              │
                                                                        Upstream Proxy:3128
                                                                              │
                                                                        Target Server
```

Трафик перехватывается **прозрачно** — приложение не нужно настраивать на прокси. Достаточно указать путь к `.exe` в конфигурации.

### Компоненты

| Компонент | Расположение | Назначение |
|-----------|-------------|------------|
| `TcpRedirectorService.exe` | `%ProgramData%\TcpRedirector\` | Windows-сервис (основной процесс) |
| `TcpRedirectorGUI.exe` | `%ProgramData%\TcpRedirector\gui\` | Графический интерфейс управления |
| `WinDivert.dll` | `%ProgramData%\TcpRedirector\` | Библиотека перехвата пакетов |
| `WinDivert64.sys` | `%ProgramData%\TcpRedirector\` | Драйвер ядра NDIS |
| `config.json` | `%ProgramData%\TcpRedirector\` | Конфигурация |
| `logs\` | `%ProgramData%\TcpRedirector\logs\` | Файлы логов |

### Порты

| Порт | Протокол | Назначение | Направление |
|------|----------|-----------|-------------|
| 34010 | TCP | TcpRelayServer — приём перенаправленных соединений | localhost |
| 34011 | TCP | **УДАЛЁН** (бывший IPC) | — |
| Named Pipe | IPC | `\\.\pipe\TcpRedirectorService` — связь GUI ↔ сервис | local |

### Учётные записи и права

- **Сервис**: `SYSTEM` (устанавливается через SCM)
- **GUI**: запускается от **Администратора** (требуется для Named Pipe с ACL)
- **WinDivert**: требует прав Администратора / SYSTEM (драйвер ядра)

---

## 2. Требования

### Системные

| Компонент | Минимальные требования |
|-----------|----------------------|
| ОС | Windows 10/11 x64, Windows Server 2019/2022 |
| Память | ~130 MB (сервис) + ~70 MB (GUI) |
| Диск | ~100 MB (установка) + место под логи |
| .NET | **Не требуется** (GUI self-contained) |

### Права

| Действие | Требуемые права |
|----------|-----------------|
| Установка сервиса | Администратор |
| Запуск сервиса | Администратор / SYSTEM (авто) |
| Запуск GUI | Администратор |
| Редактирование config.json | Администратор |

### Upstream прокси

| Требование | Примечание |
|-----------|-----------|
| HTTP CONNECT (RFC 7231) | Обязательно |
| HTTP/1.1 `200 Connection Established` | Обязательно |
| Basic Authentication (RFC 7617) | Опционально |
| Negotiate/Kerberos (RFC 4559) | Опционально, Windows-домен |

---

## 3. Установка

### 3.1. Из релиза

Релиз находится в `releases\vX.Y.Z\`. Запуск **от Администратора**:

```batch
cd C:\ProgramData\TcpRedirector

:: 1. Копирование файлов
mkdir C:\ProgramData\TcpRedirector\logs
mkdir C:\ProgramData\TcpRedirector\gui

copy TcpRedirectorService.exe C:\ProgramData\TcpRedirector\
copy WinDivert.dll C:\ProgramData\TcpRedirector\
copy WinDivert64.sys C:\ProgramData\TcpRedirector\
xcopy /E /I gui\* C:\ProgramData\TcpRedirector\gui\

:: 2. Установка Windows-сервиса
C:\ProgramData\TcpRedirector\TcpRedirectorService.exe --install
```

Результат: сервис `TcpRedirectorService` появляется в `services.msc`.

### 3.2. Регистрация драйвера WinDivert

Драйвер регистрируется автоматически при первом запуске сервиса через `EnsureDriverRunning()`. При необходимости — ручная регистрация:

```batch
sc create WinDivert type= kernel binPath= "C:\ProgramData\TcpRedirector\WinDivert64.sys"
sc start WinDivert
```

### 3.3. Запуск

> ⚠️ **Критически важно:** GUI требует запущенный сервис. Если запустить GUI без сервиса — он покажет "Disconnected" и статистика не будет отображаться. Named Pipe создаётся сервисом, GUI к нему подключается.

```batch
:: Способ 1: Быстрый запуск (из директории проекта)
run.bat                          ← запускает сервис + GUI (от Администратора!)

:: Способ 2: Windows-сервис + GUI
sc start TcpRedirectorService    ← сервис (SYSTEM)
C:\ProgramData\TcpRedirector\gui\TcpRedirectorGUI.exe   ← GUI (Администратор)

:: Способ 3: Консольный режим (для отладки)
start /MIN C:\ProgramData\TcpRedirector\TcpRedirectorService.exe --console
start C:\ProgramData\TcpRedirector\gui\TcpRedirectorGUI.exe
```

### 3.4. Удаление

```batch
:: Остановка и удаление сервиса
sc stop TcpRedirectorService
C:\ProgramData\TcpRedirector\TcpRedirectorService.exe --uninstall

:: Удаление драйвера
sc stop WinDivert
sc delete WinDivert

:: Удаление файлов
rmdir /S /Q C:\ProgramData\TcpRedirector
```

---

## 4. Конфигурация

Файл: `%ProgramData%\TcpRedirector\config.json`

### 4.1. Полный пример

```json
{
  "app": {
    "exePath": "C:\\Program Files\\MyApp\\MyApp.exe"
  },
  "proxy": {
    "host": "192.168.1.100",
    "port": 3128,
    "enabled": true
  },
  "auth": {
    "enabled": true,
    "username": "proxyuser",
    "encryptedPassword": "<DPAPI-Base64>",
    "kerberos": false
  },
  "log": {
    "level": 2,
    "fileEnabled": true,
    "maxSizeMB": 10
  },
  "log_rotation": {
    "enabled": true,
    "schedule": "daily",
    "hour": 3,
    "minute": 0,
    "max_age_days": 30,
    "archive_dir": "C:\\ProgramData\\TcpRedirector\\logs\\archive",
    "compress": true
  },
  "stats": {
    "updateIntervalMs": 2000
  },
  "rules": [
    {
      "id": "rule-1",
      "pattern": "MyApp.exe",
      "description": "Redirect MyApp traffic",
      "priority": 1,
      "enabled": true,
      "type": "process_name",
      "action": "proxy"
    }
  ]
}
```

### 4.2. Секция `app` — целевое приложение

| Параметр | Тип | Описание |
|----------|-----|----------|
| `exePath` | string | Полный путь к `.exe`-файлу целевого приложения |

### 4.3. Секция `proxy` — upstream прокси

| Параметр | Тип | По умолчанию | Описание |
|----------|-----|-------------|----------|
| `host` | string | `"127.0.0.1"` | Адрес HTTP-прокси |
| `port` | int | `3128` | Порт HTTP-прокси |
| `enabled` | bool | `true` | Включить проксирование |

### 4.4. Секция `auth` — аутентификация

| Параметр | Тип | По умолчанию | Описание |
|----------|-----|-------------|----------|
| `enabled` | bool | `false` | Требовать аутентификацию на прокси |
| `username` | string | `""` | Логин для Basic Auth |
| `encryptedPassword` | string | `""` | Пароль (DPAPI + Base64, задаётся через GUI) |
| `kerberos` | bool | `false` | Использовать Kerberos/Negotiate (взаимоисключает Basic) |

**Важно:** `kerberos = true` и `enabled = true` работают вместе. При включении Kerberos GUI автоматически включает `auth.enabled`. При выключении `auth.enabled` GUI автоматически выключает Kerberos.

### 4.5. Секция `log` — логирование

| Параметр | Тип | По умолчанию | Описание |
|----------|-----|-------------|----------|
| `level` | int | `2` | 0=TRACE, 1=DEBUG, 2=INFO, 3=WARN, 4=ERROR |
| `fileEnabled` | bool | `true` | Писать лог в файл |
| `maxSizeMB` | int | `10` | Макс. размер до мгновенной ротации (`.1`, `.2`, ... до 5) |

### 4.6. Секция `log_rotation` — ротация по расписанию

| Параметр | Тип | По умолчанию | Описание |
|----------|-----|-------------|----------|
| `enabled` | bool | `true` | Включить ротацию по расписанию |
| `schedule` | string | `"daily"` | `"hourly"` — каждый час, `"daily"` — раз в сутки |
| `hour` | int | `3` | Час ротации (0-23, для `"daily"`) |
| `minute` | int | `0` | Минута ротации (0-59) |
| `max_age_days` | int | `30` | Срок хранения архивов (дней) |
| `archive_dir` | string | `logs\archive` | Директория для архивов (полный путь) |
| `compress` | bool | `true` | Сжимать старые логи в `.zip` |

**Эта секция НЕ отображается в GUI.** Редактируется только вручную в `config.json`.

### 4.7. Секция `stats` — статистика

| Параметр | Тип | По умолчанию | Описание |
|----------|-----|-------------|----------|
| `updateIntervalMs` | int | `2000` | Интервал обновления статистики в GUI (мс) |

### 4.8. Секция `rules` — правила фильтрации

| Параметр | Тип | Описание |
|----------|-----|----------|
| `id` | string | Уникальный идентификатор правила |
| `pattern` | string | Шаблон: имя процесса (`chrome.exe`), путь (`C:\...\app.exe`) |
| `description` | string | Человекочитаемое описание |
| `priority` | int | Приоритет (меньше = выше) |
| `enabled` | bool | Правило активно |
| `type` | string | `"process_name"`, `"process_path"`, `"global"` |
| `action` | string | `"proxy"` — через прокси, `"direct"` — напрямую, `"block"` — заблокировать |

### 4.9. Применение изменений

- **Через GUI:** изменения в Settings → Save применяются немедленно
- **Через config.json:** требуется перезапуск сервиса:
  ```batch
  sc stop TcpRedirectorService
  sc start TcpRedirectorService
  ```

---

## 5. Настройка архивирования логов

### 5.1. Два уровня ротации

TcpRedirector использует **двухуровневую систему ротации**:

| Уровень | Триггер | Механизм | Файлы |
|--------|---------|----------|-------|
| **Размерная** | `maxSizeMB` превышен | Мгновенно: переименование `.1` → `.2` → ... → `.5` | `tcp_redirector.1.log` ... `.5.log` |
| **По расписанию** | Шедулер (раз в 60 сек) | Планово: закрытие → переименование с timestamp → сжатие в `.zip` | `tcp_redirector_2026-07-10_0300.log` → `.zip` |

### 5.2. Как это работает

```
1. Размер файла > maxSizeMB (10 MB):
   tcp_redirector.4.log → удаляется
   tcp_redirector.3.log → tcp_redirector.4.log
   ...
   tcp_redirector.log    → tcp_redirector.1.log
   Создаётся новый tcp_redirector.log

2. По расписанию (ежедневно в 03:00):
   tcp_redirector.log закрывается
   → переименовывается в tcp_redirector_2026-07-10_0300.log
   → сжимается в logs_2026-07-10_0300.zip (PowerShell Compress-Archive)
   → перемещается в archive_dir
   → архивы старше 30 дней удаляются
   → открывается новый tcp_redirector.log
```

### 5.3. Примеры конфигурации

#### Ежедневная ротация в 03:00 со сжатием

```json
"log_rotation": {
  "enabled": true,
  "schedule": "daily",
  "hour": 3,
  "minute": 0,
  "max_age_days": 30,
  "archive_dir": "C:\\ProgramData\\TcpRedirector\\logs\\archive",
  "compress": true
}
```

#### Ежечасная ротация без сжатия

```json
"log_rotation": {
  "enabled": true,
  "schedule": "hourly",
  "minute": 0,
  "max_age_days": 7,
  "archive_dir": "C:\\ProgramData\\TcpRedirector\\logs\\archive",
  "compress": false
}
```

#### Только размерная ротация (без scheduled)

```json
"log_rotation": {
  "enabled": false,
  "schedule": "daily",
  "hour": 3,
  "minute": 0,
  "max_age_days": 30,
  "archive_dir": "C:\\ProgramData\\TcpRedirector\\logs\\archive",
  "compress": true
}
```

При `"enabled": false` поток `LogRotator` не запускается, работает только размерная ротация.

#### Хранение архивов 90 дней с ежедневной ротацией в полночь

```json
"log_rotation": {
  "enabled": true,
  "schedule": "daily",
  "hour": 0,
  "minute": 0,
  "max_age_days": 90,
  "archive_dir": "D:\\Logs\\TcpRedirector",
  "compress": true
}
```

### 5.4. Очистка старых архивов

Очистка запускается при **каждой** плановой ротации:

1. Сканируются `archive_dir` и `log_dir`
2. Файлы `.zip` и `.log` (кроме текущего `tcp_redirector.log`) с датой изменения старше `max_age_days` — удаляются
3. Текущий лог-файл никогда не удаляется

### 5.5. Ручной запуск архивирования

Плановую ротацию можно форсировать через PowerShell (от Администратора):

```powershell
# Установить minute = текущая минута + 1, дождаться срабатывания
# Или перезапустить сервис — ротатор запустится с чистым last_rotate
sc stop TcpRedirectorService
sc start TcpRedirectorService
```

---

## 6. Аутентификация на прокси

### 6.1. Basic Authentication

**Настройка через GUI:**
1. Settings → Proxy → Auth Required: **включить**
2. Username: ввести логин
3. Password: ввести пароль
4. Сохранить

Пароль шифруется через **DPAPI** и хранится в `config.json` как `auth.encryptedPassword` (Base64). В открытом виде нигде не сохраняется.

### 6.2. Kerberos/Negotiate (Windows-домен)

**Требования:**
- Машина в домене Active Directory
- Сервис запущен от `SYSTEM` (машинный аккаунт) или доменного пользователя
- Прокси поддерживает `Negotiate` (SPNEGO)

**Настройка через GUI:**
1. Settings → Proxy → Use Kerberos: **включить**
2. Auth Required включится автоматически
3. Сохранить

**SPN прокси** формируется автоматически: `HTTP/<proxy_host>`.

**Ограничение:** максимум 3 раунда SSPI-negotiation.

### 6.3. Проверка Kerberos

```powershell
# Проверить наличие Kerberos-тикетов
klist

# Проверить доступность SPN прокси
setspn -Q HTTP/proxyhost.domain.local
```

---

## 7. Проверка работоспособности

### 7.1. Проверка сервиса

```batch
:: Статус сервиса
sc query TcpRedirectorService

:: Должен быть: STATE: 4 RUNNING
```

### 7.2. Проверка драйвера WinDivert

```batch
:: Статус драйвера
sc query WinDivert

:: Должен быть: STATE: 4 RUNNING
```

### 7.3. Проверка через GUI

1. Запустить `gui\TcpRedirectorGUI.exe` от Администратора
2. Нажать **Connect** — статус: "Connected"
3. Вкладка **Stats**: отображается `Active Connections`, `Total Rx/Tx`
4. Вкладка **Connections**: список активных соединений

### 7.4. Проверка перенаправления

1. Запустить целевое приложение (указанное в `app.exePath`)
2. Выполнить действие, инициирующее TCP-соединение (открыть сайт)
3. В GUI → Stats: `Active Connections` > 0
4. В логах (`%ProgramData%\TcpRedirector\logs\tcp_redirector.log`):
   ```
   [INFO ] [relay       ] CONNECT <host>:<port> → 200 Established
   ```

### 7.5. Проверка Named Pipe (IPC)

```batch
:: Проверить доступность пайпа
powershell -Command "Test-Path \\.\pipe\TcpRedirectorService"

:: Должен вернуть: True
```

---

## 8. Управление сервисом

### 8.1. Команды SCM

```batch
sc start   TcpRedirectorService    :: Запуск
sc stop    TcpRedirectorService    :: Остановка
sc query   TcpRedirectorService    :: Статус
sc config  TcpRedirectorService start= auto  :: Автозапуск
```

### 8.2. Команды сервиса

```batch
TcpRedirectorService.exe --install     :: Установить сервис
TcpRedirectorService.exe --uninstall   :: Удалить сервис
TcpRedirectorService.exe --console     :: Консольный режим (Ctrl+C для остановки)
```

### 8.3. Управление драйвером WinDivert

```batch
sc start  WinDivert    :: Запуск драйвера
sc stop   WinDivert    :: Остановка драйвера
sc query  WinDivert    :: Статус драйвера
```

Обычно ручное управление драйвером не требуется — сервис запускает его автоматически.

---

## 9. Диагностика и логи

### 9.1. Расположение логов

```
%ProgramData%\TcpRedirector\logs\
├── tcp_redirector.log              ← текущий лог
├── tcp_redirector.1.log            ← размерная ротация
├── tcp_redirector.2.log
├── ...
├── tcp_redirector_2026-07-10_0300.log  ← временная ротация
└── archive\
    └── logs_2026-07-10_0300.zip        ← архивы
```

### 9.2. Уровни логирования

| Уровень | Значение | Когда использовать |
|---------|----------|-------------------|
| TRACE | 0 | Максимальная детализация: каждый пакет `[PROXIED]`/`[MISSED]`/`[DIRECT]` |
| DEBUG | 1 | Отладка проблем соединения |
| INFO | 2 | Нормальная работа (запуск, остановка, CONNECT) |
| WARN | 3 | Предупреждения |
| ERROR | 4 | Только ошибки |

**Изменение уровня** — в GUI: Settings → Log Level, или в `config.json` → `log.level`.

### 9.3. Формат записи

```
[2026-07-10 14:30:15.123] [INFO ] [relay       ] CONNECT google.com:443 → 200 Established
[2026-07-10 14:30:15.456] [DEBUG] [capture     ] [PROXIED] chrome.exe:49823 → 142.250.185.142:443 (120 bytes)
[2026-07-10 14:30:20.789] [WARN ] [windivert   ] PID lookup failed for port 50123
```

### 9.4. Потоки в логах

| Logger | Источник |
|--------|----------|
| `service` | ServiceMain — жизненный цикл сервиса |
| `relay` | TcpRelayServer — CONNECT, bridge |
| `capture` | WinDivertCapture — перехват пакетов |
| `windivert` | WinDivert API — ошибки драйвера |
| `pipe` | PipeServer — IPC (GUI ↔ сервис) |
| `config` | ConfigManager — загрузка/сохранение конфига |
| `rotator` | LogRotator — ротация логов |

### 9.5. Диагностический лог GUI

При проблемах подключения GUI пишет в `%ProgramData%\TcpRedirector\gui\ipc_diag.log`:

```
14:30:15.123 ConnectAsync: connecting to pipe TcpRedirectorService...
14:30:15.234 ConnectAsync: connected OK
```

---

## 10. Безопасность

### 10.1. Модель угроз

| Актив | Защита |
|-------|--------|
| Канал GUI ↔ Сервис | Named Pipe `\\.\pipe\TcpRedirectorService` + ACL: `D:(A;;GA;;;BA)(A;;GA;;;SY)` |
| Пароль прокси | DPAPI (`CryptProtectData`) + Base64 в config.json |
| Ключи Kerberos | SSPI (не сохраняются) |
| Буфер IPC | Лимит 1 MB → защита от OOM |
| Размерная ротация | Мгновенная, предотвращает переполнение диска |
| Архивы логов | Автоочистка старше `max_age_days` |

### 10.2. Кто может подключиться к сервису

Только:
- **SYSTEM** (сам сервис)
- **BUILTIN\Administrators** (пользователь, запускающий GUI)

Обычные пользователи **не могут** подключиться к Named Pipe.

### 10.3. Хранение пароля

Пароль шифруется Windows DPAPI с флагом `CRYPTPROTECT_UI_FORBIDDEN`:
- Привязан к учётной записи, выполнившей шифрование
- Не может быть расшифрован на другой машине
- Не может быть расшифрован другим пользователем (если сервис и GUI от разных пользователей — требуется доменный контекст)

---

## 11. Устранение неполадок

### 11.1. Сервис не запускается

| Симптом | Причина | Решение |
|---------|---------|---------|
| `sc start` → не запускается | WinDivert не загружен | `sc query WinDivert` → `sc start WinDivert` |
| `sc start` → ошибка 1053 | Сервис упал при инициализации | Запустить `--console`, смотреть логи |
| GUI: статус "Disconnected" | Named Pipe не создан | Запустить GUI от Администратора |

### 11.2. Трафик не перенаправляется

| Симптом | Причина | Решение |
|---------|---------|---------|
| GUI: Active Connections = 0 | Правила не совпадают | Проверить `app.exePath` и `rules` в config.json |
| `[MISSED]` в логах | Пакет от процесса не из правил | Добавить правило для процесса |
| `[DIRECT]` в логах | Правило с `action: "direct"` | Изменить `action` на `"proxy"` |

### 11.3. Ошибка подключения к прокси

| Симптом | Причина | Решение |
|---------|---------|---------|
| `CONNECT ... → 407` | Требуется аутентификация | Включить `auth.enabled`, указать логин/пароль или Kerberos |
| `CONNECT ... → timeout` | Прокси недоступен | Проверить `proxy.host` и `proxy.port` |
| `WinDivert err=2` | Драйвер не найден | `sc create WinDivert ...` → `sc start WinDivert` |

### 11.4. Проблемы с Named Pipe (IPC)

| Симптом | Причина | Решение |
|---------|---------|---------|
| GUI: "Access Denied" | Запущен не от Администратора | Запустить GUI от Администратора |
| GUI: "Pipe not found" | Сервис не запущен | `sc start TcpRedirectorService` |
| GUI: таймаут подключения (5с) | Сервис запущен, но пайп занят | Перезапустить сервис |

### 11.5. Проблемы с логами

| Симптом | Причина | Решение |
|---------|---------|---------|
| Лог не пишется | Нет прав на `%ProgramData%\TcpRedirector\logs` | Создать папку от Администратора |
| Лог растёт бесконечно | `log_rotation.enabled = false` | Включить ротацию в config.json |
| Архивы не создаются | PowerShell заблокирован | Проверить: `powershell Get-ExecutionPolicy` |

---

## 12. Обновление

### 12.1. Процедура обновления

```batch
:: 1. Остановить сервис
sc stop TcpRedirectorService

:: 2. Сделать резервную копию конфига
copy C:\ProgramData\TcpRedirector\config.json C:\backup\config.json.bak

:: 3. Заменить файлы из нового релиза
copy /Y releases\vX.Y.Z\TcpRedirectorService.exe C:\ProgramData\TcpRedirector\
copy /Y releases\vX.Y.Z\WinDivert.dll C:\ProgramData\TcpRedirector\
copy /Y releases\vX.Y.Z\WinDivert64.sys C:\ProgramData\TcpRedirector\
xcopy /E /Y releases\vX.Y.Z\gui\* C:\ProgramData\TcpRedirector\gui\

:: 4. Проверить config.json на новые поля (см. пример в разделе 4.1)

:: 5. Запустить сервис
sc start TcpRedirectorService

:: 6. Проверить статус
sc query TcpRedirectorService
```

### 12.2. Откат

```batch
sc stop TcpRedirectorService
copy /Y C:\backup\config.json.bak C:\ProgramData\TcpRedirector\config.json
:: Восстановить файлы предыдущей версии
sc start TcpRedirectorService
```

---

## Приложение А: Быстрый старт (checklist)

- [ ] `WinDivert64.sys` и `WinDivert.dll` в `C:\ProgramData\TcpRedirector\`
- [ ] `TcpRedirectorService.exe --install` от Администратора
- [ ] `sc start WinDivert`
- [ ] `sc start TcpRedirectorService`
- [ ] `config.json`: указан `app.exePath`, `proxy.host`, `proxy.port`
- [ ] `sc query TcpRedirectorService` → STATE: RUNNING
- [ ] GUI запущен от Администратора → Connected
- [ ] Целевое приложение запущено → трафик виден в Stats

## Приложение Б: Полезные команды PowerShell

```powershell
# Статус сервиса и драйвера
Get-Service TcpRedirectorService, WinDivert | Format-Table Name, Status

# Последние 20 строк лога
Get-Content "C:\ProgramData\TcpRedirector\logs\tcp_redirector.log" -Tail 20

# Размер логов
Get-ChildItem "C:\ProgramData\TcpRedirector\logs" -Recurse |
    Measure-Object -Property Length -Sum |
    Select-Object Count, @{N="TotalMB";E={[math]::Round($_.Sum/1MB, 2)}}

# Архивы старше 30 дней
Get-ChildItem "C:\ProgramData\TcpRedirector\logs\archive\*.zip" |
    Where-Object { $_.LastWriteTime -lt (Get-Date).AddDays(-30) } |
    Select-Object Name, LastWriteTime
```
