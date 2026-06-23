# TcpRedirector

Прозрачное перенаправление TCP-соединений выбранного приложения через HTTP Proxy (CONNECT) на Windows 10/11 x64.

**Разработчик:** Kalikin Iliya

---

## Архитектура

```
┌──────────┐     ┌──────────────────┐     ┌──────────────┐     ┌──────────┐
│   GUI    │────▶│    Service       │◀───▶│  WinDivert   │◀───▶│  Apps    │
│ (WPF C#) │     │ (C++ DST mod)   │     │ Capture/Diver│     │(chrome..)│
└──────────┘     └──────┬───────────┘     └──────────────┘     └──────────┘
                        │
                        ▼
                 ┌──────────────┐
                 │ HTTP Proxy   │
                 │ (CONNECT)    │
                 └──────────────┘
```

## Компоненты

| Компонент | Технология | Назначение |
|-----------|-----------|------------|
| **WinDivert Capture** | C++, WinDivert API | Перехват TCP-пакетов на уровне LAYER_NETWORK, DST modification |
| **TcpRelayServer** | C++, Winsock | HTTP CONNECT туннели, SSPI/Kerberos auth, bidirectional bridge |
| **Windows Service** | C++, Win32 API | Управление жизненным циклом, IPC с GUI |
| **GUI** | C#, WPF, MVVM | Настройка, мониторинг, управление |

## Возможности

- ✅ Прозрачный перехват TCP через WinDivert (без WFP драйвера, без DLL Injection)
- ✅ HTTP CONNECT прокси с Basic-аутентификацией
- ✅ **Kerberos/Negotiate аутентификация через SSPI** (Windows Integrated Auth)
- ✅ DST modification — SYN-пакеты перенаправляются на локальный relay
- ✅ Правила: по имени процесса, по пути, глобальный режим
- ✅ Мониторинг соединений (PID, хост, порт, RX/TX, длительность)
- ✅ Управление сервисом (start/stop/restart/install/uninstall)
- ✅ Логирование с ротацией (INFO/DEBUG/TRACE)
- ✅ Консольный режим для отладки
- ✅ Безопасное хранение пароля (Windows DPAPI)

## Быстрый старт

### Установка сервиса

```batch
# 1. Скопируйте файлы в одну директорию:
#    TcpRedirectorService.exe + WinDivert.dll + WinDivert64.sys

# 2. Установите сервис (от имени Администратора)
TcpRedirectorService.exe --install

# 3. Запустите сервис
net start TcpRedirectorService
```

### Консольный режим (для отладки)

```batch
TcpRedirectorService.exe --console
```

### Полное описание установки

Подробная пошаговая инструкция: [INSTALL_GUIDE.md](docs/INSTALL_GUIDE.md)

## Документация

- [Архитектура](docs/01_ARCHITECTURE_OVERVIEW.md)
- [Windows Service (актуальная архитектура)](docs/03_WINDOWS_SERVICE.md)
- [Установка на чистую Windows](docs/INSTALL_GUIDE.md)
- [Конфигурация](docs/CONFIG_DESIGN.md)
- [Логирование](docs/LOGGER_DESIGN.md)
- [Статистика](docs/STATS_DESIGN.md)
- [GUI](docs/04_GUI.md)
- [Connection Flow](docs/05_CONNECTION_FLOW.md)
- [Risks & Limitations](docs/07_RISKS_AND_LIMITATIONS.md)
- [Repository Structure](docs/08_REPOSITORY_STRUCTURE.md)

## Сборка из исходников

### Требования

- Visual Studio 2022 (MSVC v143)
- Windows SDK 10.0.26100+
- WinDivert 2.2.2-A (x64)

### Сборка

```bash
msbuild src\service\TcpRedirectorService\TcpRedirectorService.vcxproj /p:Configuration=Release /p:Platform=x64
```

### Структура репозитория

```
TcpRedirector/
├── src/
│   └── service/TcpRedirectorService/     # Основной проект
│       ├── adapters/                     # Адаптеры (driving/driven)
│       ├── domain/                       # Доменная модель (entities/ports/services)
│       └── infrastructure/              # Инфраструктура (auth/capture/config/ipc/logging/relay/stats)
├── tests/                                # Тесты
├── docs/                                 # Документация
├── external/                             # Внешние зависимости (ProxyBridge, WinDivert)
└── gui/                                  # WPF GUI (C#)
```

---

**Разработчик:** Kalikin Iliya

**Лицензия:** MIT