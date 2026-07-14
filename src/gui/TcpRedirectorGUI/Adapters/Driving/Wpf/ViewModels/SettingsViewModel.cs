using System.Collections.ObjectModel;
using System.ComponentModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Microsoft.Win32;
using TcpRedirectorGUI.Domain.Entities;
using TcpRedirectorGUI.Domain.Ports;
using TcpRedirectorGUI.Infrastructure.Config;

namespace TcpRedirectorGUI.Adapters.Driving.Wpf.ViewModels;

/// <summary>
/// Settings ViewModel — proxy configuration, authentication, rules.
///
/// Persistence:
///   Save button → config.json (atomic write) + IPC sync to service (best-effort).
///   Config is read at startup, after service start, and after service stop.
///
/// Rule lifecycle:
///   Rule in list = active. Removed = inactive. No per-rule enable/disable toggle.
///   C++ RuleEngine still supports enabled field; GUI always sends enabled=true.
///
/// Service does NOT write config.json — GUI is the sole writer.
///
/// WP5: extended with capture-mode selector, Wintun settings, and per-app
/// multi-port rules (v2 schema). The legacy <c>Rules</c> collection is retained
/// for compile compatibility and IPC-side sync — its contents are now derived
/// from <see cref="Apps"/> on Save.
/// </summary>
public partial class SettingsViewModel : ObservableObject
{
    private readonly IConfigRepository _config;
    private readonly ITcpRedirectorService? _svc;

    public SettingsViewModel(IConfigRepository config, ITcpRedirectorService? svc = null)
    {
        _config = config;
        _svc = svc;
    }

    // ── Proxy ────────────────────────────────────────
    [ObservableProperty] private string _host = "";
    [ObservableProperty] private int _port = 3128;
    [ObservableProperty] private bool _authRequired;

    // ── Auth ─────────────────────────────────────────
    // Kerberos and Basic auth are mutually exclusive:
    //  - Enabling Kerberos auto-enables AuthRequired
    //  - Disabling AuthRequired auto-disables Kerberos
    [ObservableProperty] private string _login = "";
    [ObservableProperty] private string _password = "";
    [ObservableProperty] private bool _kerberosEnabled;

    partial void OnKerberosEnabledChanged(bool value)
    {
        if (value && !AuthRequired)
            AuthRequired = true;
    }

    partial void OnAuthRequiredChanged(bool value)
    {
        if (!value && KerberosEnabled)
            KerberosEnabled = false;
    }

    // ── Rules (legacy — kept for IPC compat) ─────────
    [ObservableProperty] private ObservableCollection<Rule> _rules = [];
    [ObservableProperty] private Rule? _selectedRule;

    // ── WP5: Capture mode ────────────────────────────
    /// <summary>Enum values shown in the "Режим захвата" combo box.</summary>
    public IReadOnlyList<CaptureMode> CaptureModes { get; } =
        [CaptureMode.WinDivert, CaptureMode.Wintun];

    [ObservableProperty] private CaptureMode _captureMode = CaptureMode.WinDivert;

    /// <summary>Drives IsEnabled on the Wintun sub-panel.</summary>
    public bool IsWintunSelected => CaptureMode == CaptureMode.Wintun;

    partial void OnCaptureModeChanged(CaptureMode value)
    {
        OnPropertyChanged(nameof(IsWintunSelected));
    }

    // ── WP5: Wintun settings ─────────────────────────
    [ObservableProperty] private WintunSettings _wintun = new();

    /// <summary>Enum values shown in the "Движок" combo box.</summary>
    public IReadOnlyList<WintunEngineKind> WintunEngines { get; } =
        [WintunEngineKind.Embedded, WintunEngineKind.External];

    // Wintun.* is a plain POCO with no INPC, so we mirror the fields we bind
    // to as flat properties here. Cheaper than wrapping WintunSettings +
    // ExternalEngineSettings each in their own VMs.
    public string WintunAdapterName
    {
        get => Wintun.AdapterName;
        set { if (Wintun.AdapterName != value) { Wintun.AdapterName = value; OnPropertyChanged(); } }
    }

    public string WintunTunnelIpv4Cidr
    {
        get => Wintun.TunnelIpv4Cidr;
        set { if (Wintun.TunnelIpv4Cidr != value) { Wintun.TunnelIpv4Cidr = value; OnPropertyChanged(); } }
    }

