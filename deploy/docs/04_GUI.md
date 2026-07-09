# GUI (WPF C#) — детальная архитектура

## 1. Технологический стек

| Компонент | Технология |
|-----------|------------|
| Платформа | .NET 8.0+ |
| Язык | C# 12 |
| UI | Windows Presentation Foundation (WPF) |
| MVVM Toolkit | CommunityToolkit.Mvvm |
| DI | Microsoft.Extensions.DependencyInjection |
| IPC Client | System.IO.Pipes (Named Pipe Client) |
| Serialization | System.Text.Json |
| Charts | LiveChartsCore (или OxyPlot) |
| DataGrid | System.Windows.Controls.DataGrid |

## 2. Структура проекта

```
src/gui/TcpRedirectorGUI/
├── App.xaml / App.xaml.cs           # Точка входа
├── MainWindow.xaml / .cs            # Главное окно (Shell)
├── appsettings.json                 # Настройки GUI
│
├── Models/
│   ├── ProxyConfig.cs               # Модель настроек прокси
│   ├── Rule.cs                      # Модель правила
│   ├── ConnectionRecord.cs          # Модель соединения
│   ├── LogEntry.cs                  # Модель записи лога
│   ├── ServiceStatus.cs             # Модель статуса сервиса
│   ├── ServiceStats.cs              # Модель статистики
│   └── IpcMessage.cs                # Модель IPC сообщения
│
├── ViewModels/
│   ├── ShellViewModel.cs            # Главная VM
│   ├── ProxySettingsViewModel.cs    # Настройки прокси
│   ├── RulesViewModel.cs            # Управление правилами
│   ├── ConnectionsViewModel.cs      # Мониторинг соединений
│   ├── LogsViewModel.cs             # Просмотр логов
│   └── ServiceControlViewModel.cs   # Управление сервисом
│
├── Views/
│   ├── ProxySettingsView.xaml / .cs
│   ├── RulesView.xaml / .cs
│   ├── ConnectionsView.xaml / .cs
│   ├── LogsView.xaml / .cs
│   └── ServiceControlView.xaml / .cs
│
├── Services/
│   ├── IpcClient.cs                 # Named Pipe клиент
│   ├── IIpcClient.cs                # Интерфейс клиента
│   ├── ServiceController.cs         # SCM управление
│   ├── IServiceController.cs        # Интерфейс управления
│   └── NotificationService.cs       # Push-уведомления
│
├── Converters/
│   ├── BoolToVisibilityConverter.cs
│   ├── ConnectionStateToColorConverter.cs
│   ├── LogLevelToColorConverter.cs
│   └── BytesToHumanReadableConverter.cs
│
├── Controls/
│   ├── ConnectionDetailsControl.xaml / .cs
│   ├── RuleEditorControl.xaml / .cs
│   └── StatisticsControl.xaml / .cs
│
├── Styles/
│   ├── AppTheme.xaml                # Тёмная/светлая тема
│   └── ControlStyles.xaml           # Стили элементов
│
└── TcpRedirectorGUI.csproj
```

## 3. Классы моделей

```csharp
// Models/ProxyConfig.cs
public class ProxyConfig
{
    public string Host { get; set; } = string.Empty;
    public int Port { get; set; } = 3128;
    public bool AuthRequired { get; set; }
    public bool HasPassword { get; set; }  // Пароль не передаётся в GUI
    // Пароль задаётся отдельным методом
}
```

```csharp
// Models/Rule.cs
public class Rule
{
    public string Id { get; set; } = Guid.NewGuid().ToString();
    public RuleType Type { get; set; }
    public RuleAction Action { get; set; } = RuleAction.Proxy;
    public string Pattern { get; set; } = string.Empty;
    public string Description { get; set; } = string.Empty;
    public int Priority { get; set; }
    public bool Enabled { get; set; } = true;
    public DateTime Created { get; set; } = DateTime.UtcNow;
    public DateTime Modified { get; set; } = DateTime.UtcNow;
}

public enum RuleType { ProcessName, ProcessPath, Global }
public enum RuleAction { Proxy, Direct, Block }
```

```csharp
// Models/ConnectionRecord.cs
public class ConnectionRecord
{
    public ulong Id { get; set; }
    public uint Pid { get; set; }
    public string ProcessName { get; set; } = string.Empty;
    public string ProcessPath { get; set; } = string.Empty;
    public string DestinationHost { get; set; } = string.Empty;
    public string DestinationIp { get; set; } = string.Empty;
    public ushort DestinationPort { get; set; }
    public DateTime StartTime { get; set; }
    public TimeSpan Duration { get; set; }
    public ulong RxBytes { get; set; }
    public ulong TxBytes { get; set; }
    public bool ProxyEnabled { get; set; }
    public ConnectionState State { get; set; }
}

public enum ConnectionState
{
    Redirecting,
    ConnectingToProxy,
    TunnelEstablished,
    Closing,
    Closed,
    Error
}
```

