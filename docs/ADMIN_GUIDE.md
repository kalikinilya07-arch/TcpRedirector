# Руководство администратора — TcpRedirector

**Версия:** 1.0.0  
**Дата:** 13.07.2026  
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
| `WinDivert64.sys` | `C:\Windows\System32\drivers\` | Драйвер ядра (устанавливается как служба) |
| `config.json` | `%ProgramData%\TcpRedirector\` | Конфигурация |
| `logs\` | `%ProgramData%\TcpRedirector\logs\` | Файлы логов |

### Порты

| Порт | Протокол | Назначение | Направление |
|------|----------|-----------|-------------|
| 34010 | TCP | TcpRelayServer — приём перенаправленных соединений | localhost |
| Named Pipe | IPC | `\\.\pipe\TcpRedirectorService` — связь GUI ↔ сервис | local |

### Учётные записи и права

| Действие | Кто выполняет | Требуемые права |
|----------|--------------|-----------------|
| **Установка** (1 раз) | Администратор | Администратор |
| **Ежедневный запуск** GUI | Пользователь | **Не требуются** |
| Сервис TcpRedirector | SYSTEM (SCM) | SYSTEM |
| Драйвер WinDivert | Служба WinDivert (auto-start) | SYSTEM |

**Важно:** После первичной установки администратором, пользователь запускает GUI без прав администратора. WinDivert работает как Windows-служба с автостартом.

---

## 2. Требования

### Системные

| Компонент | Минимальные требования |
|-----------|----------------------|
| ОС | Windows 10/11 x64, Windows Server 2019/2022 |
| Память | ~130 MB (сервис) + ~70 MB (GUI) |
| Диск | ~100 MB (установка) + место под логи |
| .NET | **Не требуется** (GUI self-contained) |

### Upstream прокси

| Требование | Примечание |
|-----------|-----------|
| HTTP CONNECT (RFC 7231) | Обязательно |
| HTTP/1.1 `200 Connection Established` | Обязательно |
| Basic Authentication (RFC 7617) | Опционально |
| Negotiate/Kerberos (RFC 4559) | Опционально, Windows-домен |

---

## 3. Установка

Установка состоит из двух этапов:

1. **Администратор** (один раз): установка драйвера WinDivert + сервиса + настройка
2. **Пользователь** (ежедневно): запуск GUI

### 3.1. Подготовка: защита от антивируса

Windows Defender может удалить драйвер WinDivert. Добавьте исключение **перед** установкой:

```batch
:: Запустить от Администратора!
powershell -Command "Add-MpPreference -ExclusionPath 'C:\ProgramData\TcpRedirector'"
powershell -Command "Add-MpPreference -ExclusionPath 'C:\Windows\System32\drivers\WinDivert64.sys'"
```

### 3.2. Установка драйвера WinDivert (один раз, администратор)

WinDivert работает как Windows-служба с автостартом. Это гарантирует, что:
- Драйвер загружается при загрузке системы
- Пользователь не требует прав администратора для работы
- Антивирус не блокирует загрузку (если добавлено исключение)

```batch
:: Запустить от Администратора!

:: Вариант A: через WinDivertInstall.exe (если есть в дистрибутиве)
cd C:\ProgramData\TcpRedirector
WinDivertInstall.exe install

:: Вариант B: через sc (если WinDivertInstall.exe отсутствует)
sc create WinDivert type= kernel binPath= "C:\Windows\System32\drivers\WinDivert64.sys"
sc start WinDivert
```

Проверка:
```batch
sc query WinDivert
:: Должен показать STATE: RUNNING
```

### 3.3. Установка сервиса TcpRedirector (один раз, администратор)

```batch
:: Запустить от Администратора!

:: Копирование файлов
mkdir C:\ProgramData\TcpRedirector\logs
mkdir C:\ProgramData\TcpRedirector\gui

copy TcpRedirectorService.exe C:\ProgramData\TcpRedirector\
copy WinDivert.dll C:\ProgramData\TcpRedirector\
copy WinDivert64.sys C:\Windows\System32\drivers\
xcopy /E /I gui\* C:\ProgramData\TcpRedirector\gui\