    public int WintunMtu
    {
        get => Wintun.Mtu;
        set { if (Wintun.Mtu != value) { Wintun.Mtu = value; OnPropertyChanged(); } }
    }

    public WintunEngineKind WintunEngine
    {
        get => Wintun.Engine;
        set
        {
            if (Wintun.Engine != value)
            {
                Wintun.Engine = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(IsExternalEngine));
            }
        }
    }

    public bool IsExternalEngine => Wintun.Engine == WintunEngineKind.External;

    public bool WintunProcessFilterEnabled
    {
        get => Wintun.ProcessFilterEnabled;
        set
        {
            if (Wintun.ProcessFilterEnabled != value)
            {
                Wintun.ProcessFilterEnabled = value;
                OnPropertyChanged();
            }
        }
    }

    public string ExternalExecutable
    {
        get => Wintun.ExternalEngine.Executable;
        set
        {
            if (Wintun.ExternalEngine.Executable != value)
            {
                Wintun.ExternalEngine.Executable = value;
                OnPropertyChanged();
            }
        }
    }

    public string ExternalSocks5Listen
    {
        get => Wintun.ExternalEngine.Socks5Listen;
        set
        {
            if (Wintun.ExternalEngine.Socks5Listen != value)
            {
                Wintun.ExternalEngine.Socks5Listen = value;
                OnPropertyChanged();
            }
        }
    }

    public bool ExternalRestartOnCrash
    {
        get => Wintun.ExternalEngine.RestartOnCrash;
        set
        {
            if (Wintun.ExternalEngine.RestartOnCrash != value)
            {
                Wintun.ExternalEngine.RestartOnCrash = value;
                OnPropertyChanged();
            }
        }
    }

    // ── WP5: Apps (per-app multi-port rules) ────────
    [ObservableProperty] private ObservableCollection<AppRuleViewModel> _apps = new();
    [ObservableProperty] private AppRuleViewModel? _selectedApp;

    // ── Log Level ────────────────────────────────────
    [ObservableProperty] private int _logLevelFilter = 2;
    [ObservableProperty] private string _logFilterLabel = "INFO+";
    [ObservableProperty] private string _logMsg = "";

    partial void OnLogLevelFilterChanged(int value)
    {
        LogFilterLabel = value switch
        {
            0 => "TRACE+", 1 => "DEBUG+", 2 => "INFO+", 3 => "WARN+", 4 => "ERROR+", _ => "INFO+"
        };
        _config.WriteInt("log", "level", value);
        try { if (_svc is { IsConnected: true }) _ = _svc.SetLogLevelAsync(value); } catch { }
    }

    [RelayCommand]
    private async Task SaveLogLevelAsync()
    {
        _config.WriteInt("log", "level", LogLevelFilter);
        try { if (_svc is { IsConnected: true }) await _svc.SetLogLevelAsync(LogLevelFilter); } catch { }
        LogMsg = "\u2713 Saved";
        _ = ClearLogMsgAfterDelay();
    }

    private async Task ClearLogMsgAfterDelay()
    {
        await Task.Delay(3000);
        LogMsg = "";
    }

    // ── Status ───────────────────────────────────────
    [ObservableProperty] private string _msg = "";
    public event Action? Saved;

    /// <summary>Load settings from config.json.</summary>
    public void LoadFromConfig()
    {
        Host = _config.ReadString("proxy", "host", "127.0.0.1");
        var portStr = _config.ReadString("proxy", "port", "3128");
        Port = int.TryParse(portStr, out var p) ? p : 3128;
        AuthRequired = _config.ReadBool("auth", "enabled");
        KerberosEnabled = _config.ReadBool("auth", "kerberos");
        Login = _config.ReadString("auth", "username");
        Password = "";
        LogLevelFilter = _config.ReadInt("log", "level", 2);

        // v2 blocks
        CaptureMode = _config.ReadCaptureMode();
        Wintun = _config.ReadWintunSettings();
        // Flat mirrors need to fire PropertyChanged so bindings pick up the new instance.
        OnPropertyChanged(nameof(WintunAdapterName));
        OnPropertyChanged(nameof(WintunTunnelIpv4Cidr));
        OnPropertyChanged(nameof(WintunMtu));
        OnPropertyChanged(nameof(WintunEngine));
        OnPropertyChanged(nameof(IsExternalEngine));
        OnPropertyChanged(nameof(WintunProcessFilterEnabled));
        OnPropertyChanged(nameof(ExternalExecutable));
        OnPropertyChanged(nameof(ExternalSocks5Listen));
        OnPropertyChanged(nameof(ExternalRestartOnCrash));
        OnPropertyChanged(nameof(IsWintunSelected));

        // Apps (v2), with v1 fallback handled inside the repository.
        var loaded = _config.ReadApps();
        Apps.Clear();
        foreach (var a in loaded) Apps.Add(new AppRuleViewModel(a));

        // Legacy Rules — populated only for IPC/PR wire-compat.
        var fileRules = _config.ReadRules();
        Rules.Clear();
        foreach (var r in fileRules)
            Rules.Add(r);
    }

    [RelayCommand]
    private async Task AddRule()
    {
        var dlg = new OpenFileDialog
        {
            Title = "Выберите приложение",
            Filter = "Executables (*.exe)|*.exe|All files (*.*)|*.*",
            CheckFileExists = true
        };
        if (dlg.ShowDialog() != true) return;

        var name = System.IO.Path.GetFileName(dlg.FileName);
        Apps.Add(new AppRuleViewModel(new AppRule
        {
            Pattern = name,
            ExePath = dlg.FileName,
            ProxyId = "default",
            RouteAllTraffic = false
        }));

        // Keep legacy Rules mirror populated for IPC compatibility.
        Rules.Add(new Rule
        {
            Id = Guid.NewGuid().ToString(),
            Type = RuleType.ProcessPath,
            Action = RuleAction.Proxy,
            Pattern = dlg.FileName,
            Description = name,
            Priority = Rules.Count + 1,
            Enabled = true
        });

        await SaveAsync();
    }

    [RelayCommand]
    private async Task DeleteApp(AppRuleViewModel? app)
    {
        if (app is null) return;
        Apps.Remove(app);
        // Best-effort clean-up of legacy mirror
        var legacy = Rules.FirstOrDefault(r =>
            r.Pattern == app.Model.ExePath || r.Pattern == app.Model.Pattern);
        if (legacy is not null) Rules.Remove(legacy);
        await SaveAsync();
    }

    [RelayCommand]
    private async Task DeleteRule(Rule? rule)
    {
        if (rule is null) return;
        Rules.Remove(rule);
        await SaveAsync();
    }

    /// <summary>True when every app row's port spec parses cleanly.</summary>
    public bool AreAppsValid => Apps.All(a => !a.HasErrors);

    [RelayCommand]
    private async Task SaveAsync()
    {
        try
        {
            // 0. Client-side validation — refuse to save if any row is red.
            if (!AreAppsValid)
            {
                Msg = "\u2717 Ошибка: исправьте некорректные порты";
                _ = ClearMsgAfterDelay();
                return;
            }

            // 1. Materialise view-models → domain AppRules.
            var domainApps = new List<AppRule>(Apps.Count);
            foreach (var vm in Apps)
                domainApps.Add(vm.ToDomain());

            var currentPwd = Password;

            // 2. Write full v2 payload (with legacy mirror inside).
            var ok = _config.WriteFullV2(
                new ProxyConfig
                {
                    Host = Host, Port = Port,
                    AuthRequired = AuthRequired, Login = Login,
                    KerberosEnabled = KerberosEnabled
                },
                CaptureMode,
                Wintun,
                domainApps);

            if (!ok)
            {
                Msg = "\u2717 Ошибка: не удалось записать config.json";
                _ = ClearMsgAfterDelay();
                return;
            }

            // 3. IPC sync (best-effort) — service still consumes legacy rules[].
            //    Re-derive the Rules collection from the just-written apps[] so
            //    the in-memory VM matches what's now on disk.
            Rules.Clear();
            foreach (var a in domainApps)
            {
                if (a.RouteAllTraffic) continue;
                if (a.Ports.Count == 0 && a.PortRanges.Count == 0)
                {
                    // Route-all=false with no ports would never match on the legacy
                    // reader either; skip it there too.
                    continue;
                }
                Rules.Add(new Rule
                {
                    Id = Guid.NewGuid().ToString(),
                    Type = RuleType.ProcessName,
                    Action = RuleAction.Proxy,
                    Pattern = a.Pattern,
                    Description = a.Pattern,
                    Priority = Rules.Count + 1,
                    Enabled = true
                });
            }

            if (_svc is { IsConnected: true })
            {
                try
                {
                    await _svc.SetRulesAsync([.. Rules]);
                    await _svc.SetConfigAsync(new ProxyConfig
                    {
                        Host = Host, Port = Port,
                        AuthRequired = AuthRequired, Login = Login,
                        Password = currentPwd, KerberosEnabled = KerberosEnabled
                    });
                }
                catch { /* IPC failure is non-fatal */ }
            }

            Msg = "\u2713 Сохранено";
            Password = "";
            Saved?.Invoke();
            _ = ClearMsgAfterDelay();
        }
        catch (Exception ex)
        {
            Msg = $"\u2717 Ошибка: {ex.Message}";
            _ = ClearMsgAfterDelay();
        }
    }

    private async Task ClearMsgAfterDelay()
    {
        await Task.Delay(3000);
        if (Msg.Contains('\u2713') || Msg.Contains('\u2717'))
            Msg = "";
    }
}