```csharp
// Models/ServiceStatus.cs
public class ServiceStatus
{
    public bool IsRunning { get; set; }
    public ServiceState State { get; set; }
    public string? ErrorMessage { get; set; }
}

public enum ServiceState { Stopped, Running, Starting, Stopping, Error }
```

## 4. ViewModels

```csharp
// ViewModels/ShellViewModel.cs
public partial class ShellViewModel : ObservableObject
{
    [ObservableProperty]
    private bool _isServiceRunning;

    [ObservableProperty]
    private string _serviceStatusText = "Stopped";

    [ObservableProperty]
    private string _activeView = "Connections";

    // Sub-ViewModels
    public ProxySettingsViewModel ProxySettings { get; }
    public RulesViewModel Rules { get; }
    public ConnectionsViewModel Connections { get; }
    public LogsViewModel Logs { get; }
    public ServiceControlViewModel ServiceControl { get; }

    // Navigation commands
    [RelayCommand]
    private void NavigateTo(string viewName) => ActiveView = viewName;

    // Periodic polling
    private Timer? _pollingTimer;  // 1 second interval
}
```

```csharp
// ViewModels/ProxySettingsViewModel.cs
public partial class ProxySettingsViewModel : ObservableObject
{
    [ObservableProperty]
    private string _host = string.Empty;

    [ObservableProperty]
    private int _port = 3128;

    [ObservableProperty]
    private bool _authRequired;

    [ObservableProperty]
    private string _login = string.Empty;

    [ObservableProperty]
    private string _password = string.Empty;  // Only for input, not sent back

    [ObservableProperty]
    private bool _isPasswordSet;

    [ObservableProperty]
    private bool _isSaving;

    [ObservableProperty]
    private string? _errorMessage;

    [RelayCommand]
    private async Task SaveAsync()
    {
        // Сохраняем пароль отдельным IPC-вызовом (DPAPI в сервисе)
        // Пароль не передаётся в ответах сервиса
    }

    [RelayCommand]
    private async Task TestConnectionAsync()
    {
        // Проверка: сервис пытается CONNECT к тестовому хосту
    }
}
```

```csharp
// ViewModels/RulesViewModel.cs
public partial class RulesViewModel : ObservableObject
{
    [ObservableProperty]
    private ObservableCollection<Rule> _rules = new();

    [ObservableProperty]
    private Rule? _selectedRule;

    [ObservableProperty]
    private bool _isEditing;

    [ObservableProperty]
    private string? _searchText;

    [RelayCommand]
    private async Task AddRuleAsync() { /* Open editor dialog */ }

    [RelayCommand]
    private async Task EditRuleAsync(Rule rule) { /* Open editor with rule */ }

    [RelayCommand]
    private async Task DeleteRuleAsync(Rule rule) { /* Confirm + delete */ }

    [RelayCommand]
    private async Task MoveUpAsync(Rule rule) { /* Reorder */ }

    [RelayCommand]
    private async Task MoveDownAsync(Rule rule) { /* Reorder */ }

    [RelayCommand]
    private async Task ToggleRuleAsync(Rule rule) { /* Enable/disable */ }
}
```

```csharp
// ViewModels/ConnectionsViewModel.cs
public partial class ConnectionsViewModel : ObservableObject
{
    [ObservableProperty]
    private ObservableCollection<ConnectionRecord> _connections = new();

    [ObservableProperty]
    private string? _filterText;

    [ObservableProperty]
    private string? _selectedProcessFilter;

    [ObservableProperty]
    private ConnectionRecord? _selectedConnection;

    [ObservableProperty]
    private ConnectionDetailsViewModel? _selectedConnectionDetails;

    // Live updates from service via IPC push
    public void UpdateConnections(List<ConnectionRecord> updatedConnections)
    {
        // Batch update to avoid UI flicker
    }
}
```

```csharp
// ViewModels/LogsViewModel.cs
public partial class LogsViewModel : ObservableObject
{
    [ObservableProperty]
    private ObservableCollection<LogEntry> _logEntries = new();

    [ObservableProperty]
    private LogLevel _minLevel = LogLevel.INFO;

    [ObservableProperty]
    private string? _searchText;

    [ObservableProperty]
    private bool _autoScroll = true;

    // Virtualization for large log sets
    // Only show last 10000 entries in UI
    private const int MaxVisibleEntries = 10000;
}
```

