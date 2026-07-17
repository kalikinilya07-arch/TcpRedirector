# Руководство администратора — TcpRedirector v1.1.5

## 1. Назначение продукта

TcpRedirector — прозрачный TCP-редиректор для Windows 10/11 x64. Перехватывает TCP-трафик выбранных приложений на уровне ядра (WinDivert) и перенаправляет его через upstream HTTP(S)-прокси с поддержкой Kerberos/Negotiate и Basic аутентификации.

**Ключевые возможности:**
- Перехват TCP-пакетов на уровне ядра (WinDivert) — не требует настройки приложений
- Правила маршрутизации по имени процесса (например, `chrome.exe → proxy`)
- Аутентификация на прокси: Kerberos/Negotiate (SSPI) и Basic
- GUI для управления (WPF, .NET 9.0)
- Служба Windows (LocalSystem, автозапуск)

## 2. Архитектура развёртывания

```
┌─────────────────────────────────────────────────┐
│ Windows 10/11 x64                                │
│                                                   │
│  ┌──────────────────────┐                        │
│  │ TcpRedirectorService  │ SYSTEM, AutoStart     │
│  │ (C++ service)         │                        │
│  │  ├─ WinDivert         │ перехват пакетов       │
│  │  ├─ TcpRelayServer    │ HTTP CONNECT (34010)   │
│  │  ├─ AuthProvider      │ Kerberos/Basic         │
│  │  └─ PipeServer        │ IPC (named pipe)       │
│  └──────┬───────────────┘                        │
│         │ named pipe                              │
│  ┌──────┴───────────────┐                        │
│  │ TcpRedirectorAuthAgent│ User Session          │
│  │ (C++ agent)           │ SSPI/Kerberos         │
│  └──────────────────────┘                        │
│                                                   │
│  ┌──────────────────────┐                        │
│  │ TcpRedirectorGUI      │ User (без UAC)        │
│  │ (WPF, .NET 9.0)       │ Управление через IPC  │
│  └──────────────────────┘                        │
└─────────────────────────────────────────────────┘
```

## 3. Системные требования

| Компонент | Требование |
|-----------|------------|
| ОС | Windows 10 21H2+ / Windows 11 (x64) |
| .NET | Windows Desktop Runtime 9.0 (x64) |
| Права | Администратор (для установки драйвера и службы) |
| Память | ~50 МБ RAM (сервис) + ~100 МБ (GUI) |
| Диск | ~100 МБ |

## 4. Установка