// ===========================================================================
// AppRuleViewModel — one row in the "Приложения" DataGrid.
//
// Wraps a domain <see cref="AppRule"/> and exposes a single edit-friendly
// <c>PortSpec</c> string that binds to the port textbox. <c>PortsEditable</c>
// (== !RouteAllTraffic) drives IsEnabled on that textbox.
//
// Implements INotifyDataErrorInfo so WPF's default validation template
// paints a red border and shows the error via the tooltip. Empty PortSpec
// is valid; parse errors set a per-row error message.
// ===========================================================================
public sealed class AppRuleViewModel : ObservableObject, INotifyDataErrorInfo
{
    private readonly AppRule _model;
    private string? _portsError;
    private string _portSpec;

    public AppRuleViewModel(AppRule model)
    {
        _model = model;
        _portSpec = PortSpecParser.Format(model.Ports, model.PortRanges);
    }

    public AppRule Model => _model;

    public string Pattern
    {
        get => _model.Pattern;
        set
        {
            if (_model.Pattern != value)
            {
                _model.Pattern = value;
                OnPropertyChanged();
            }
        }
    }

    public string ExePath
    {
        get => _model.ExePath;
        set
        {
            if (_model.ExePath != value)
            {
                _model.ExePath = value;
                OnPropertyChanged();
            }
        }
    }

