# Структура репозитория

**Разработчик:** Kalikin Iliya

## 1. Полная структура репозитория

```
TcpRedirector/
│
├── README.md                               # Описание проекта
├── .gitignore                              # Игнорируемые файлы
├── CMakeLists.txt                          # Корневой CMake (для тестов)
├── vcpkg.json                              # Зависимости vcpkg
├── build.bat                               # Сборка сервиса
├── build_debug.bat                         # Сборка Debug
├── build_refactored.bat                       # (резерв)
├── run.bat                                 # Запуск сервиса
├── TESTS_AND_INSTRUCTIONS.md               # Инструкции по тестам
│
├── docs/                                   # Документация
│   ├── 01_ARCHITECTURE_OVERVIEW.md         # Архитектура (WinDivert DST-mod)
│   ├── 03_WINDOWS_SERVICE.md              # Windows Service (актуальная)
│   ├── 04_GUI.md                          # GUI дизайн (WPF C#)
│   ├── 05_CONNECTION_FLOW.md             # Поток соединения (DST mod)
│   ├── 07_RISKS_AND_LIMITATIONS.md       # Риски и ограничения
│   ├── 08_REPOSITORY_STRUCTURE.md         # Структура репозитория (этот файл)
│   ├── ANALYSIS.md                        # Анализ архитектуры перехвата
│   ├── CONFIG_DESIGN.md                   # Дизайн конфигурации
│   ├── DST_MODIFICATION_AUDIT.md          # Аудит DST modification
│   ├── GUI_DESIGN.md                      # Дизайн GUI (3 вкладки)
│   ├── HEXAGONAL_AUDIT.md                 # Аудит гексагональной архитектуры
│   ├── INSTALL_GUIDE.md                   # Инструкция по установке
│   ├── LOGGER_DESIGN.md                   # Дизайн логгера
│   ├── PACKET_INJECTION_TASK.md           # Задание на DST modification
│   ├── REVIEW.md                          # Cross-review компонентов
│   ├── STATS_DESIGN.md                    # Дизайн статистики
│   ├── STATUS.md                          # Статус проекта
│   └── TEST_INSTRUCTIONS.md               # Инструкции по тестированию
│
├── src/
│   └── service/                            # Windows Service (C++)
│       └── TcpRedirectorService/
│           ├── main.cpp                   # Точка входа:
│           │                                # --install / --uninstall / --console
│           ├── TcpRedirectorService.vcxproj # Проект Visual Studio
│           ├── CompositionRoot.h           # DI-контейнер
│           │
│           ├── adapters/
│           │   ├── driving/
│           │   │   ├── ServiceMain.h       # TcpRedirectorService — жизненный цикл
│           │   │   ├── IpcHandler.h        # Обработчик IPC-запросов (GUI)
│           │   │   └── (резерв)
│           │   └── driven/
│           │       └── ProxyEngine.h       # Proxy-движок (bridge mode, не используется)
│           │
│           ├── domain/
│           │   ├── entities/
│           │   │   └── ProxyConfig.h       # ProxyConfig, Rule, ConnectionRecord, LogEntry
│           │   ├── ports/
│           │   │   ├── ICapture.h           # Порт захвата пакетов
│           │   │   ├── IConfigStore.h       # Порт конфигурации
│           │   │   ├── IConnectionMonitor.h # Порт мониторинга
│           │   │   ├── IConnectionTable.h   # Порт таблицы соединений (ConnectionInfo)
│           │   │   ├── IProxyConnector.h    # Порт подключения к прокси
│           │   │   └── IRelayServer.h       # Порт relay-сервера
│           │   └── services/
│           │       ├── RuleEngine.h         # Движок правил маршрутизации
│           │       └── ConnectionTracker.h  # Отслеживание соединений
│           │
│           └── infrastructure/
│               ├── auth/
│               │   ├── auth_sspi.h          # SSPI-аутентификация (Negotiate/Kerberos)
│               │   └── auth_sspi.cpp        # Реализация SSPI
│               ├── capture/
│               │   ├── WinDivertCapture.h   # Захват TCP-пакетов через WinDivert
│               │   └── WinDivertCapture.cpp # Реализация захвата + DST modification
│               ├── config/
│               │   ├── Config.h             # Структуры Config, AuthSettings, LogSettings
│               │   ├── ConfigManager.h      # Управление конфигурацией
│               │   └── ConfigManager.cpp    # JSON + DPAPI + thread-safe
│               ├── ipc/
│               │   └── PipeServer.h         # Named Pipe сервер для GUI
│               ├── logging/
│               │   ├── Logger.h             # Асинхронный логгер
│               │   └── Logger.cpp           # WriterThread, ротация, ring buffer
│               ├── relay/
│               │   ├── TcpRelayServer.h     # Relay-сервер (CONNECT + bridge)
│               │   └── ConnectionTable.h    # Таблица соединений (SRWLock)
│               └── stats/
│                   ├── StatsCollector.h     # Сбор статистики (lock-free)
│                   └── StatsCollector.cpp   # OnPacket/GetStats/Reset
│
├── tests/                                   # Тесты
│   ├── CMakeLists.txt                       # CMake для тестов (Catch2 v3)
│   ├── README.md                            # Описание тестов
│   ├── test_main.cpp                        # Основные тесты
│   ├── test_comprehensive.cpp               # Комплексные тесты (95% coverage)
│   ├── test_mock_proxy.cpp                  # Тесты mock-прокси
│   ├── test_packet_gen.cpp                  # Тесты генератора пакетов
│   ├── mock_proxy.cpp                       # Mock HTTP-прокси
│   ├── mock_proxy.exe                       # Собранный бинарь
│   ├── packet_generator.cpp                 # Генератор тестовых пакетов
│   ├── packet_generator.exe                 # Собранный бинарь
│   ├── REDIRECT_TEST_PLAN.md                # План тестирования редиректа
│   ├── integration_test.bat                 # Интеграционный тест
│   ├── run_integration_test.bat             # Запуск интеграционного теста
│   ├── run_test.bat                         # Запуск тестов
│   ├── prepare_test_build.bat               # Подготовка тестовой сборки
│   ├── test_redirect_with_google.bat        # Тест редиректа через google
│   ├── setup_config.ps1                     # PowerShell скрипт конфигурации
│   ├── proxy_log.txt                        # Лог тестового прокси
│   ├── proxy_google_log.txt                 # Лог теста google
│   │
│   ├── unit/
│   │   └── service/
│   │       └── RuleEngineTest.cpp           # Unit-тесты RuleEngine
│   │
│   └── build*/                              # Директории сборки (в .gitignore)
│       └── _deps/catch2-src/               # Catch2 v3 (через CMake FetchContent)
│
├── external/                                # Внешние зависимости
│   ├── ProxyBridge/                         # ProxyBridge (эталонная реализация)
│   │   ├── Linux/                           # ProxyBridge для Linux
│   │   ├── MacOS/                           # ProxyBridge для macOS
│   │   └── Windows/                         # ProxyBridge для Windows
│   │       ├── cli/                         # CLI-инструменты
│   │       │   └── main.c                   # CLI для тестирования
│   │       ├── src/                         # Исходный код
│   │       │   ├── ProxyBridge.c            # Основная реализация
│   │       │   ├── ProxyBridge.h            # Заголовочный файл
│   │       │   └── ProxyBridge.rc           # Ресурсы
│   │       └── README.md                    # Документация ProxyBridge
│   │
│   └── WinDivert/
│       └── WinDivert-2.2.2-A/              # WinDivert SDK
│           ├── include/
│           │   └── windivert.h              # Заголовочный файл WinDivert
│           ├── lib/
│           │   └── x64/
│           │       └── windivert.lib        # Статическая библиотека
│           ├── README                       # Документация WinDivert
│           └── (бинарные файлы: WinDivert.dll, WinDivert64.sys)
│
├── gui/                                     # WPF GUI (C#)
│   └── TcpRedirectorGUI/
│       ├── Domain/
│       │   ├── Entities/
│       │   │   └── ProxyConfig.cs           # Модели данных
│       │   └── Ports/
│       │       └── ITcpRedirectorService.cs # Интерфейс сервиса
│       │
│       ├── Adapters/
│       │   └── Driving/
│       │       └── Wpf/
│       │           └── ViewModels/
│       │               ├── ShellViewModel.cs         # Главная VM
│       │               └── ProxySettingsVM.cs        # Настройки прокси
│       │
│       └── Infrastructure/
│           └── Ipc/
│               ├── IpcClient.cs             # Named Pipe клиент
│               └── ServiceController.cs     # SCM управление
│
└── tools/                                    # Инструменты
    ├── scripts/
    │   ├── build_all.bat                    # Сборка всего проекта
    │   ├── build_all.cmd                    # Сборка (альтернатива)
    │   ├── build_driver.bat                 # Сборка драйвера (заглушка)
    │   ├── install_service.cmd              # Установка сервиса
    │   ├── setup_deps.bat                   # Установка зависимостей
    │   ├── setup_npcap_sdk.bat              # Установка Npcap SDK
    │   ├── create_cert.ps1                  # Создание сертификата
    │   ├── install_cert.bat                 # Установка сертификата
    │   └── sign_driver.bat                  # Подпись драйвера
    │
    └── certs/
        ├── TcpRedirector.cer                # Сертификат
        └── TcpRedirector.pfx                # PFX-сертификат
```

