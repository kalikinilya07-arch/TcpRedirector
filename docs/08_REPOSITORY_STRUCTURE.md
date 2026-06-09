# Структура репозитория и кодовой базы

## 1. Полная структура репозитория

```
TcpRedirector/
│
├── README.md                               # Описание проекта
├── LICENSE                                 # Лицензия
├── CONTRIBUTING.md                         # Правила контрибьюции
├── CHANGELOG.md                            # История изменений
├── .gitignore                              # Игнорируемые файлы
├── .editorconfig                           # Настройки редактора
│
├── docs/                                   # Документация
│   ├── 01_ARCHITECTURE_OVERVIEW.md         # Общая архитектура
│   ├── 02_WFP_DRIVER.md                    # WFP драйвер
│   ├── 03_WINDOWS_SERVICE.md              # Windows Service
│   ├── 04_GUI.md                          # GUI
│   ├── 05_CONNECTION_FLOW.md             # Поток соединения
│   ├── 06_IMPLEMENTATION_PLAN.md          # План реализации
│   ├── 07_RISKS_AND_LIMITATIONS.md       # Риски и ограничения
│   ├── 08_REPOSITORY_STRUCTURE.md         # Структура репозитория (этот файл)
│   ├── images/                            # Схемы и диаграммы
│   │   ├── architecture_overview.png
│   │   ├── wfp_layers.png
│   │   └── connection_flow.png
│   └── testing.md                         # План тестирования
│
├── src/
│   ├── driver/                             # WFP Redirect Driver (C++, WDK)
│   │   └── TcpRedirectorDriver/
│   │       ├── TcpRedirectorDriver.sln
│   │       ├── TcpRedirectorDriver.vcxproj
│   │       ├── TcpRedirectorDriver.inf
│   │       ├── driver.h
│   │       ├── driver.c                   # DriverEntry, DriverUnload
│   │       ├── trace.h                    # WPP трассировка
│   │       ├── callouts/
│   │       │   ├── redirect_callout.h
│   │       │   ├── redirect_callout.c     # ALE_AUTH_CONNECT классификация
│   │       │   ├── flow_callout.h
│   │       │   └── flow_callout.c         # FLOW_ESTABLISHED + CLOSURE
│   │       ├── common/
│   │       │   ├── redirect_info.h        # REDIRECT_INFO структура
│   │       │   ├── redirect_queue.h
│   │       │   ├── redirect_queue.c       # Очередь редиректов
│   │       │   ├── process_info.h
│   │       │   └── process_info.c         # PID → Process Name/Path
│   │       ├── communication/
│   │       │   ├── device_io.h
│   │       │   ├── device_io.c            # IOCTL обработчики
│   │       │   ├── ioctl_codes.h          # Определения IOCTL кодов
│   │       │   ├── event_channel.h
│   │       │   └── event_channel.c        # Kernel event management
│   │       ├── rules_cache.h
│   │       └── rules_cache.c              # Кэш правил (таблица быстрого поиска)
│   │
│   ├── service/                            # Windows Service (C++, Boost.Asio)
│   │   └── TcpRedirectorService/
│   │       ├── TcpRedirectorService.sln
│   │       ├── TcpRedirectorService.vcxproj
│   │       ├── main.cpp                   # Точка входа
│   │       ├── resource.h
│   │       ├── core/
│   │       │   ├── ServiceMain.h
│   │       │   ├── ServiceMain.cpp        # SCM entry point
│   │       │   ├── ServiceManager.h
│   │       │   ├── ServiceManager.cpp     # Жизненный цикл сервиса
│   │       │   └── ServiceInstaller.h
│   │       │   └── ServiceInstaller.cpp   # Install/Uninstall
│   │       ├── communication/
│   │       │   ├── DriverCommunicator.h
│   │       │   ├── DriverCommunicator.cpp # IOCTL с драйвером
│   │       │   ├── PipeServer.h
│   │       │   ├── PipeServer.cpp         # IPC с GUI
│   │       │   ├── IpcMessage.h
│   │       │   └── IpcMessage.cpp         # Форматы сообщений
│   │       ├── config/
│   │       │   ├── ConfigManager.h
│   │       │   ├── ConfigManager.cpp      # JSON конфигурация
│   │       │   ├── ProxyConfig.h
│   │       │   ├── ProxyConfig.cpp        # Параметры прокси
│   │       │   ├── SecretsManager.h
│   │       │   └── SecretsManager.cpp     # DPAPI
│   │       ├── rules/
│   │       │   ├── RuleEngine.h
│   │       │   ├── RuleEngine.cpp         # Движок правил
│   │       │   ├── RuleMatcher.h
│   │       │   ├── RuleMatcher.cpp        # Сопоставление с правилом
│   │       │   ├── Rule.h
│   │       │   ├── Rule.cpp               # Типы и структуры правил
│   │       │   └── WildcardMatcher.h
│   │       │   └── WildcardMatcher.cpp    # Wildcard matching
│   │       ├── proxy/
│   │       │   ├── ProxyEngine.h
│   │       │   ├── ProxyEngine.cpp        # Менеджер сессий
│   │       │   ├── ProxySession.h
│   │       │   ├── ProxySession.cpp       # Одна прокси-сессия
│   │       │   ├── HttpConnectParser.h
│   │       │   ├── HttpConnectParser.cpp  # Парсинг CONNECT ответа
│   │       │   ├── TunnelBridge.h
│   │       │   └── TunnelBridge.cpp       # Двусторонняя пересылка
│   │       ├── monitoring/
│   │       │   ├── ConnectionTracker.h
│   │       │   ├── ConnectionTracker.cpp  # Отслеживание соединений
│   │       │   ├── StatisticsCollector.h
│   │       │   ├── StatisticsCollector.cpp # Сбор статистики
│   │       │   └── PerformanceCounters.h
│   │       │   └── PerformanceCounters.cpp # Performance counters
│   │       └── logging/
│   │           ├── Logger.h
│   │           ├── Logger.cpp             # spdlog обёртка
│   │           ├── LogRotator.h
│   │           ├── LogRotator.cpp         # Ротация логов
│   │           └── LogSinks.h             # Кастомные sinks
│   │
│   ├── gui/                                # GUI (C#, WPF)
│   │   └── TcpRedirectorGUI/
│   │       ├── TcpRedirectorGUI.sln
│   │       ├── TcpRedirectorGUI.csproj
│   │       ├── App.xaml
│   │       ├── App.xaml.cs
│   │       ├── appsettings.json
│   │       ├── MainWindow.xaml
│   │       ├── MainWindow.xaml.cs
│   │       ├── Models/
│   │       │   ├── ProxyConfig.cs
│   │       │   ├── Rule.cs
│   │       │   ├── ConnectionRecord.cs
│   │       │   ├── LogEntry.cs
│   │       │   ├── ServiceStatus.cs
│   │       │   ├── ServiceStats.cs
│   │       │   └── IpcMessage.cs
│   │       ├── ViewModels/
│   │       │   ├── ShellViewModel.cs
│   │       │   ├── ProxySettingsViewModel.cs
│   │       │   ├── RulesViewModel.cs
│   │       │   ├── ConnectionsViewModel.cs
│   │       │   ├── LogsViewModel.cs
│   │       │   └── ServiceControlViewModel.cs
│   │       ├── Views/
│   │       │   ├── ProxySettingsView.xaml
│   │       │   ├── ProxySettingsView.xaml.cs
│   │       │   ├── RulesView.xaml
│   │       │   ├── RulesView.xaml.cs
│   │       │   ├── ConnectionsView.xaml
│   │       │   ├── ConnectionsView.xaml.cs
│   │       │   ├── LogsView.xaml
│   │       │   ├── LogsView.xaml.cs
│   │       │   ├── ServiceControlView.xaml
│   │       │   └── ServiceControlView.xaml.cs
│   │       ├── Services/
│   │       │   ├── IIpcClient.cs
│   │       │   ├── IpcClient.cs           # Named Pipe client
│   │       │   ├── IServiceController.cs
│   │       │   ├── ServiceController.cs   # SCM management
│   │       │   └── NotificationService.cs
│   │       ├── Converters/
│   │       │   ├── BoolToVisibilityConverter.cs
│   │       │   ├── ConnectionStateToColorConverter.cs
│   │       │   ├── LogLevelToColorConverter.cs
│   │       │   └── BytesToHumanReadableConverter.cs
│   │       ├── Controls/
│   │       │   ├── ConnectionDetailsControl.xaml
│   │       │   ├── ConnectionDetailsControl.xaml.cs
│   │       │   ├── RuleEditorControl.xaml
│   │       │   ├── RuleEditorControl.xaml.cs
│   │       │   ├── StatisticsControl.xaml
│   │       │   └── StatisticsControl.xaml.cs
│   │       └── Styles/
│   │           ├── AppTheme.xaml
│   │           ├── ControlStyles.xaml
│   │           └── Colors.xaml
│   │
│   └── common/                              # Общие компоненты
│       ├── TcpRedirectorCommon/
│       │   ├── TcpRedirectorCommon.vcxproj
│       │   ├── IpcProtocol.h               # IPC контракты
│       │   ├── IpcProtocol.cpp
│       │   ├── RedirectData.h              # REDIRECT_INFO (user-mode копия)
│       │   ├── IoctlCodes.h                # IOCTL коды (shared)
│       │   └── Version.h                   # Версия продукта
│       └── TcpRedirectorCommon.csproj      # C# общие типы
│           ├── IpcMessages.cs
│           └── ConnectionTypes.cs
│
├── tests/
│   ├── driver/                              # Тесты драйвера
│   │   ├── TcpRedirectorDriverTest/
│   │   │   ├── TcpRedirectorDriverTest.vcxproj
│   │   │   └── ...
│   │   └── test_utilities/
│   │       ├── MockWfpApi.h                # Mock для WFP API
│   │       └── TestDriverControl.cpp       # Загрузка/выгрузка драйвера
│   │
│   ├── service/                             # Тесты сервиса
│   │   └── TcpRedirectorServiceTest/
│   │       ├── TcpRedirectorServiceTest.vcxproj
│   │       ├── tests/
│   │       │   ├── RuleEngineTest.cpp
│   │       │   ├── RuleMatcherTest.cpp
│   │       │   ├── WildcardMatcherTest.cpp
│   │       │   ├── HttpConnectParserTest.cpp
│   │       │   ├── ProxyConfigTest.cpp
│   │       │   ├── ConfigManagerTest.cpp
│   │       │   ├── SecretsManagerTest.cpp
│   │       │   └── ConnectionTrackerTest.cpp
│   │       └── mocks/
│   │           ├── MockDriverCommunicator.h
│   │           └── MockPipeServer.h
│   │
│   ├── gui/                                 # Тесты GUI
│   │   └── TcpRedirectorGUITest/
│   │       ├── TcpRedirectorGUITest.csproj
│   │       ├── ViewModelTests/
│   │       │   ├── ProxySettingsViewModelTest.cs
│   │       │   ├── RulesViewModelTest.cs
│   │       │   └── ConnectionsViewModelTest.cs
│   │       └── Services/
│   │           └── IpcClientTest.cs
│   │
│   └── integration/                         # Интеграционные тесты
│       ├── TcpRedirectorIntegrationTest/
│       │   ├── TcpRedirectorIntegrationTest.vcxproj
│       │   ├── FullPipelineTest.cpp         # Полный pipeline
│       │   ├── ProxyCompatibilityTest.cpp   # Совместимость с прокси
│       │   └── StressTest.cpp               # Нагрузочное тестирование
│       └── TestProxy/                       # Тестовый прокси-сервер
│           ├── TestProxy.h
│           └── TestProxy.cpp
│
├── tools/                                   # Инструменты
│   ├── scripts/
│   │   ├── build_driver.cmd                # Сборка драйвера
│   │   ├── build_service.cmd               # Сборка сервиса
│   │   ├── build_gui.cmd                   # Сборка GUI
│   │   ├── build_all.cmd                   # Полная сборка
│   │   ├── install_driver.cmd              # Установка драйвера
│   │   ├── uninstall_driver.cmd            # Удаление драйвера
│   │   ├── install_service.cmd             # Установка сервиса
│   │   ├── uninstall_service.cmd           # Удаление сервиса
│   │   ├── enable_testsigning.cmd          # Включение тестовой подписи
│   │   └── enable_verbose_wfp.cmd          # Включение WFP лога
│   ├── TcpRedirectorDriverUtil/            # Консольная утилита для отладки
│   │   ├── TcpRedirectorDriverUtil.vcxproj
│   │   ├── main.cpp                        # Работа с драйвером
│   │   └── commands.h                      # Команды: monitor, stats, rules
│   └── TcpRedirectorServiceTester/         # Утилита тестирования сервиса
│       ├── TcpRedirectorServiceTester.vcxproj
│       └── main.cpp
│
├── build/                                   # Выходные файлы сборки
│   ├── driver/
│   │   ├── amd64/
│   │   │   └── TcpRedirectorDriver.sys
│   │   └── x86/  (не требуется для MVP)
│   ├── service/
│   │   └── Release/
│   │       └── TcpRedirectorService.exe
│   ├── gui/
│   │   └── Release/
│   │       └── TcpRedirectorGUI.exe
│   └── installer/
│       └── TcpRedirectorSetup.msi
│
├── installer/                               # Инсталлятор
│   ├── Product.wxs                         # WiX configuration
│   ├── Driver.wxs                          # Driver component
│   ├── Service.wxs                         # Service component
│   └── GUI.wxs                             # GUI component
│
└── .github/                                 # GitHub CI/CD
    └── workflows/
        ├── build.yml                        # Build all components
        └── tests.yml                        # Run tests
```

