# План реализации

## Этап 0: Подготовка окружения (1-2 недели)

### Задачи

1. **Установка инструментов**
   - Visual Studio 2022 with Desktop development with C++
   - WDK для Windows 10/11 (10.0.26100.0+)
   - Windows SDK (10.0.26100.0+)
   - .NET 8 SDK
   - git

2. **Настройка репозитория**
   - Создать структуру директорий
   - Настроить .gitignore (C++, C#, WDK)
   - Создать CMakeLists.txt для драйвера (или .vcxproj)
   - Создать Solution файлы для Service и GUI

3. **Изучение референсов**
   - Изучить WFP Redirect sample из WDK:
     `\Program Files (x86)\Windows Kits\10\Samples\...\Network\wfp\proxyshim`
   - Изучить `FwpsRedirectHandleCreate0` документацию
   - Изучить примеры драйверов WFP из WDK

4. **Создание тестового WFP драйвера**
   - Базовый DriverEntry/DriverUnload
   - Регистрация callout на ALE_AUTH_CONNECT
   - Простой classify: FWP_ACTION_PERMIT (все пропускать)

### Результат этапа

- Работающий стенд для разработки
- Базовый драйвер, который загружается и выгружается
- Верификация: Driver Verifier проходит

---

## Этап 1: WFP Driver — MVP (3-4 недели)

### Задачи

1. **Driver Infra**
   - [`driver.c`](src/driver/TcpRedirectorDriver/driver.c): DriverEntry, DriverUnload
   - Создание Device Object: `\Device\TcpRedirectorDriver`
   - Символическая ссылка: `\DosDevices\TcpRedirectorDriver`
   - Обработка IRP_MJ_CREATE, IRP_MJ_CLOSE, IRP_MJ_DEVICE_CONTROL

2. **WFP Callout Registration**
   - [`redirect_callout.c`](src/driver/TcpRedirectorDriver/callouts/redirect_callout.c): регистрация callout
   - GUID: `TCP_REDIRECT_CALLOUT_GUID`
   - Слой: `FWPM_LAYER_ALE_AUTH_CONNECT_V4`
   - classifyFn: TcpRedirectClassify (базовая — только логгирование)
   - notifyFn: TcpRedirectNotify
   - flowDeleteFn: TcpRedirectFlowDelete

3. **Process Info**
   - [`process_info.c`](src/driver/TcpRedirectorDriver/common/process_info.c):
   - Извлечение PID из `inMetaValues->remoteId`
   - `PsGetProcessImageFileName()` — имя процесса
   - `SeLocateProcessImageName()` — полный путь

4. **Connection Redirect**
   - `FwpsRedirectHandleCreate0()` — создание redirect handle
   - `FwpsAcquireWritableLayerDataPointer0()` — модификация данных слоя
   - Установка `FWP_ALE_FLAG_REDIRECT_TCP_CONNECTION`
   - `FwpsApplyModifiedLayerData0()` — применение
   - Сохранение оригинального адреса в REDIRECT_INFO

5. **Redirect Queue**
   - [`redirect_queue.c`](src/driver/TcpRedirectorDriver/common/redirect_queue.c):
   - Структура: список REDIRECT_INFO
   - Spin lock для синхронизации
   - InsertTail / RemoveHead
   - Максимум 4096 элементов

6. **Kernel↔User Communication**
   - [`device_io.c`](src/driver/TcpRedirectorDriver/communication/device_io.c):
   - IOCTL: `IOCTL_REDIRECTOR_GET_PENDING`
   - IOCTL: `IOCTL_REDIRECTOR_ACK_REDIRECT`
   - Event: `KeSetEvent` для уведомлений
   - IOCTL: `IOCTL_REDIRECTOR_SET_RULES` (кэш правил)

7. **Driver Test Utility**
   - Консольная утилита на C++ (user-mode)
   - Открытие `\\.\TcpRedirectorDriver`
   - Получение событий редиректа
   - Вывод информации о редиректах в консоль

### Критерии готовности MVP драйвера

- Драйвер загружается, callout зарегистрирован
- Chrome.exe → connect() перехватывается
- Драйвер определяет PID, имя, путь процесса
- Драйвер выполняет редирект на локальный порт
- Информация о редиректе доступна user-mode утилите
- Драйвер проходит проверки Driver Verifier

---

## Этап 2: Windows Service + Proxy Engine — MVP (3-4 недели)

### Задачи

1. **Service Bootstrap**
   - [`ServiceMain.cpp`](src/service/TcpRedirectorService/core/ServiceMain.cpp):
   - RegisterServiceCtrlHandlerEx
   - SetServiceStatus
   - Skeleton service with start/stop

2. **Driver Communicator**
   - [`DriverCommunicator.cpp`](src/service/TcpRedirectorService/communication/DriverCommunicator.cpp):
   - Open driver device
   - IOCTL send/receive
   - Event waiting loop
   - Thread-safe access to redirect queue

3. **Proxy Engine — Core**
   - [`ProxyEngine.cpp`](src/service/TcpRedirectorService/proxy/ProxyEngine.cpp):
   - Boost.Asio io_context
   - TCP resolver + TCP socket
   - Proxy connection management
   - Session pool

4. **Proxy Session**
   - [`ProxySession.cpp`](src/service/TcpRedirectorService/proxy/ProxySession.cpp):
   - ResolveProxy → Connect → Send CONNECT
   - Parse CONNECT response (HTTP/1.1 200)
   - Bidirectional bridging (async_read → async_write loop)
   - Error handling, timeouts

5. **HTTP Proxy Auth**
   - Basic authentication: `Proxy-Authorization: Basic base64(login:password)`
   - Base64 encoding

6. **Config Manager**
   - [`ConfigManager.cpp`](src/service/TcpRedirectorService/config/ConfigManager.cpp):
   - JSON config file: `%ProgramData%\TcpRedirector\config.json`
   - ProxyConfig (host, port, login)
   - Password via DPAPI (encrypted blob in config)

7. **Simple Rules Engine**
   - [`RuleEngine.cpp`](src/service/TcpRedirectorService/rules/RuleEngine.cpp):
   - Process name matching (exact + wildcard)
   - Process path matching
   - Global rule (*)
   - Priority-ordered matching

8. **Logging**
   - spdlog setup
   - Log levels: INFO, DEBUG, TRACE
   - Console + file sinks
   - Log rotation (size-based)

9. **Integration Test**
   - Full pipeline test:
   - Start service → load driver → configure proxy → test with chrome.exe
   - Verify CONNECT is sent to proxy
   - Verify data flows through tunnel

### Критерии готовности MVP сервиса

- Сервис стартует, загружает драйвер
- Сервис получает события редиректа от драйвера
- Proxy Engine подключается к прокси и выполняет CONNECT
- Трафик приложения проходит через HTTP прокси
- Правила работают (по имени процесса и глобальное)
- Логи пишутся с правильными уровнями
- Пароль хранится в DPAPI

---

## Этап 3: GUI — MVP (2-3 недели)

### Задачи

1. **GUI Bootstrap**
   - WPF проект
   - MainWindow с TabControl
   - ShellViewModel с навигацией

2. **IPC Client**
   - Named pipe клиент
   - Подключение к `\\.\pipe\TcpRedirectorService`
   - JSON message protocol
   - Push listeners (connections, logs, stats)

3. **Proxy Settings View**
   - Host, Port, Login, Password поля
   - Save command → IPC to service
   - Test Connection button

4. **Rules View**
   - DataGrid для списка правил
   - Add/Edit/Delete
   - Drag-drop reorder (опционально)
   - Enable/disable toggle

5. **Connections View**
   - DataGrid с реальным обновлением
   - Columns: PID, Process, Destination, Port, Duration, RX/TX
   - Filter by process name
   - Connection details panel

6. **Logs View**
   - ListBox/ListView с виртуализацией
   - Level filter (INFO/DEBUG/TRACE)
   - Search box
   - Auto-scroll
   - Color coding by log level

7. **Service Control View**
   - Start/Stop/Restart buttons
   - Status display
   - Statistics: total connections, active, RX/TX
   - Simple chart (connections over time)

8. **Status Bar**
   - Service status indicator
   - Active connections count
   - RX/TX totals

### Критерии готовности MVP GUI

- Все 5 вкладок работают
- IPC соединение стабильно
- Настройки сохраняются через сервис
- Правила добавляются/редактируются/удаляются
- Соединения отображаются в реальном времени
- Логи отображаются с фильтрацией
- Сервис запускается/останавливается из GUI

---

## Этап 4: Production — от MVP к релизу (4-6 недель)

### Задачи

1. **IPv6 поддержка**
   - Callout на `FWPM_LAYER_ALE_AUTH_CONNECT_V6`
   - REDIRECT_INFO для IPv6 (16-byte address)
   - Proxy Engine IPv6 resolution

2. **Производительность**
   - Оптимизация callout (минимизация работы в kernel)
   - Пул соединений Proxy Engine
   - Увеличение буферов до 128KB
   - Настройка TCP_NODELAY, SO_RCVBUF, SO_SNDBUF
   - Тюнинг числа потоков io_context

3. **Надёжность**
   - Watchdog: проверка состояния драйвера
   - Auto-recovery: перезагрузка драйвера при падении
   - Graceful shutdown: закрытие всех туннелей при остановке сервиса
   - Crash dump analysis support

4. **Обработка edge cases**
   - Resolver failure (DNS для прокси не разрешается)
   - Partial HTTP CONNECT response (chunked?)
   - Non-standard proxy responses
   - Connection reset during CONNECT
   - Multiple simultaneous connections from same process
   - Very long-lived connections

5. **Дополнительные типы правил**
   - Rule by destination IP range (CIDR)
   - Rule by destination port range
   - Rule by destination domain (DNS-based)
   - Composite rules (AND/OR conditions)

6. **Log rotation**
   - Size-based: 50MB per file
   - Count-based: 10 files max
   - Age-based: 30 days retention
   - Async file sink (spdlog)

7. **Инсталлятор**
   - WiX Toolset или MSIX
   - Установка драйвера (devcon или setup action)
   - Установка сервиса
   - Сертификат драйвера
   - Start menu shortcut

8. **Тестирование**
   - Unit tests: RuleEngine, HttpConnectParser
   - Integration tests: full pipeline with test proxy
   - Stress tests: 1000 simultaneous connections
   - Memory leak tests (Driver Verifier + leak detection)
   - Proxy compatibility: Squid, HAProxy, NGINX, mitmproxy

---

## Этап 5: Полировка и стабилизация (2-3 недели)

### Задачи

1. **Безопасность**
   - Audit пароля — не логируется, не кэшируется
   - Named pipe DACL (только администраторы)
   - Driver security: проверка входных IOCTL буферов
   - Signed driver (WHQL)

2. **UI/UX**
   - Тёмная тема
   - Сохранение позиции/размера окна
   - Keyboard shortcuts
   - System tray icon
   - Minimize to tray
   - Auto-start with Windows

3. **Локализация**
   - en-US, ru-RU
   - Resource files (.resx)

4. **Документация**
   - README с архитектурой
   - User guide
   - Admin guide
   - API reference

---

## Timeline (суммарно)

| Этап | Длительность | Зависимости |
|------|-------------|-------------|
| 0. Подготовка | 1-2 нед | — |
| 1. WFP Driver MVP | 3-4 нед | Этап 0 |
| 2. Service + Proxy Engine MVP | 3-4 нед | Этап 1 |
| 3. GUI MVP | 2-3 нед | Этап 2 |
| 4. Production | 4-6 нед | Этап 1,2,3 |
| 5. Полировка | 2-3 нед | Этап 4 |
| **Итого** | **15-22 нед** | |

### Параллельная работа

- Этап 1 (Driver) и Этап 2 (Service) могут пересекаться частично
  - API контракт (IOCTL коды, REDIRECT_INFO структура) фиксируется на Этапе 0
  - Service может разрабатываться с mock-драйвером
- Этап 3 (GUI) не зависит от Этапа 1 (Driver), только от IPC протокола (Этап 2)
  - GUI может разрабатываться с mock-сервисом

## MVP vs Production

### MVP включает:
- [x] WFP Driver: ALE_AUTH_CONNECT V4, редирект, IOCTL
- [x] Service: загрузка драйвера, получение редиректов, Proxy Engine
- [x] Proxy Engine: CONNECT, Basic Auth, bridging
- [x] Rules: process name, process path, global
- [x] GUI: все 5 вкладок, IPC, базовый функционал
- [x] Logging: INFO, DEBUG, TRACE, file + console
- [x] Config: JSON, DPAPI для пароля
- [x] Proxy Config: host, port, login, password

### Production добавляет:
- [ ] IPv6 поддержка
- [ ] Оптимизация производительности (1000 conn)
- [ ] Watchdog и auto-recovery
- [ ] Дополнительные типы правил (CIDR, порты, DNS)
- [ ] Log rotation (полноценная)
- [ ] Инсталлятор
- [ ] Системный tray
- [ ] Тёмная тема
- [ ] Auto-start
- [ ] Полное тестирование