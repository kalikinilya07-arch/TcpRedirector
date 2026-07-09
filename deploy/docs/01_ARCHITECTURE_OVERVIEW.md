# Архитектура решения TcpRedirector

**Разработчик:** Kalikin Iliya

## 1. Общая архитектура

Система состоит из четырёх основных компонентов, взаимодействующих последовательно:

```
┌─────────────────────────────────────────────────────────────┐
│                         GUI (WPF C#)                        │
│                   TcpRedirectorGUI.exe                      │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌────────────────┐ │
│  │Proxy     │ │ Rules    │ │Connections│ │ Service Control│ │
│  │Settings  │ │ Editor   │ │ Monitor   │ │  (Start/Stop)  │ │
│  └──────────┘ └──────────┘ └──────────┘ └────────────────┘ │
└──────────────────────┬──────────────────────────────────────┘
                       │ Named Pipe (IPC)
                       ▼
┌─────────────────────────────────────────────────────────────┐
│                   Windows Service (C++)                      │
│                  TcpRedirectorService.exe                    │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌────────────────┐ │
│  │ Config   │ │ Rules    │ │WinDivert │ │ TcpRelayServer │ │
│  │ Manager  │ │ Engine   │ │ Capture  │ │ (HTTP CONNECT) │ │
│  └──────────┘ └──────────┘ └──────────┘ └────────────────┘ │
│  ┌──────────┐ ┌──────────┐ ┌──────────────────────────┐     │
│  │Connection│ │ Connection│ │      Logger              │     │
│  │Tracker   │ │ Table     │ │  (Async + Ring Buffer)   │     │
│  └──────────┘ └──────────┘ └──────────────────────────┘     │
└──────────────────────┬──────────────────────────────────────┘
                       │ WinDivert API
                       ▼
┌─────────────────────────────────────────────────────────────┐
│              WinDivert (драйвер захвата пакетов)             │
│                 WinDivert64.sys + WinDivert.dll              │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  LAYER_NETWORK — перехват всех IP-пакетов            │   │
│  │  DST modification — SYN → 127.0.0.1:relayPort       │   │
│  └──────────────────────────────────────────────────────┘   │
└──────────────────────┬──────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────┐
│                 Пользовательские приложения                   │
│              (chrome.exe, java.exe, и т.д.)                  │
└─────────────────────────────────────────────────────────────┘
```

## 2. Принцип работы

1. **Перехват**: WinDivert перехватывает все исходящие TCP-пакеты на слое `WINDIVERT_LAYER_NETWORK`.

2. **Определение процесса**: Для каждого SYN-пакета определяется PID через `GetExtendedTcpTable` (маппинг source_port → PID).

3. **Проверка правил**: PID проверяется по `RuleEngine` — если процесс подпадает под правило с действием `Proxy`, то SYN-пакет модифицируется.

4. **DST modification**: Целевой IP и порт в SYN-пакете заменяются на `127.0.0.1:relayPort` (34010). Оригинальный адрес сохраняется в `ConnectionTable`.

5. **Relay-сервер**: `TcpRelayServer` принимает соединение на relayPort, достаёт оригинальный адрес из таблицы, подключается к HTTP-прокси и отправляет `CONNECT original_host:original_port HTTP/1.1`.

6. **Туннель**: После получения `200 Connection Established` от прокси, relay-сервер начинает двустороннюю пересылку данных (bridge) между клиентом и прокси.

7. **Мониторинг**: Сервис отслеживает состояние соединений, собирает статистику (RX/TX bytes, длительность) и передаёт в GUI.

## 3. Схема потоков данных