    public string ProxyId
    {
        get => _model.ProxyId;
        set
        {
            if (_model.ProxyId != value)
            {
                _model.ProxyId = value;
                OnPropertyChanged();
            }
        }
    }

    public bool RouteAllTraffic
    {
        get => _model.RouteAllTraffic;
        set
        {
            if (_model.RouteAllTraffic != value)
            {
                _model.RouteAllTraffic = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(PortsEditable));
            }
        }
    }

    /// <summary>Drives <c>IsEnabled</c> on the port TextBox.</summary>
    public bool PortsEditable => !RouteAllTraffic;

    /// <summary>Text form of Ports + PortRanges. Two-way bound.</summary>
    public string PortSpec
    {
        get => _portSpec;
        set
        {
            if (_portSpec == value) return;
            _portSpec = value ?? "";
            OnPropertyChanged();
            RevalidatePortSpec();
        }
    }

    /// <summary>Human-readable error text; null when the spec is valid.</summary>
    public string? PortsError
    {
        get => _portsError;
        private set
        {
            if (_portsError != value)
            {
                _portsError = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(HasErrors));
                ErrorsChanged?.Invoke(this, new DataErrorsChangedEventArgs(nameof(PortSpec)));
            }
        }
    }

    private void RevalidatePortSpec()
    {
        if (PortSpecParser.TryParse(_portSpec, out var ports, out var ranges, out var err))
        {
            _model.Ports = ports;
            _model.PortRanges = ranges;
            PortsError = null;
        }
        else
        {
            PortsError = err;
        }
    }

    /// <summary>Snapshot as a domain <see cref="AppRule"/> — re-parses PortSpec so latest edits are captured.</summary>
    public AppRule ToDomain()
    {
        if (PortSpecParser.TryParse(_portSpec, out var ports, out var ranges, out _))
        {
            _model.Ports = ports;
            _model.PortRanges = ranges;
        }
        return _model;
    }

    // INotifyDataErrorInfo
    public bool HasErrors => !string.IsNullOrEmpty(_portsError);
    public event EventHandler<DataErrorsChangedEventArgs>? ErrorsChanged;

    public System.Collections.IEnumerable GetErrors(string? propertyName)
    {
        if (propertyName == nameof(PortSpec) && !string.IsNullOrEmpty(_portsError))
            return new[] { _portsError };
        return Array.Empty<string>();
    }
}
