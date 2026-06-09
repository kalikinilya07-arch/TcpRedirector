# TcpRedirector

Прозрачное перенаправление TCP-соединений через HTTP Proxy (CONNECT) на Windows 10/11 x64.

## Архитектура

```
┌──────────┐     ┌──────────────┐     ┌──────────────┐     ┌──────────┐
│   GUI    │────▶│   Service    │◀───▶│ WFP Driver   │◀───▶│  Apps    │
│ (WPF C#) │     │ (C++/Asio)  │     │ (C++/WDK)    │     │(chrome..)│
└──────────┘     └──────┬───────┘     └──────────────┘     └──────────┘
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
| **WFP Driver** | C++, WDK, WFP ALE | Перехват TCP-соединений через ALE_AUTH_CONNECT + Connection Redirect |
| **Windows Service** | C++, Boost.Asio, spdlog | Управление драйвером, Proxy Engine, правила, IPC с GUI |
| **Proxy Engine** | C++, Boost.Asio | HTTP CONNECT туннели, двусторонняя пересылка данных |
| **GUI** | C#, WPF, MVVM | Настройка, мониторинг, управление |

## Возможности

- ✅ Прозрачный перехват TCP через WFP (без DLL Injection, без API Hooking)
- ✅ HTTP CONNECT прокси с Basic-аутентификацией
- ✅ Правила: по имени процесса, по пути, глобальный режим
- ✅ Мониторинг соединений в реальном времени (PID, хост, порт, RX/TX, длительность)
- ✅ Управление сервисом (start/stop/restart)
- ✅ Логирование с ротацией (INFO/DEBUG/TRACE)
- ✅ Безопасное хранение пароля (Windows DPAPI)
- ✅ IPv4 и IPv6
- ✅ Производительность: 1000+ соединений, <5% CPU, <200 MB RAM

## Быстрый старт

```bash
# 1. Включить тестовую подпись драйвера (на машине разработки)
tools\scripts\enable_testsigning.cmd
# Перезагрузить ПК

# 2. Собрать все компоненты
tools\scripts\build_all.cmd

# 3. Установить драйвер
tools\scripts\install_driver.cmd

# 4. Установить сервис
tools\scripts\install_service.cmd

# 5. Запустить GUI
build\gui\Release\TcpRedirectorGUI.exe
```

## Документация

- [Архитектура](docs/01_ARCHITECTURE_OVERVIEW.md)
- [WFP Driver](docs/02_WFP_DRIVER.md)
- [Windows Service](docs/03_WINDOWS_SERVICE.md)
- [GUI](docs/04_GUI.md)
- [Connection Flow](docs/05_CONNECTION_FLOW.md)
- [Implementation Plan](docs/06_IMPLEMENTATION_PLAN.md)
- [Risks & Limitations](docs/07_RISKS_AND_LIMITATIONS.md)
- [Repository Structure](docs/08_REPOSITORY_STRUCTURE.md)

## Лицензия

MIT