### 4.1. Предварительные условия
1. Установить [.NET 9.0 Windows Desktop Runtime (x64)](https://dotnet.microsoft.com/download/dotnet/9.0)
2. Иметь права администратора

### 4.2. Порядок установки

```bat
:: 1. Распаковать архив в C:\Program Files\TcpRedirector\
:: 2. Установить драйвер WinDivert (однократно)
cd C:\Program Files\TcpRedirector\bin
install_windivert.bat

:: 3. Установить службу (однократно)
install_service.bat

:: 4. Запустить службу
sc start TcpRedirectorService

:: 5. Настроить автозапуск AuthAgent для текущего пользователя
::    (выполняется GUI автоматически при включении Kerberos)
```

### 4.3. Проверка установки

```bat
:: Проверить службу
sc query TcpRedirectorService
:: STATE должно быть: 4 RUNNING

:: Проверить драйвер
sc query WinDivert
:: STATE должно быть: 4 RUNNING

:: Проверить порты
netstat -ano | findstr "34010 34011"
:: 34010 — relay server (TCP)
:: 34011 — IPC (TCP)
```

## 5. Конфигурация

Файл: `%ProgramData%\TcpRedirector\config.json`

### 5.1. Секция `proxy`

```json
{
  "proxy": {
    "host": "proxy.example.com",
    "port": 3128
  }
}
```

### 5.2. Секция `auth` — аутентификация на прокси

```json
{
  "auth": {
    "enabled": true,
    "username": "DOMAIN\\user",
    "encryptedPassword": "<DPAPI-encrypted>",
    "kerberos": true,
    "authMode": "KerberosOnly"
  }
}
```

| Параметр | Значения | Описание |
|----------|----------|----------|
| `enabled` | `true`/`false` | Включить аутентификацию |
| `kerberos` | `true`/`false` | Использовать Kerberos (через AuthAgent) |
| `authMode` | `KerberosOnly`, `KerberosPreferred`, `BasicOnly` | Режим аутентификации |

**Режимы аутентификации:**
- `KerberosOnly` — только Kerberos/Negotiate. Ошибка → разрыв соединения.
- `KerberosPreferred` — Kerberos с fallback на Basic (по умолчанию).
- `BasicOnly` — только Basic Auth (логин/пароль).

### 5.3. Секция `rules` — правила маршрутизации

```json
{
  "rules": [
    {
      "type": "process",
      "action": "proxy",
      "pattern": "chrome.exe",
      "description": "Google Chrome"
    }
  ]
}
```

| Параметр | Значения | Описание |
|----------|----------|----------|
| `type` | `process`, `path`, `global` | Тип правила |
| `action` | `proxy`, `direct`, `block` | Действие |
| `pattern` | строка | Имя процесса или путь |

### 5.4. Секция `log` — логирование

```json
{
  "log": {
    "level": 2,
    "directory": "C:\\ProgramData\\TcpRedirector\\logs",
    "max_file_size_mb": 10,
    "max_files": 5
  }
}
```

| Уровень | Значение | Когда использовать |
|---------|----------|--------------------|
| 0 — Trace | Максимальная детализация | Отладка |
| 1 — Debug | Отладочные сообщения | Диагностика |
| 2 — Info | Информационные (по умолчанию) | Продакшен |
| 3 — Warn | Предупреждения | Продакшен |
| 4 — Error | Только ошибки | Продакшен |
| 5 — Off | Логирование отключено | — |

## 6. Ежедневная эксплуатация

### 6.1. Запуск/остановка службы

```bat
sc start TcpRedirectorService
sc stop TcpRedirectorService
```

### 6.2. Просмотр логов

```bat
:: Текущий лог
type "%ProgramData%\TcpRedirector\logs\tcp_redirector.log"

:: Последние 100 строк
powershell -Command "Get-Content '%ProgramData%\TcpRedirector\logs\tcp_redirector.log' -Tail 100"

:: Поиск ошибок
findstr /i "error fail" "%ProgramData%\TcpRedirector\logs\tcp_redirector.log"
```

### 6.3. Мониторинг соединений

```bat
:: Активные соединения через редиректор
netstat -ano | findstr "34010"
```

## 7. Диагностика неисправностей

### 7.1. Служба не запускается

```bat
:: Проверить статус
sc query TcpRedirectorService

:: Проверить журнал Windows
Eventvwr.msc → Windows Logs → System

:: Запустить в консольном режиме для диагностики
cd C:\Program Files\TcpRedirector\bin
TcpRedirectorService.exe --console
```

### 7.2. Трафик не перенаправляется

```bat
:: Проверить, что драйвер WinDivert загружен
sc query WinDivert

:: Проверить правила
type "%ProgramData%\TcpRedirector\config.json"

:: Проверить, что relay-сервер слушает порт
netstat -ano | findstr "34010"
```

### 7.3. Kerberos-аутентификация не работает

```bat
:: Проверить, что AuthAgent запущен
tasklist | findstr TcpRedirectorAuthAgent

:: Проверить named pipe агента
dir \\.\pipe\TcpRedirectorAuth

:: Проверить лог агента
type "%ProgramData%\TcpRedirector\logs\auth_agent.log"

:: Проверить задачу в планировщике
schtasks /query /tn "TcpRedirectorAuthAgent"

:: Проверить доступность контроллера домена
nltest /dsgetdc:DOMAIN
```

### 7.4. GUI не подключается к сервису

```bat
:: Проверить, что служба запущена
sc query TcpRedirectorService

:: Проверить named pipe сервиса
dir \\.\pipe\TcpRedirectorService

:: Запустить GUI от администратора
```

### 7.5. Медленная работа / деградация

```bat
:: Проверить количество активных соединений
netstat -ano | findstr "34010" | find /c "ESTABLISHED"

:: Проверить использование памяти службой
tasklist /fi "imagename eq TcpRedirectorService.exe"

:: Перезапустить службу
sc stop TcpRedirectorService && sc start TcpRedirectorService
```

## 8. Обновление

См. [`UPGRADE.md`](UPGRADE.md).

Краткий порядок:
```bat
sc stop TcpRedirectorService
taskkill /F /IM TcpRedirectorAuthAgent.exe
:: Заменить файлы .exe в C:\Program Files\TcpRedirector\bin\
sc start TcpRedirectorService
:: GUI запустит AuthAgent автоматически
```

## 9. Удаление

См. [`UNINSTALL.md`](UNINSTALL.md).

```bat
sc stop TcpRedirectorService
sc delete TcpRedirectorService
sc delete WinDivert
taskkill /F /IM TcpRedirectorAuthAgent.exe
schtasks /delete /tn "TcpRedirectorAuthAgent" /f
rmdir /s /q "C:\Program Files\TcpRedirector"
del /f "%ProgramData%\TcpRedirector\config.json"
```

## 10. Журнал изменений

См. [`CHANGELOG.md`](CHANGELOG.md).