```csharp
// ViewModels/ServiceControlViewModel.cs
public partial class ServiceControlViewModel : ObservableObject
{
    [ObservableProperty]
    private ServiceState _currentState = ServiceState.Stopped;

    [ObservableProperty]
    private string? _errorMessage;

    [ObservableProperty]
    private ServiceStats _stats = new();

    [RelayCommand]
    private async Task StartServiceAsync() { /* SCM start */ }

    [RelayCommand]
    private async Task StopServiceAsync() { /* SCM stop */ }

    [RelayCommand]
    private async Task RestartServiceAsync() { /* SCM restart */ }

    [RelayCommand]
    private async Task RefreshStatusAsync() { /* Poll status */ }
}
```

## 5. IPC Client (GUI → Service)

```csharp
// Services/IpcClient.cs
public class IpcClient : IIpcClient, IDisposable
{
    private NamedPipeClientStream? _pipeClient;
    private readonly string _pipeName = "TcpRedirectorService";
    private readonly JsonSerializerOptions _jsonOptions;
    private CancellationTokenSource? _cts;

    public event Action<List<ConnectionRecord>>? OnConnectionsUpdated;
    public event Action<List<LogEntry>>? OnLogEntryReceived;
    public event Action<ServiceStats>? OnStatsUpdated;
    public event Action<ServiceStatus>? OnStatusChanged;

    public async Task ConnectAsync()
    {
        _pipeClient = new NamedPipeClientStream(".", _pipeName,
            PipeDirection.InOut, PipeOptions.Asynchronous);
        await _pipeClient.ConnectAsync(5000);
        _cts = new CancellationTokenSource();
        _ = ListenForPushAsync(_cts.Token);  // Background listener
    }

    public async Task<IpcResponse> SendRequestAsync(IpcRequest request)
    {
        // 1. Serialize request to JSON
        // 2. Write to pipe (length-prefixed)
        // 3. Read response
        // 4. Deserialize and return
    }

    private async Task ListenForPushAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            // Read push messages from pipe
            var message = await ReadMessageAsync(ct);
            DispatchPushMessage(message);
        }
    }

    public void Dispose()
    {
        _cts?.Cancel();
        _pipeClient?.Dispose();
    }
}
```

## 6. Макет GUI

### 6.1 Главное окно

```
┌─────────────────────────────────────────────────────────────────┐
│ TcpRedirector — Proxy Redirector                  _ □ X         │
├─────────────────────────────────────────────────────────────────┤
│ [Proxy Settings] [Rules] [Connections] [Logs] [Service Control] │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│                 Content Area (based on selected tab)             │
│                                                                  │
│                                                                  │
│                                                                  │
├─────────────────────────────────────────────────────────────────┤
│ Status: ● Running  |  Active Connections: 42  |  RX: 1.2 GB    │
└─────────────────────────────────────────────────────────────────┘
```

### 6.2 Proxy Settings View

```
┌─────────────────────────────────────────────────────────────────┐
│ Proxy Settings                                                   │
├─────────────────────────────────────────────────────────────────┤
│ Host:        [________________]                                  │
│ Port:        [_____]                                             │
│ Auth:        [x] Require authentication                          │
│ Login:       [________________]                                  │
│ Password:    [________________]                                  │
│                                                                  │
│ [Save]  [Test Connection]                                        │
│                                                                  │
│ Status: Connection successful (25ms latency)                     │
└─────────────────────────────────────────────────────────────────┘
```

### 6.3 Rules View

```
┌─────────────────────────────────────────────────────────────────┐
│ Rules                                              [+ Add]      │
├─────────────────────────────────────────────────────────────────┤
│ Search: [_______________]                                       │
├─────────────────────────────────────────────────────────────────┤
│ # │ Type         │ Pattern          │ Action │ Enabled │ Edit │
│───┼──────────────┼──────────────────┼────────┼─────────┼──────│
│ 1 │ Process Name │ chrome.exe       │ Proxy  │   ✓     │ [✎]  │
│ 2 │ Process Name │ firefox.exe      │ Proxy  │   ✓     │ [✎]  │
│ 3 │ Process Path │ C:\Apps\*        │ Proxy  │   ✓     │ [✎]  │
│ 4 │ Global       │ *                │ Direct │   ✓     │ [✎]  │
│───┼──────────────┼──────────────────┼────────┼─────────┼──────│
│                                                       [🗑]     │
└─────────────────────────────────────────────────────────────────┘
```

### 6.4 Connections View

