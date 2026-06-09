# TcpRedirector — Инструкция по запуску и тестированию

## 1. Системные требования

| Компонент | Требование |
|-----------|-----------|
| ОС | Windows 10/11 x64 (22H2+) |
| Права | Администратор (для загрузки драйвера и установки сервиса) |
| Память | 200 MB RAM |
| Диск | 50 MB |
| Подпись драйвера | Test signing mode для разработки |

**Важно:** Для продакшена требуется подписанный драйвер (EV-сертификат + Hardware Dev Center).

---

## 2. Быстрый старт (разработка)

### Шаг 1: Включение тестовой подписи драйверов

```cmd
# Запустить от имени Администратора!
bcdedit /set testsigning on

# Перезагрузить компьютер
shutdown /r /t 0
```

### Шаг 2: Установка инструментов

1. **Visual Studio 2022** — `Desktop development with C++`, `.NET desktop development`
2. **Windows Driver Kit (WDK) 10.0.26100+** — скачать из Microsoft
3. **.NET 8 SDK** — для GUI
4. **Git** — для управления версиями

### Шаг 3: Клонирование и сборка

```cmd
# Открыть "Developer Command Prompt for VS 2022" от Администратора

# Перейти в папку проекта
cd C:\Users\user\Desktop\TcpRedirector

# Полная сборка
tools\scripts\build_all.cmd
```

### Шаг 4: Установка компонентов

```cmd
# Установка сервиса (требует админских прав)
tools\scripts\install_service.cmd

# Запуск сервиса
net start TcpRedirectorService
```

### Шаг 5: Запуск GUI

```cmd
# Запустить GUI
start src\gui\TcpRedirectorGUI\bin\Release\net8.0-windows\TcpRedirectorGUI.exe
```

---

## 3. Тестирование

### 3.1 Модульные тесты (Service)

```cmd
# Компиляция и запуск тестов RuleEngine
cl /EHsc /Fe:RuleEngineTest.exe ^
    tests\unit\service\RuleEngineTest.cpp ^
    /I src\service\TcpRedirectorService

RuleEngineTest.exe
```

**Ожидаемый результат:**
```
=== TcpRedirector Service Unit Tests ===

Test: WildcardMatch...       PASS
Test: PathRule...            PASS
Test: GlobalRule...          PASS
Test: PriorityOrder...       PASS
Test: DisabledRule...        PASS
Test: RemoveRule...          PASS

All tests passed!
```

### 3.2 Тестирование драйвера (вручную)

```cmd
# 1. Включить тестовую подпись (см. выше)

# 2. Установить драйвер
tools\scripts\install_driver.cmd

# 3. Проверить загрузку
sc query TcpRedirectorDriver

# 4. Просмотр событий драйвера
# Использовать DbgView (Sysinternals) для просмотра DbgPrint

# 5. Тест с утилитой
tools\TcpRedirectorDriverUtil\Debug\TcpRedirectorDriverUtil.exe --monitor
```

### 3.3 Интеграционное тестирование

Для интеграционного теста потребуется тестовый HTTP Proxy:

```cmd
# 1. Запустить тестовый прокси (например, mitmproxy)
mitmproxy --listen-port 3128

# 2. Настроить TcpRedirector на localhost:3128

# 3. Запустить сервис
net start TcpRedirectorService

# 4. Открыть браузер (Chrome/Firefox)
# Должен появиться CONNECT-запрос в mitmproxy

# 5. Проверить GUI - соединения должны отображаться
```

### 3.4 Нагрузочное тестирование

```cmd
# Использовать тестовый инструмент
tests\stress\StressTest.exe --connections 1000 --duration 60

# Мониторинг:
# - Performance Monitor: \TcpRedirector\*
# - Task Manager: CPU < 5%, RAM < 200 MB
```

---

## 4. Структура проекта после реализации

