# Инструкция по установке TcpRedirector на чистую Windows 10/11 x64

**Разработчик:** Kalikin Iliya

---

## Содержание

1. [Системные требования](#1-системные-требования)
2. [Подготовка к установке](#2-подготовка-к-установке)
3. [Установка сервиса](#3-установка-сервиса)
4. [Настройка конфигурации](#4-настройка-конфигурации)
5. [Запуск и проверка](#5-запуск-и-проверка)
6. [Управление сервисом](#6-управление-сервисом)
7. [Обновление](#7-обновление)
8. [Удаление](#8-удаление)
9. [Решение проблем](#9-решение-проблем)

---

## 1. Системные требования

### 1.1. Аппаратные требования

| Параметр | Минимальные | Рекомендуемые |
|----------|-------------|---------------|
| Процессор | x64, 2 ядра | x64, 4+ ядра |
| ОЗУ | 512 MB свободно | 2+ GB |
| Диск | 50 MB свободно | 100 MB |

### 1.2. Программные требования

| Компонент | Версия |
|-----------|--------|
| Windows | 10 x64 (22H2+) / 11 x64 (24H2+) / Server 2022 |
| Права | **Администратор** (обязательно) |

### 1.3. Комплект поставки

Для работы TcpRedirector необходимы три файла в одной директории:

```
TcpRedirectorService.exe   — исполняемый файл сервиса
WinDivert.dll              — библиотека WinDivert (x64)
WinDivert64.sys            — драйвер WinDivert (x64)
```

> **Важно:** Версии WinDivert.dll и WinDivert64.sys должны совпадать (рекомендуется WinDivert 2.2.2-A).

---

## 2. Подготовка к установке

### 2.1. Копирование файлов

1. Создайте директорию для сервиса, например:
   ```
   C:\Program Files\TcpRedirector\
   ```
   или
   ```
   C:\Tools\TcpRedirector\
   ```

2. Скопируйте все три файла в эту директорию:
   - `TcpRedirectorService.exe`
   - `WinDivert.dll`
   - `WinDivert64.sys`

### 2.2. Проверка прав администратора

Убедитесь, что вы запускаете командную строку (CMD) или PowerShell **от имени Администратора**:

```batch
whoami /groups | find "S-1-5-32-544"
```

Если вывод содержит `S-1-5-32-544` — права администратора есть.

---

## 3. Установка сервиса

### 3.1. Установка Windows Service

```batch
@echo off
REM Запустить от имени Администратора!

REM Перейти в директорию с файлами
cd /d "C:\Program Files\TcpRedirector"

REM Установить сервис
TcpRedirectorService.exe --install
```

При успешной установке вы увидите:
```
Service installed successfully
```

### 3.2. Проверка установки сервиса

```batch
sc query TcpRedirectorService
```

Ожидаемый вывод:
```
SERVICE_NAME: TcpRedirectorService
STATE              : 1  STOPPED
```

---

## 4. Настройка конфигурации

### 4.1. Автоматическое создание конфига

При первом запуске сервис создаёт конфигурацию по умолчанию:
```
%ProgramData%\TcpRedirector\config.json
```

### 4.2. Пример конфигурации

```json
{
    "app": {
        "exePath": "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe"
    },
    "proxy": {
        "host": "192.168.1.100",
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
    }
}
```

### 4.3. Параметры конфигурации

| Параметр | Описание | Пример |
|----------|----------|--------|
| `app.exePath` | Полный путь к EXE-файлу, чей трафик перехватывать | `C:\Program Files\Google\Chrome\Application\chrome.exe` |
| `proxy.host` | Адрес HTTP-прокси | `127.0.0.1` или `proxy.example.com` |
| `proxy.port` | Порт HTTP-прокси | `3128` |
| `proxy.enabled` | Включить/выключить перенаправление | `true` или `false` |
| `auth.enabled` | Требуется ли Basic-авторизация на прокси | `true` или `false` |
| `auth.username` | Логин для Basic-авторизации | `user` |
| `auth.kerberos` | Использовать Kerberos/Negotiate вместо Basic | `true` или `false` |
| `log.level` | Уровень логирования: 0=Error, 1=Warn, 2=Info, 3=Debug, 4=Trace | `2` |
| `log.fileEnabled` | Писать логи в файл | `true` |
| `log.maxSizeMB` | Максимальный размер файла лога до ротации | `10` |

### 4.4. Настройка Basic-авторизации

Если прокси требует Basic-авторизацию:

1. В `config.json` установите:
   ```json
   "auth": {
       "enabled": true,
       "username": "ваш_логин",
       "kerberos": false
   }
   ```

2. Пароль задаётся через IPC (из GUI) или через прямой вызов API.
   Пароль хранится в зашифрованном виде (DPAPI).

### 4.5. Настройка Kerberos-авторизации

Если прокси поддерживает Negotiate/Kerberos:

```json
"auth": {
    "enabled": true,
    "username": "",
    "encryptedPassword": "",
    "kerberos": true
}
```

> **Важно:** Kerberos-аутентификация использует учётную запись текущего пользователя Windows (под которым запущен сервис). Сервис обычно работает под `SYSTEM`. Для корректной работы Kerberos убедитесь, что:
> - Компьютер находится в домене (или настроен Kerberos)
> - Учётная запзь имеет доступ к прокси-серверу
> - DNS-разрешение целевого прокси работает корректно

---

## 5. Запуск и проверка

### 5.1. Запуск сервиса

```batch
net start TcpRedirectorService
```

Ожидаемый вывод:
```
The TcpRedirector Service service is starting.
The TcpRedirector Service service was started successfully.
```

### 5.2. Проверка статуса

```batch
sc query TcpRedirectorService
```

Ожидаемый вывод:
```
SERVICE_NAME: TcpRedirectorService
STATE              : 4  RUNNING
```

### 5.3. Проверка логов

Логи пишутся в:
```
%ProgramData%\TcpRedirector\logs\
```

Просмотр последних записей:
```batch
type "%ProgramData%\TcpRedirector\logs\tcp_redirector.log"
```

Ожидаемые записи при успешном запуске:
```
[2026-06-23 12:00:00.000] [INFO ] [service    ] Initializing TcpRedirector Service...
[2026-06-23 12:00:00.050] [INFO ] [service    ] TcpRelayServer started on port 34010
[2026-06-23 12:00:00.100] [INFO ] [service    ] WinDivert capture started (DST-modification mode)
[2026-06-23 12:00:00.150] [INFO ] [service    ] TcpRedirector Service initialized successfully
```

### 5.4. Консольный режим (для отладки)

Если WinDivert.dll отсутствует или сервис не установлен:

```batch
TcpRedirectorService.exe --console
```

В этом режиме:
- Сервис запускается как консольное приложение
- Логи выводятся в консоль и в файл
- Можно нажать Ctrl+C для остановки

---

## 6. Управление сервисом

### 6.1. Стандартные команды

```batch
REM Запуск
net start TcpRedirectorService

REM Остановка
net stop TcpRedirectorService

REM Статус
sc query TcpRedirectorService
```

### 6.2. Изменение типа запуска

```batch
REM Автоматический запуск при старте Windows
sc config TcpRedirectorService start= auto

REM Ручной запуск
sc config TcpRedirectorService start= demand

REM Отключение
sc config TcpRedirectorService start= disabled
```

### 6.3. Перезапуск сервиса

```batch
net stop TcpRedirectorService && net start TcpRedirectorService
```

---

## 7. Обновление

### 7.1. Остановка сервиса

```batch
net stop TcpRedirectorService
```

### 7.2. Замена файлов

```batch
copy /Y новый_TcpRedirectorService.exe "C:\Program Files\TcpRedirector\TcpRedirectorService.exe"
```

> **Примечание:** WinDivert.dll и WinDivert64.sys обычно не меняются между версиями.

### 7.3. Запуск обновлённого сервиса

```batch
net start TcpRedirectorService
```

---

## 8. Удаление

### 8.1. Остановка и удаление сервиса

```batch
@echo off
REM Запустить от имени Администратора!

REM Остановить сервис
net stop TcpRedirectorService

REM Удалить сервис
TcpRedirectorService.exe --uninstall
```

При успешном удалении:
```
Service uninstalled successfully
```

### 8.2. Удаление файлов (опционально)

```batch
rmdir /S /Q "C:\Program Files\TcpRedirector"
rmdir /S /Q "%ProgramData%\TcpRedirector"
```

---

## 9. Решение проблем

### 9.1. Сервис не запускается

**Симптом:** `net start` возвращает ошибку

**Проверка логов:**
```batch
type "%ProgramData%\TcpRedirector\logs\tcp_redirector.log"
```

**Возможные причины и решения:**

| Проблема | Решение |
|----------|---------|
| WinDivert.dll не найден | Поместите WinDivert.dll в ту же директорию, что и exe |
| WinDivert64.sys не найден | Поместите WinDivert64.sys рядом с exe |
| Нет прав администратора | Запустите CMD от имени Администратора |
| Прокси недоступен | Проверьте `proxy.host` и `proxy.port` в config.json |
| Конфиг повреждён | Удалите `%ProgramData%\TcpRedirector\config.json` и перезапустите сервис |

### 9.2. WinDivert не захватывает пакеты

**Проверка:**
1. Убедитесь, что WinDivert64.sys рядом с exe
2. Проверьте логи: `[WinDivert Open] handle=X`
3. Убедитесь, что целевой процесс запущен

### 9.3. CONNECT к прокси не работает

**Проверка логов:**
```
[RELAY] [CONNECT] Connecting to proxy.example.com:3128
[RELAY] [CONNECT] CONNECT example.com:443 HTTP/1.1
[RELAY] [CONNECT] Response: HTTP/1.1 200 Connection Established
```

**Если ошибка:**
- `connect failed: 10061` — прокси недоступен (проверьте адрес и порт)
- `407 Proxy Auth Required` — требуется авторизация (включите `auth.enabled`)
- `504 Gateway Timeout` — целевой хост недоступен

### 9.4. Kerberos-авторизация не работает

**Проверка логов:**
```
[SSPI] AcquireCredentialsHandle OK
[SSPI] Authenticating as: username (SPN=HTTP/proxy.example.com)
[SSPI] Token #1: 88 bytes
```

**Если ошибки:**
- `No Kerberos credentials` — сервис работает не под доменной учётной записью
- `SPN not found` — проверьте DNS-разрешение proxy.host

### 9.5. Логи не пишутся

```batch
REM Проверьте, что директория существует
dir "%ProgramData%\TcpRedirector\logs"

REM Проверьте права на запись
icacls "%ProgramData%\TcpRedirector\logs"
```

### 9.6. Получение диагностической информации

```batch
REM Версия сервиса
TcpRedirectorService.exe --version

REM Статус сервиса
sc queryex TcpRedirectorService

REM PID сервиса
sc queryex TcpRedirectorService | find "PID"

REM Последние 50 строк лога
powershell -Command "Get-Content '%ProgramData%\TcpRedirector\logs\tcp_redirector.log' -Tail 50"
```

---

## Приложение A: Быстрая установка (одной командой)

Скопируйте файлы в `C:\Tools\TcpRedirector\`, затем выполните:

```batch
@echo off
cd /d "C:\Tools\TcpRedirector"
TcpRedirectorService.exe --install
sc config TcpRedirectorService start= auto
net start TcpRedirectorService
sc query TcpRedirectorService
```

## Приложение B: Структура директорий после установки

```
C:\Program Files\TcpRedirector\
├── TcpRedirectorService.exe
├── WinDivert.dll
└── WinDivert64.sys

%ProgramData%\TcpRedirector\
├── config.json
└── logs\
    ├── tcp_redirector.log
    ├── tcp_redirector.1.log
    └── ...
```

---

*Документация подготовлена для проекта TcpRedirector.*
*Разработчик: Kalikin Iliya*