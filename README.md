# TcpRedirector v1.1.5

Прозрачный TCP-прокси-редиректор для Windows. Перехватывает TCP-трафик выбранных приложений на уровне ядра (WinDivert) и перенаправляет через upstream HTTP-прокси с поддержкой Kerberos/Negotiate аутентификации.

## Возможности

| Функция | Описание |
|---------|----------|
| Перехват трафика | WinDivert — перехват TCP-пакетов на уровне ядра, без LSP/WFP |
| Прокси-редирект | Прозрачное перенаправление TCP-соединений через upstream HTTP(S) прокси |
| Kerberos/Negotiate | Полноценная SSPI-аутентификация через выделенный AuthAgent |
| Basic-аутентификация | Резервный режим с логином/паролем |
| DPAPI-шифрование | Пароль прокси хранится зашифрованным в конфиге (Windows DPAPI) |
| Гибкие правила | Фильтрация по процессу, IP-адресу, порту, CIDR-маске |
| GUI (WPF) | Управление правилами, мониторинг соединений, график трафика |
| Windows-сервис | Автозапуск, работа в фоне без интерактивного входа |
| Ротация логов | Автоматическая архивация и сжатие старых логов |

## Характеристики

| Параметр | Значение |
|----------|----------|
| Языки | C++17 (сервис, агент), C# 13 (GUI) |
| Платформа | Windows 10/11 x64 |
| Требования | .NET 9.0 Windows Desktop Runtime |
| Права | Администратор (обязательно для WinDivert) |
| Версия | 1.1.5 |

## Быстрая установка

### 1. Установить .NET 9.0 Runtime
Скачать и установить **Windows Desktop Runtime 9.0 (x64)**:
```
https://dotnet.microsoft.com/download/dotnet/9.0
```