## 2. Количество файлов по компонентам

| Компонент | Файлов | Формат |
|-----------|--------|--------|
| Windows Service (C++) | 20 | .h / .cpp |
| Документация | 18 | .md |
| Тесты | 20 | .cpp / .bat / .md |
| GUI (C#) | 5 | .cs |
| Скрипты | 9 | .bat / .cmd / .ps1 |
| External | ~10 | .c / .h / .md |
| Инструменты | 4 | .bat / .ps1 / .cer |
| **Итого** | **~86** | |

## 3. Зависимости

### Windows Service (C++)

```
External:
  ├── WinDivert 2.2.2-A — захват и модификация пакетов
  ├── nlohmann/json 3.11+ — JSON парсинг (через vcpkg)
  └── Windows SSPI (secur32.lib) — Kerberos/Negotiate auth

Windows API:
  ├── WinSock2 (ws2_32.lib) — TCP сокеты
  ├── WinDivert (windivert.lib) — захват пакетов
  ├── SCM (advapi32.lib) — Service Control Manager
  └── DPAPI (crypt32.lib) — шифрование пароля
```

### GUI (C#)

```
External:
  └── CommunityToolkit.Mvvm 8.x — MVVM Source Generators

Встроенное:
  ├── System.IO.Pipes — Named Pipe клиент
  └── System.Text.Json — сериализация
```

## 4. Сборка проекта

### Требования

| Инструмент | Версия | Назначение |
|-----------|--------|------------|
| Visual Studio | 2022 17.12+ | IDE |
| MSVC Toolchain | v143 | C++ компилятор |
| Windows SDK | 10.0.26100+ | Windows API |
| WinDivert | 2.2.2-A | Захват пакетов |

### Сборка сервиса

```bash
msbuild src\service\TcpRedirectorService\TcpRedirectorService.vcxproj /p:Configuration=Release /p:Platform=x64
```

### Сборка тестов

```bash
cmake -S tests -B tests/build
cmake --build tests/build
```

### Установка

```bash
# Установка сервиса (требует админских прав)
src\service\TcpRedirectorService\build\service\x64\Release\TcpRedirectorService.exe --install

# Запуск
net start TcpRedirectorService
```

## 5. Конфигурационные файлы

### %ProgramData%\TcpRedirector\config.json

```json
{
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
    }
}
```

### Расположение файлов на диске

```
%ProgramData%\TcpRedirector\
├── config.json               # Конфигурация (зашифрованный пароль)
└── logs\
    └── tcp_redirector.log    # Текущий лог
    └── tcp_redirector.1.log  # Ротированный
    └── ...

Директория сервиса:
├── TcpRedirectorService.exe
├── WinDivert.dll
└── WinDivert64.sys