```
Приложение              WinDivert           TcpRelayServer          HTTP Proxy
(chrome.exe)            (kernel)            (user-mode)             (remote)
     │                      │                    │                      │
     │──TCP SYN──────────►  │                    │                      │
     │   (example.com:443)  │                    │                      │
     │                      │──Find PID by─────  │                      │
     │                      │   src_port         │                      │
     │                      │──Check rules─────  │                      │
     │                      │──Modify DST──────  │                      │
     │                      │   127.0.0.1:34010  │                      │
     │                      │──WinDivertSend───  │                      │
     │                      │                    │                      │
     │◄────SYN-ACK────────  │                    │                      │
     │───ACK────────────────►│                    │                      │
     │ (TCP handshake to    │                    │                      │
     │  127.0.0.1:34010)    │                    │                      │
     │                      │                    │                      │
     │                      │                    │──accept────────      │
     │                      │                    │──table.Lookup()────  │
     │                      │                    │   srcPort→orig dst   │
     │                      │                    │                      │
     │                      │                    │──TCP connect────────►│
     │                      │                    │   to proxy:port      │
     │                      │                    │──CONNECT────────────►│
     │                      │                    │   example.com:443    │
     │                      │                    │◄──200 OK──────────── │
     │                      │                    │                      │
     │                      │                    │──Bridge established  │
     │◄══data═══════════════►│◄══════════════════►════data═════════════►│
```

## 4. Компоненты и их ответственность

### 4.1 WinDivert Capture (WinDivertCapture)
- Загрузка WinDivert.dll (динамическая через LoadLibrary)
- Открытие WinDivert handle с фильтром "true" (все пакеты)
- Цикл захвата: Recv → Parse → PID lookup → DST modify → Send
- Per-port bitmap для кэширования PID-решений
- Обработка SYN/RST/FIN для отслеживания соединений

### 4.2 Windows Service (TcpRedirectorService.exe)
- Инициализация всех компонентов (ConfigManager, Logger, Relay, Capture)
- Управление правилами маршрутизации (RuleEngine)
- Управление конфигурацией прокси (ConfigManager)
- IPC с GUI через Named Pipe
- Логирование (асинхронный Logger с ротацией)

### 4.3 TcpRelayServer (HTTP CONNECT relay)
- Приём TCP-соединений на relayPort (34010)
- Определение оригинального адреса из ConnectionTable
- HTTP CONNECT к прокси (Basic или Kerberos/Negotiate auth)
- Двусторонняя пересылка данных (bridge mode)
- SSPI-аутентификация (Negotiate/Kerberos) через auth_sspi

### 4.4 GUI (TcpRedirectorGUI.exe)
- Настройка параметров прокси
- Редактор правил маршрутизации
- Мониторинг активных соединений в реальном времени
- Просмотр логов с фильтрацией
- Управление сервисом (start/stop/restart)

## 5. Технологический стек

| Компонент | Технология | Версия |
|-----------|-----------|--------|
| Захват пакетов | WinDivert | 2.2.2-A |
| Сервис | C++20 / MSVC | Visual Studio 2022 |
| JSON | nlohmann/json | 3.11+ |
| Авторизация | Windows SSPI | secur32.lib |
| Шифрование пароля | Windows DPAPI | CryptProtectData |
| IPC (Service↔GUI) | Named Pipe | Windows API |
| GUI | C# / WPF / .NET | 8.0+ |
| MVVM Toolkit | CommunityToolkit.Mvvm | 8.x |

## 6. Поддерживаемые платформы

| Платформа | Поддержка |
|-----------|-----------|
| Windows 10 x64 (22H2+) | ✅ Full |
| Windows 11 x64 (24H2+) | ✅ Full |
| Windows Server 2022 | ✅ Full |
| Windows 10 ARM64 | ❌ (x64 only) |

## 7. Ограничения архитектуры

1. **Только TCP** — UDP/QUIC не перехватываются
2. **Только IPv4** — IPv6 поддержка отсутствует
3. **Только HTTP CONNECT Proxy** — SOCKS не поддерживается
4. **Один прокси-сервер** — без цепочек и балансировки
5. **Нет fallback** — при недоступности прокси соединения блокируются
6. **Нет TLS MITM** — трафик проходит туннель без расшифровки
7. **Требуются права администратора** — для загрузки драйвера WinDivert
8. **WinDivert.dll и WinDivert64.sys** — должны находиться рядом с exe-файлом
9. **DST modification** — модифицируется только SYN-пакет, все остальные пакеты проходят прозрачно