```
TcpRedirector/
│
├── docs/                           # 8 документов архитектуры
│
├── src/
│   ├── driver/TcpRedirectorDriver/ # WFP драйвер (C, C++, WDK)
│   │   ├── domain/entities/        # RedirectInfo, RuleEntry
│   │   ├── domain/ports/           # IRedirectHandler
│   │   ├── domain/services/        # RedirectService
│   │   ├── infrastructure/wfp/     # WfpCallout (ALE_AUTH_CONNECT)
│   │   ├── infrastructure/ioctl/   # IoctlHandler
│   │   └── adapters/driving/       # DriverEntry
│   │
│   ├── service/TcpRedirectorService/ # Windows Service (C++)
│   │   ├── domain/entities/         # ProxyConfig, Rule, ConnectionRecord
│   │   ├── domain/ports/            # IDriverCommunicator, IProxyConnector, IConfigStore
│   │   ├── domain/services/         # RuleEngine, ConnectionTracker
│   │   ├── infrastructure/driver/   # DriverCommunicator (IOCTL)
│   │   ├── infrastructure/ipc/      # PipeServer
│   │   ├── infrastructure/config/   # ConfigManager, SecretsManager (DPAPI)
│   │   ├── infrastructure/logging/  # Logger (spdlog)
│   │   ├── adapters/driven/         # ProxyEngine (Boost.Asio)
│   │   └── adapters/driving/        # ServiceMain, main.cpp
│   │
│   └── gui/TcpRedirectorGUI/        # GUI (C#, WPF)
│       ├── Domain/Entities/         # ProxyConfig, Rule, ConnectionRecord
│       ├── Domain/Ports/            # ITcpRedirectorService, IServiceController
│       ├── Infrastructure/Ipc/      # IpcClient (Named Pipe)
│       ├── Infrastructure/Scm/      # ServiceController
│       └── Adapters/Driving/Wpf/    # Views, ViewModels, Styles
│
├── tests/
│   ├── unit/service/                # RuleEngineTest.cpp
│   ├── integration/                 # Full pipeline tests
│   └── stress/                      # Load testing
│
├── tools/scripts/                   # build_all, install_service
└── build/                           # Build outputs
```

---

## 5. Гексагональная архитектура

Проект строго следует принципам гексагональной архитектуры (Ports & Adapters):

```
                    ┌──────────────┐
                    │   GUI (WPF)  │
                    │   Driving    │
                    │   Adapter    │
                    └──────┬───────┘
                           │ ITcpRedirectorService (Port)
                           ▼
┌──────────────────────────────────────────┐
│              DOMAIN LAYER                │
│  ┌────────────────────────────────────┐  │
│  │  Entities: ProxyConfig, Rule, ...   │  │
│  │  Ports: ITcpRedirectorService      │  │
│  │  Services: (thin, delegated to     │  │
│  │            infrastructure)          │  │
│  └────────────────────────────────────┘  │
└──────────────────────────────────────────┘
      ▲                              ▲
      │ IProxyConnector (Port)      │ IConfigStore (Port)
      ▼                              ▼
┌──────────────┐            ┌──────────────┐
│ ProxyEngine  │            │ ConfigManager│
│ Driven       │            │ Driven       │
│ Adapter      │            │ Adapter      │
└──────────────┘            └──────────────┘

▲ IDriverCommunicator (Port)
▼
┌──────────────┐
│ DriverComm   │
│ Driven       │
│ Adapter      │
└──────────────┘
      │ IOCTL
      ▼
┌──────────────┐
│ WFP Driver   │
│ (kernel)     │
└──────────────┘
```

---

## 6. Проверка работоспособности

### Check-list перед сдачей

- [ ] **Driver Verifier**: драйвер проходит DDI compliance + Special Pool
- [ ] **Service**: стартует, стопается, перезапускается
- [ ] **Proxy Engine**: CONNECT туннель устанавливается
- [ ] **Rules**: chrome.exe → proxy, * → direct
- [ ] **GUI**: все 5 вкладок работают
- [ ] **IPC**: Named Pipe стабилен, push-уведомления работают
- [ ] **Config**: JSON + DPAPI, пароль не логируется
- [ ] **Logs**: INFO/DEBUG/TRACE, ротация
- [ ] **Performance**: 1000 соединений, CPU < 5%, RAM < 200 MB

---

## 7. Известные ограничения MVP

1. **Только IPv4** — IPv6 callout не зарегистрирован (будет в Production)
2. **Один прокси** — без цепочек
3. **Нет fallback** — при недоступности прокси трафик блокируется
4. **Только HTTP CONNECT** — без SOCKS
5. **Нет кэша правил в драйвере** — правила проверяются в user-mode
6. **Нет WebSocket счётчика** — используется общий bridging
7. **Требуется админ** — для загрузки драйвера
8. **Требуется тестовая подпись** — для разработки