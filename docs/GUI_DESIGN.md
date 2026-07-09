# GUI TcpRedirector

## Архитектура

WPF приложение на .NET 9.0, паттерн MVVM через CommunityToolkit.Mvvm.

### ViewModels

```
┌─────────────────────────────────────────────────────────┐
│ MainWindow                                              │
│ ┌───────────────────────┬─────────────────────────────┐ │
│ │ Левая панель           │ Правая панель                │ │
│ │ ┌───────────────────┐  │ ┌─────────────────────────┐ │ │
│ │ │ ShellViewModel    │  │ │ SettingsViewModel       │ │ │
│ │ │ - Start/Stop      │  │ │ - Proxy config          │ │ │
│ │ │ - Status          │  │ │ - Auth (Basic/Kerberos) │ │ │
│ │ │ - Traffic graph   │  │ │ - Rules                 │ │ │
│ │ └───────────────────┘  │ │ - Log level             │ │ │
│ │                        │ └─────────────────────────┘ │ │
│ │ ┌───────────────────┐  │ ┌─────────────────────────┐ │ │
│ │ │ StatsViewModel    │  │ │ Logs (RichTextBox)      │ │ │
│ │ │ - Connections     │  │ │                         │ │ │
│ │ │ - Bytes RX/TX     │  │ │                         │ │ │
│ │ │ - Uptime          │  │ │                         │ │ │
│ │ └───────────────────┘  │ └─────────────────────────┘ │ │
│ └───────────────────────┴─────────────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

### ShellViewModel

Файл: [`ShellViewModel.cs`](TcpRedirector/src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/ShellViewModel.cs)

- **AutoStartAndConnect**: при запуске находит `TcpRedirectorService.exe`, запускает в `--console` режиме, подключается через IPC
- **PollLoop**: таймер 2 секунды → `GetStatsAsync()` + `GetServiceStatusAsync()`
- **StartService/StopService**: управление через `ServiceController`

### SettingsViewModel

Файл: [`SettingsViewModel.cs`](TcpRedirector/src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/SettingsViewModel.cs)

- **LoadFromConfig**: читает `config.json` через `JsonConfigRepository`
- **SaveAsync**: пишет `config.json` + опционально синхронизирует с сервисом через IPC
- **Kerberos ↔ AuthRequired**: взаимоисключающие (реализовано через partial methods)

### StatsViewModel

Файл: [`StatsViewModel.cs`](TcpRedirector/src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/StatsViewModel.cs)

- **PushStats**: получает `ServiceStats` из ShellViewModel и обновляет UI
- **TrafficGraph**: кастомный WPF control для графика трафика (Rx/Tx)

### IPC

Файл: [`IpcClient.cs`](TcpRedirector/src/gui/TcpRedirectorGUI/Infrastructure/Ipc/IpcClient.cs)

- **Транспорт**: TCP socket → `localhost:34011`
- **Протокол**: JSON-RPC style — `{"method":"...", "params":{...}}` → `{"status":"success", "data":{...}}`
- Методы: `get_config`, `set_config`, `get_rules`, `set_rules`, `get_connections`, `get_logs`, `get_stats`, `get_service_status`, `set_log_level`

### ServiceController

Файл: [`ServiceController.cs`](TcpRedirector/src/gui/TcpRedirectorGUI/Infrastructure/Scm/ServiceController.cs)

- Управление Windows-сервисом через SCM (`StartServiceW`, `StopServiceW`)
- `IsAdministrator()` — проверка прав
- `KillServiceProcess()` — force kill через `GetProcessesByName`

---

## Конфигурация (JsonConfigRepository)

Файл: [`JsonConfigRepository.cs`](TcpRedirector/src/gui/TcpRedirectorGUI/Infrastructure/Config/JsonConfigRepository.cs)

- Чтение/запись `config.json`
- `WriteFull` — атомарная запись (temp → rename)
- `ReadRules` / `ReadString` / `ReadBool` / `ReadInt` — парсинг JSON