## 2. Количество файлов по компонентам

| Компонент | Header-файлы | Source-файлы | Всего |
|-----------|-------------|-------------|-------|
| WFP Driver | 10 | 10 | 20 |
| Service | 22 | 22 | 44 |
| GUI | 15 | 30 | 45 |
| Common (C++) | 3 | 2 | 5 |
| Common (C#) | 2 | — | 2 |
| Tests | ~12 | ~12 | 24 |
| Tools | ~3 | ~3 | 6 |
| Installer | — | 4 | 4 |
| Scripts | — | 10 | 10 |
| Docs | 8 | — | 8 |
| **Итого** | **~75** | **~93** | **~168** |

## 3. Зависимости

### WFP Driver (C++/WDK)

```
Windows Driver Kit (WDK 10.0.26100.0)
  ├── fwpsk.h          — WFP callout API (kernel)
  ├── fwpmk.h          — WFP management API (kernel)
  ├── wdm.h            — Windows Driver Model
  ├── ntddk.h          — NT kernel API
  └── ks.h             — kernel streaming (event objects)

Windows SDK (10.0.26100.0)
  └── fwpmu.h          — WFP user-mode management API
```

### Windows Service (C++)

```
External:
  ├── Boost.Asio 1.86+       — Асинхронный TCP
  ├── Boost.Beast (optional) — Для HTTP CONNECT (или самописный)
  ├── nlohmann/json 3.11+    — JSON парсинг
  ├── spdlog 1.14+           — Логирование
  └── fmt 11.0+              — Форматирование (или использовать std::format)

Windows API:
  ├── wtsapi32.h   — Terminal Services API
  ├── winsvc.h     — Service Control Manager
  └── dpapi.h      — CryptProtectData / CryptUnprotectData
```

### GUI (C#)

```
External:
  ├── CommunityToolkit.Mvvm 8.x   — MVVM Source Generators
  ├── Microsoft.Extensions.DI     — Dependency Injection
  ├── System.IO.Pipes             — Named Pipe (встроенный)
  ├── System.Text.Json            — JSON (встроенный)
  └── LiveChartsCore/ScottPlot    — Графики (опционально)
```

## 4. Сборка проекта

### Системные требования

| Инструмент | Версия | Назначение |
|-----------|--------|------------|
| Visual Studio | 2022 17.12+ | IDE |
| MSVC Toolchain | v143 | C++ компилятор |
| Windows SDK | 10.0.26100.0+ | Windows API |
| WDK | 10.0.26100.0+ | Драйверы |
| .NET SDK | 8.0+ | GUI |
| CMake | 3.28+ | (опционально, для драйвера) |
| WiX Toolset | 5.0+ | Инсталлятор |

### Команды сборки

```bash
# Сборка драйвера (через MSBuild)
msbuild src\driver\TcpRedirectorDriver\TcpRedirectorDriver.vcxproj /p:Configuration=Release /p:Platform=x64

# Сборка сервиса
msbuild src\service\TcpRedirectorService\TcpRedirectorService.sln /p:Configuration=Release

# Сборка GUI
dotnet build src\gui\TcpRedirectorGUI\TcpRedirectorGUI.csproj -c Release

# Полная сборка (через скрипт)
tools\scripts\build_all.cmd

# Установка драйвера (требует админских прав)
tools\scripts\install_driver.cmd

# Установка сервиса
tools\scripts\install_service.cmd
```

## 5. Конфигурационные файлы

### %ProgramData%\TcpRedirector\config.json

```json
{
    "proxy": {
        "host": "proxy.example.com",
        "port": 3128,
        "login": "user",
        "password_encrypted": "AQAAANCMnd8BFdERjHoAwE/Cl+sBAAAA..."
    },
    "rules": [
        {
            "id": "550e8400-e29b-41d4-a716-446655440000",
            "type": "process_name",
            "action": "proxy",
            "pattern": "chrome.exe",
            "description": "Google Chrome",
            "priority": 1,
            "enabled": true
        }
    ],
    "logging": {
        "level": "INFO",
        "max_file_size_mb": 50,
        "max_files": 10,
        "max_age_days": 30
    },
    "service": {
        "auto_start": true,
        "keep_driver_loaded": true,
        "max_connections": 1000
    }
}
```

### Расположение файлов на диске

```
%ProgramData%\TcpRedirector\
├── config.json               # Конфигурация (зашифрованный пароль)
├── logs\
│   ├── tcp_redirector.log    # Текущий лог
│   ├── tcp_redirector.1.log  # Ротированный
│   ├── tcp_redirector.2.log
│   └── ...
├── driver\
│   └── TcpRedirectorDriver.sys  # Копия драйвера
└── stats.db                  # (опционально) БД статистики

%ProgramFiles%\TcpRedirector\
├── TcpRedirectorService.exe
├── TcpRedirectorGUI.exe
└── dependencies\
    ├── boost_asio.dll
    ├── spdlog.dll
    └── nlohmann_json.dll

%SystemRoot%\System32\drivers\
└── TcpRedirectorDriver.sys   # Системная копия драйвера