```
┌─────────────────────────────────────────────────────────────────┐
│ Connections                                      Filter: [____]  │
├─────────────────────────────────────────────────────────────────┤
│ PID   │ Process    │ Destination    │ Port │ Duration │ RX/TX   │
│───────┼────────────┼────────────────┼──────┼──────────┼─────────│
│ 1234  │ chrome.exe │ google.com     │  443 │ 00:02:34 │ 1.2M/45K│
│ 5678  │ java.exe   │ api.example.com│ 8080 │ 00:01:12 │ 890K/12K│
│ 9012  │ chrome.exe │ 142.250.185.46│  443 │ 00:00:45 │ 234K/8K │
│───────┼────────────┼────────────────┼──────┼──────────┼─────────│
│ Total: 3 active connections                                       │
│                                                                  │
│ [Connection Details]                                             │
│ ┌─────────────────────────────────────────────────────────────┐  │
│ │ PID: 1234                   │ Process: chrome.exe          │  │
│ │ Host: google.com            │ IP: 142.250.185.46           │  │
│ │ Port: 443                   │ Start: 12:34:56              │  │
│ │ Duration: 00:02:34          │ Proxy: Yes                   │  │
│ │ RX: 1,234,567 bytes         │ TX: 45,678 bytes             │  │
│ └─────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

### 6.5 Logs View

```
┌─────────────────────────────────────────────────────────────────┐
│ Logs                                          Level: [INFO ▼]   │
├─────────────────────────────────────────────────────────────────┤
│ Search: [_______________]                      [Clear] [Export] │
├─────────────────────────────────────────────────────────────────┤
│ 12:34:56  │ INFO  │ Service started successfully               │
│ 12:34:57  │ INFO  │ Driver loaded                              │
│ 12:34:58  │ INFO  │ Rules applied: 3 active                    │
│ 12:35:01  │ DEBUG │ Connection redirected: chrome.exe (1234)   │
│ 12:35:01  │ DEBUG │ CONNECT google.com:443 → proxy.example.com │
│ 12:35:02  │ INFO  │ Tunnel established: chrome.exe → google.com│
│ 12:35:10  │ TRACE │ [1234] RX: 1024 bytes                      │
│─────────────────────────────────────────────────────────────────│
│ [x] Auto-scroll                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 6.6 Service Control View

```
┌─────────────────────────────────────────────────────────────────┐
│ Service Control                                                  │
├─────────────────────────────────────────────────────────────────┤
│ Status: ● Running (uptime: 2d 14h 32m)                         │
├─────────────────────────────────────────────────────────────────┤
│                        ┌─────────┐                              │
│                        │  STOP   │                              │
│                        └─────────┘                              │
│                    ┌───────────┐                                │
│                    │ RESTART   │                                │
│                    └───────────┘                                │
├─────────────────────────────────────────────────────────────────┤
│ Statistics:                                                      │
│ ┌─────────────────────────────────────────────────────────────┐  │
│ │ Total connections: 1,234             Active: 42             │  │
│ │ Total RX: 1.2 GB                     Total TX: 345 MB      │  │
│ │ Proxy errors: 12                     Avg latency: 45ms     │  │
│ └─────────────────────────────────────────────────────────────┘  │
│ ├───────────────────────────────────────────────────────────────┤│
│ │ [Simple line chart showing connections over time]              ││
│ └───────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────┘
```

## 7. DI конфигурация

```csharp
// App.xaml.cs
public partial class App : Application
{
    private IServiceProvider _serviceProvider = null!;

    protected override void OnStartup(StartupEventArgs e)
    {
        var services = new ServiceCollection();

        // Services
        services.AddSingleton<IIpcClient, IpcClient>();
        services.AddSingleton<IServiceController, ServiceController>();
        services.AddTransient<INotificationService, NotificationService>();

        // ViewModels
        services.AddSingleton<ShellViewModel>();
        services.AddTransient<ProxySettingsViewModel>();
        services.AddTransient<RulesViewModel>();
        services.AddTransient<ConnectionsViewModel>();
        services.AddTransient<LogsViewModel>();
        services.AddTransient<ServiceControlViewModel>();

        // Views
        services.AddTransient<MainWindow>();

        _serviceProvider = services.BuildServiceProvider();

        // Start main window
        _serviceProvider.GetRequiredService<MainWindow>().Show();
    }
}
```

## 8. Безопасность GUI

1. **Пароль не кэшируется в GUI** — передаётся сервису и немедленно забывается
2. **Нет логов пароля** — поле Password маскируется
3. **IPC через Named Pipe** — только локальный доступ
4. **Статус `has_password`** — булево поле, не пароль
5. **Требуется Admin** — GUI проверяет права при запуске