### 2. Распаковать релизный архив
Скачайте [`TcpRedirector_v1.1.5.zip`](release/1.1.5/TcpRedirector_v1.1.5.zip) и распакуйте в `C:\Program Files\TcpRedirector\`.

### 3. Запустить установку
От **Администратора** выполните:
```batch
cd C:\Program Files\TcpRedirector
install.bat
```
Скрипт установит драйвер WinDivert, зарегистрирует Windows-сервис и запустит его.

### 4. Запустить GUI
```batch
C:\Program Files\TcpRedirector\bin\TcpRedirectorGUI.exe
```
GUI подключается к сервису через именованный канал (IPC). Для Kerberos-аутентификации включите опцию в настройках — агент [`TcpRedirectorAuthAgent.exe`](src/auth-agent/) запустится автоматически.

## Структура после установки

```
C:\Program Files\TcpRedirector\
├── bin\
│   ├── TcpRedirectorService.exe    # C++ сервис (WinDivert + релей)
│   ├── TcpRedirectorAuthAgent.exe  # Агент Kerberos/Negotiate (SSPI)
│   ├── TcpRedirectorGUI.exe        # GUI (WPF, .NET 9.0)
│   ├── WinDivert.dll               # Драйвер перехвата пакетов
│   ├── WinDivert64.sys             # Драйвер ядра
│   ├── install_windivert.bat       # Установка драйвера
│   └── gui\                        # Зависимости .NET (self-contained)
├── docs\                           # Документация (RU + EN)
├── install.bat                     # Полная установка
├── uninstall.bat                   # Полное удаление
├── deploy.bat                      # Сборка релизного пакета (для разработчиков)
└── TcpRedirectorAuthAgent.xml      # Манифест Scheduled Task для AuthAgent
```

## Архитектура

```
┌──────────────────┐                         ┌─────────────────────────┐
│   GUI (WPF/C#)   │                         │   AuthAgent (C++)       │
│   net9.0         │                         │   SSPI Negotiate        │
│   Правила,       │                         │   User Session          │
│   Мониторинг     │                         └───────────┬─────────────┘
└────────┬─────────┘                                     │
         │ Named Pipe IPC                                │ Named Pipe IPC
         │ (config, stats,                               │ (create_context,
         │  connections)                                 │  continue_context)
         ▼                                               ▼
┌─────────────────────────────────────────────────────────────────────┐
│                    TcpRedirectorService (C++)                        │
│                    SYSTEM Account                                    │
│                                                                     │
│  ┌──────────────┐  ┌───────────────┐  ┌──────────────────────────┐  │
│  │ WinDivert    │  │ Rule Engine   │  │ TcpRelayServer           │  │
│  │ Capture      │──│ (process/ip/  │──│ (CONNECT tunnel +        │  │
│  │ (kernel)     │  │  port/CIDR)   │  │  bidirectional relay)    │  │
│  └──────────────┘  └───────────────┘  └───────────┬──────────────┘  │
│                                                    │                 │
│                                          ┌─────────▼──────────┐     │
│                                          │ Auth Provider      │     │
│                                          │ KerberosAgent /    │     │
│                                          │ Basic (DPAPI)      │     │
│                                          └────────────────────┘     │
└─────────────────────────────────────────────────────────────────────┘
         │                                               │
         │ WinDivert                                     │ TCP
         │ (packet interception)                         │ (upstream proxy)
         ▼                                               ▼
   Приложения                                     Upstream
   (Chrome, etc.)                                 HTTP-прокси
```

### Поток данных

1. **WinDivert** перехватывает исходящие TCP-пакеты на уровне ядра
2. **Rule Engine** проверяет: процесс/IP/порт совпадает с правилом? Если нет — пакет уходит без изменений
3. Для подходящих пакетов **TcpRelayServer** устанавливает CONNECT-туннель к upstream-прокси
4. **Auth Provider** добавляет `Proxy-Authorization: Negotiate <token>` (Kerberos) или `Basic` в CONNECT-запрос
5. После установки туннеля (200 OK) запускается **Bridge** — двунаправленная ретрансляция данных между клиентом и прокси

## Документация

Вся документация в двух вариантах (RU/EN) находится в [`docs/`](docs/):

| Документ | RU | EN |
|----------|----|----|
| Инструкция администратора | [ADMIN_GUIDE_RU.md](docs/ADMIN_GUIDE_RU.md) | [ADMIN_GUIDE_EN.md](docs/ADMIN_GUIDE_EN.md) |
| Меры защиты информации | [SECURITY_RU.md](docs/SECURITY_RU.md) | [SECURITY_EN.md](docs/SECURITY_EN.md) |
| Архитектура приложения | [ARCHITECTURE_RU.md](docs/ARCHITECTURE_RU.md) | [ARCHITECTURE_EN.md](docs/ARCHITECTURE_EN.md) |
| Документация для разработчика | [DEVELOPER_RU.md](docs/DEVELOPER_RU.md) | [DEVELOPER_EN.md](docs/DEVELOPER_EN.md) |

Дополнительно:
- [`BUILD_AND_DEPLOY.md`](docs/BUILD_AND_DEPLOY.md) — сборка и развертывание
- [`INSTALL.md`](docs/INSTALL.md) / [`UNINSTALL.md`](docs/UNINSTALL.md) — установка и удаление
- [`UPGRADE.md`](docs/UPGRADE.md) — обновление с предыдущих версий
- [`WINDIVERT_TROUBLESHOOTING.md`](docs/WINDIVERT_TROUBLESHOOTING.md) — решение проблем с WinDivert
- [`SMOKE_TEST.md`](docs/SMOKE_TEST.md) — смоук-тестирование

## Сборка из исходников

### Требования
- Visual Studio 2022+ (C++ Desktop Development)
- .NET 9.0 SDK
- vcpkg (для C++ зависимостей: nlohmann-json, spdlog)

### C++ сервис и агент
```batch
build.bat          # Release-сборка сервиса + агента
build_debug.bat    # Debug-сборка
build_agent.bat    # Только агент
```

### C# GUI
```batch
dotnet build src/gui/TcpRedirectorGUI/TcpRedirectorGUI.csproj -c Release
```

### Сборка релизного пакета
```batch
deploy.bat         # Копирует сборку в releases\v1.1.5\, создаёт ZIP
```

## Тесты
```batch
cd tests/build2 && ctest -C Release
```

## Changelog

См. [`CHANGELOG.md`](CHANGELOG.md).

### v1.1.5
- **Исправлено**: гонка на именованном канале AuthAgent (recursive_mutex) — устранены зависания потоков ConnectionHandler
- **Исправлено**: TCP_NODELAY на клиентских и прокси-сокетах — устранены задержки алгоритма Нейгла
- **Исправлено**: убран лишний FlushFileBuffers на message-mode пайпе
- **Исправлено**: try-catch вокруг OneWayRelay(dn_cfg) для exception safety
- **Исправлено**: переинициализация AuthProvider при reload конфига через GUI

### v1.1.4
- **Исправлено**: SSPI-буфер — правильный размер выходного токена
- **Добавлено**: WarmUp() — предварительный запуск AuthAgent при старте сервиса
- **Добавлено**: логирование bridge-соединений

### v1.1.3
- Kerberos/Negotiate аутентификация через выделенный AuthAgent
- DPAPI-шифрование пароля в config.json
- Атомарная запись конфига (temp file + ReplaceFile)

## Лицензия

Proprietary. All rights reserved.