:: Установка сервиса
C:\ProgramData\TcpRedirector\TcpRedirectorService.exe --install
```

Результат: сервис `TcpRedirectorService` появляется в `services.msc` с типом запуска `Automatic`.

### 3.4. Настройка конфигурации (администратор)

```batch
:: Создать config.json в C:\ProgramData\TcpRedirector\config.json
:: См. раздел 4. Конфигурация
```

### 3.5. Запуск сервиса (администратор)

```batch
:: Запустить от Администратора!
sc start TcpRedirectorService
```

### 3.6. Ежедневный запуск GUI (пользователь, без прав администратора)

После установки пользователь запускает GUI **без прав администратора**:

```batch
:: Запускается обычным пользователем (двойной клик)
C:\ProgramData\TcpRedirector\gui\TcpRedirectorGUI.exe
```

GUI подключается к сервису через Named Pipe. Сервис и драйвер уже работают.

### 3.7. Консольный режим (для отладки, администратор)

```batch
:: Запустить от Администратора!
C:\ProgramData\TcpRedirector\TcpRedirectorService.exe --console
```

### 3.8. Удаление

```batch
:: Запустить от Администратора!

:: Остановка и удаление сервиса
sc stop TcpRedirectorService
C:\ProgramData\TcpRedirector\TcpRedirectorService.exe --uninstall

:: Удаление драйвера WinDivert
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
# Принудительная ротация логов
Restart-Service TcpRedirectorService
```

---

## 6. Аутентификация на прокси

### 6.1. Basic Authentication

Логин задаётся в GUI, пароль шифруется через DPAPI (машина + пользователь SYSTEM).

```
GUI → DPAPI Encrypt → config.json (encryptedPassword)
SCM/service → DPAPI Decrypt → HTTP CONNECT Proxy-Authorization: Basic ...
```

### 6.2. Kerberos / Negotiate

Включается в GUI (Settings → Kerberos). Требования:

- Windows-домен
- Сервис TcpRedirector работает под `SYSTEM` (делегация доверена системе)
- SPN `HTTP/{proxy-fqdn}` зарегистрирован в AD для компьютера

---

## 7. Проверка работоспособности

### 7.1. После установки (администратор)

```batch
:: 1. Проверить драйвер WinDivert
sc query WinDivert
:: STATE: RUNNING

:: 2. Проверить сервис
sc query TcpRedirectorService
:: STATE: RUNNING

:: 3. Проверить Named Pipe
powershell -Command "Get-ChildItem \\.\pipe\ -ErrorAction SilentlyContinue | Where-Object Name -eq 'TcpRedirectorService'"
:: Должен быть в списке

:: 4. Проверить лог сервиса
type C:\ProgramData\TcpRedirector\logs\tcp_redirector.log
:: Должен содержать "Service is running"
```

### 7.2. Ежедневная проверка (пользователь)

1. Запустить `C:\ProgramData\TcpRedirector\gui\TcpRedirectorGUI.exe`
2. В правом нижнем углу должно быть: **Connected**
3. Открыть вкладку **Stats** — должна отображаться статистика трафика
4. Открыть вкладку **Connections** — должны отображаться активные соединения

---

## 8. Управление сервисом

### 8.1. Через GUI

Кнопки **Start** / **Stop** в главном окне GUI. GUI подключается к уже запущенному сервису через Named Pipe.

### 8.2. Через командную строку (администратор)

```batch
:: Запуск
sc start TcpRedirectorService

:: Остановка
sc stop TcpRedirectorService

:: Перезапуск
sc stop TcpRedirectorService && sc start TcpRedirectorService

:: Статус
sc query TcpRedirectorService
```

### 8.3. Через services.msc

Сервис `TcpRedirectorService` — тип запуска `Automatic`.

---

## 9. Диагностика и логи

### 9.1. Файлы логов

| Файл | Расположение | Назначение |
|------|-------------|------------|
| `tcp_redirector.log` | `%ProgramData%\TcpRedirector\logs\` | Основной лог сервиса |
| `gui_diag.log` | Рядом с `TcpRedirectorGUI.exe` | Диагностический лог GUI |
| `windivert_debug.log` | `%ProgramData%\TcpRedirector\logs\` | Лог WinDivert (сетевое ядро) |

### 9.2. Уровни логирования

| Уровень | Значение | Когда использовать |
|---------|----------|-------------------|
| 0 | TRACE | Полная трассировка всех пакетов |
| 1 | DEBUG | Отладка (рекомендуется для диагностики) |
| 2 | INFO | Нормальная работа |
| 3 | WARN | Только предупреждения |
| 4 | ERROR | Только ошибки |

### 9.3. Диагностический лог GUI

Включает детальные сообщения о каждом шаге запуска, подключения к Named Pipe и командах IPC. Полезен при проблемах с подключением GUI к сервису.

---

## 10. Безопасность

### 10.1. IPC (Named Pipe)

- Pipe name: `\\.\pipe\TcpRedirectorService`
- ACL: только `BUILTIN\Administrators` и `LOCAL_SYSTEM`
- Максимальный размер сообщения: 1 MB
- Формат: JSON

### 10.2. Пароль (DPAPI)

- Пароль шифруется через `CryptProtectData` с `CRYPTPROTECT_UI_FORBIDDEN`
- Ключ привязан к машине + учётной записи SYSTEM
- Расшифровать можно только на той же машине, под тем же пользователем

### 10.3. WinDivert

- Драйвер работает как Windows-служба с правами SYSTEM
- После установки через `WinDivertInstall.exe` или `sc create` — драйвер загружается при старте системы
- Не требует прав администратора у пользователя, запускающего GUI

---

## 11. Устранение неполадок

### 11.1. WinDivert: ошибка 5 (ERROR_ACCESS_DENIED)

**Причина:** Windows Defender удалил `WinDivert64.sys`, или драйвер не установлен.

**Решение (администратор):**

```batch
:: 1. Добавить исключение Defender
powershell -Command "Add-MpPreference -ExclusionPath 'C:\ProgramData\TcpRedirector'"
powershell -Command "Add-MpPreference -ExclusionPath 'C:\Windows\System32\drivers\WinDivert64.sys'"

:: 2. Переустановить драйвер
sc create WinDivert type= kernel binPath= "C:\Windows\System32\drivers\WinDivert64.sys"
sc start WinDivert

:: 3. Проверить
sc query WinDivert
```

### 11.2. WinDivert: ошибка 2 (ERROR_FILE_NOT_FOUND)

**Причина:** `WinDivert64.sys` удалён антивирусом. Требуется переустановка.

**Решение:** см. п. 11.1.

### 11.3. GUI показывает "Disconnected"

**Причина:** Сервис не запущен, или Named Pipe не создан.

**Проверка:**
```batch
sc query TcpRedirectorService
:: Если STOPPED: sc start TcpRedirectorService

:: Проверить Named Pipe
powershell -Command "Get-ChildItem \\.\pipe\ -ErrorAction SilentlyContinue | Where-Object Name -eq 'TcpRedirectorService'"
```

### 11.4. GUI показывает "Start failed"

**Причина:** Сервис не запускается, или GUI не может подключиться к Named Pipe.

**Диагностика:** Открыть `gui_diag.log` рядом с `TcpRedirectorGUI.exe` и найти последние записи с `[SCM]` или `[IPC]`.

### 11.5. GUI не отображает статистику

**Причина:** GUI подключён к сервису, но не получает push-уведомления о статистике.

**Проверка:**
1. Открыть вкладку Stats — если данные пустые, проверить `gui_diag.log` на `PollLoop: IPC error`
2. Перезапустить GUI

### 11.6. Лог-файлы не ротируются

**Проверка:**
```batch
:: Проверить секцию log_rotation в config.json
type C:\ProgramData\TcpRedirector\config.json | find "log_rotation"

:: Проверить, что enabled: true
:: Проверить права на запись в archive_dir
```

---

## 12. Обновление

### 12.1. Остановка сервиса

```batch
sc stop TcpRedirectorService
```

### 12.2. Замена файлов

```batch
:: Копировать новые версии
copy /Y TcpRedirectorService.exe C:\ProgramData\TcpRedirector\
copy /Y WinDivert.dll C:\ProgramData\TcpRedirector\
xcopy /E /Y gui\* C:\ProgramData\TcpRedirector\gui\
```

### 12.3. Запуск сервиса

```batch
sc start TcpRedirectorService
```

### 12.4. Обновление драйвера WinDivert

Если новая версия WinDivert:

```batch
:: Остановка
sc stop WinDivert
sc delete WinDivert

:: Копирование нового драйвера
copy /Y WinDivert64.sys C:\Windows\System32\drivers\

:: Установка
sc create WinDivert type= kernel binPath= "C:\Windows\System32\drivers\WinDivert64.sys"
sc start WinDivert
