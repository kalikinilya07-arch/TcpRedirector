using System.Collections;
using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.ComponentModel;
using System.Net;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Microsoft.Win32;
using TcpRedirectorGUI.Domain.Entities;
using TcpRedirectorGUI.Domain.Ports;
using TcpRedirectorGUI.Infrastructure.Config;
using TcpRedirectorGUI.Infrastructure.Localization;

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
public partial class SettingsViewModel : ObservableObject, INotifyDataErrorInfo
{
    private readonly IConfigRepository _config;
    private readonly ITcpRedirectorService? _svc;

    // Field-level validation errors keyed by public property name.
    private readonly Dictionary<string, string> _fieldErrors = new();

    // When true, property changes neither mark the VM dirty nor re-validate
    // (used while loading from disk so a reload doesn't look like user edits).
    private bool _suppressChangeTracking;

    // Property names that must NOT flip the dirty flag or trigger validation.
    private static readonly HashSet<string> s_nonContentProps = new(StringComparer.Ordinal)
    {
        nameof(Msg), nameof(LogMsg), nameof(LogFilterLabel),
        nameof(IsDirty), nameof(HasErrors), nameof(SelectedApp),
        nameof(SelectedRule), nameof(IsWintunSelected), nameof(IsWinDivertSelected),
        nameof(IsExternalEngine),
        // Не «контентные»: отражают лишь наличие сохранённого пароля/плейсхолдер.
        nameof(HasStoredPassword), nameof(PasswordPlaceholder),
        // (Задача №3) Производное от AuthRequired/KerberosEnabled — не контент.
        nameof(ShowBasicCredentials),
        // (Phase 9) Производное от KerberosEnabled/PerUserEnabled — не контент.
        nameof(ShowPerUserOptions)
    };

    public SettingsViewModel(IConfigRepository config, ITcpRedirectorService? svc = null)
    {
        _config = config;
        _svc = svc;

        Apps.CollectionChanged += OnAppsCollectionChanged;
    }

    // ── Dirty tracking ───────────────────────────────
    /// <summary>True when the user changed settings that have not been saved yet.</summary>
    [ObservableProperty] private bool _isDirty;

    // ── Change tracking + validation plumbing ────────
    protected override void OnPropertyChanged(PropertyChangedEventArgs e)
    {
        base.OnPropertyChanged(e);

        if (_suppressChangeTracking) return;
        var name = e.PropertyName;
        if (string.IsNullOrEmpty(name) || s_nonContentProps.Contains(name!)) return;

        IsDirty = true;
        ValidateAll();
    }

    private void OnAppsCollectionChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        if (e.OldItems is not null)
            foreach (AppRuleViewModel a in e.OldItems)
            {
                a.ErrorsChanged -= OnAppErrorsChanged;
                a.PropertyChanged -= OnAppPropertyChanged;
            }
        if (e.NewItems is not null)
            foreach (AppRuleViewModel a in e.NewItems)
            {
                a.ErrorsChanged += OnAppErrorsChanged;
                a.PropertyChanged += OnAppPropertyChanged;
            }

        if (!_suppressChangeTracking)
            IsDirty = true;

        OnPropertyChanged(nameof(HasErrors));
        SaveAllCommand.NotifyCanExecuteChanged();
    }

    private void OnAppErrorsChanged(object? sender, DataErrorsChangedEventArgs e)
    {
        OnPropertyChanged(nameof(HasErrors));
        SaveAllCommand.NotifyCanExecuteChanged();
    }

    // B7 (QA audit): editing an EXISTING app row (Pattern / PortSpec / ProxyId /
    // RouteAllTraffic) must mark the config dirty. Previously only collection
    // add/remove and per-row error changes were observed, so in-place edits left
    // IsDirty=false and ReloadFromConfigIfClean() (on service start/stop)
    // silently discarded them.
    private void OnAppPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (_suppressChangeTracking) return;
        // Skip notifications that don't represent a content change.
        if (e.PropertyName is nameof(AppRuleViewModel.PortsError)
                           or nameof(AppRuleViewModel.HasErrors)
                           or nameof(AppRuleViewModel.PortsEditable))
            return;

        IsDirty = true;
        OnPropertyChanged(nameof(HasErrors));
        SaveAllCommand.NotifyCanExecuteChanged();
    }

    /// <summary>
    /// Re-validates every field. Errors are surfaced through
    /// <see cref="INotifyDataErrorInfo"/> so bound controls highlight and show a
    /// tooltip; <see cref="HasErrors"/> gates <see cref="SaveAllCommand"/>.
    /// </summary>
    private void ValidateAll()
    {
        // Proxy
        SetError(nameof(Host),
            string.IsNullOrWhiteSpace(Host) ? Loc.T("Val.HostRequired")
            : IsValidHost(Host) ? null : Loc.T("Val.HostInvalid"));

        SetError(nameof(Port),
            Port is >= 1 and <= 65535 ? null : Loc.T("Val.PortRange"));

        // Auth (Basic only): login required when auth enabled and not Kerberos.
        SetError(nameof(Login),
            (AuthRequired && !KerberosEnabled && string.IsNullOrWhiteSpace(Login))
                ? Loc.T("Val.LoginRequired") : null);

        // (Phase 9) Per-user helper timeout — validated only when the per-user
        // options are actually in effect (Kerberos + per-user enabled).
        SetError(nameof(HelperTimeoutMs),
            (ShowPerUserOptions && HelperTimeoutMs is < 100 or > 120000)
                ? Loc.T("Val.HelperTimeoutRange") : null);

        // Wintun-only fields.
        var wintun = CaptureMode == CaptureMode.Wintun;
        SetError(nameof(WintunAdapterName),
            wintun && string.IsNullOrWhiteSpace(WintunAdapterName)
                ? Loc.T("Val.AdapterNameRequired") : null);

        SetError(nameof(WintunTunnelIpv4Cidr),
            wintun && !IsValidIpv4Cidr(WintunTunnelIpv4Cidr)
                ? Loc.T("Val.Ipv4Cidr") : null);

        SetError(nameof(WintunMtu),
            wintun && WintunMtu is < 576 or > 65535
                ? Loc.T("Val.MtuRange") : null);

        SetError(nameof(WintunRouteLadderPrefix),
            wintun && WintunRouteLadderPrefix is < 1 or > 8
                ? Loc.T("Val.RouteLadderRange") : null);

        var external = wintun && WintunEngine == WintunEngineKind.External;
        SetError(nameof(ExternalExecutable),
            external && string.IsNullOrWhiteSpace(ExternalExecutable)
                ? Loc.T("Val.ExternalExeRequired") : null);

        SetError(nameof(ExternalSocks5Listen),
            external && !IsValidHostPort(ExternalSocks5Listen)
                ? Loc.T("Val.Socks5HostPort") : null);

        OnPropertyChanged(nameof(HasErrors));
        SaveAllCommand.NotifyCanExecuteChanged();
    }

    private void SetError(string property, string? error)
    {
        _fieldErrors.TryGetValue(property, out var existing);
        var incoming = error ?? "";
        if (string.Equals(existing ?? "", incoming, StringComparison.Ordinal))
            return;

        if (string.IsNullOrEmpty(error))
            _fieldErrors.Remove(property);
        else
            _fieldErrors[property] = error;

        ErrorsChanged?.Invoke(this, new DataErrorsChangedEventArgs(property));
    }

    // ── Validators ───────────────────────────────────
    private static bool IsValidHost(string host)
    {
        host = host.Trim();
        if (host.Length == 0) return false;
        if (IPAddress.TryParse(host, out _)) return true;
        return Uri.CheckHostName(host) != UriHostNameType.Unknown;
    }

    private static bool IsValidIpv4Cidr(string cidr)
    {
        if (string.IsNullOrWhiteSpace(cidr)) return false;
        var slash = cidr.IndexOf('/');
        if (slash <= 0 || slash == cidr.Length - 1) return false;

        var ipPart = cidr[..slash].Trim();
        var maskPart = cidr[(slash + 1)..].Trim();

        if (!IPAddress.TryParse(ipPart, out var ip) ||
            ip.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork)
            return false;

        if (!int.TryParse(maskPart, out var bits) || bits is < 0 or > 32)
            return false;

        return true;
    }

    /// <summary>
    /// (Phase 9) Coerces a persisted <c>auth.fallback_policy</c> value to one of
    /// the three valid wire tokens; anything else → the safe default "drop".
    /// Kept in sync with the repository's normalisation.
    /// </summary>
    private static string NormalizeFallbackPolicy(string? policy) =>
        (policy?.Trim().ToLowerInvariant()) switch
        {
            "system" => "system",
            "error"  => "error",
            _        => "drop"
        };

    private static bool IsValidHostPort(string value)
    {
        if (string.IsNullOrWhiteSpace(value)) return false;
        var colon = value.LastIndexOf(':');
        if (colon <= 0 || colon == value.Length - 1) return false;

        var host = value[..colon].Trim();
        var portStr = value[(colon + 1)..].Trim();
        if (host.Length == 0 || !IsValidHost(host)) return false;
        return int.TryParse(portStr, out var port) && port is >= 1 and <= 65535;
    }

    // ── INotifyDataErrorInfo ─────────────────────────
    public bool HasErrors =>
        _fieldErrors.Count > 0 || Apps.Any(a => a.HasErrors);

    public event EventHandler<DataErrorsChangedEventArgs>? ErrorsChanged;

    public IEnumerable GetErrors(string? propertyName)
    {
        if (!string.IsNullOrEmpty(propertyName) &&
            _fieldErrors.TryGetValue(propertyName!, out var err) &&
            !string.IsNullOrEmpty(err))
        {
            return new[] { err };
        }
        return Array.Empty<string>();
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

    // (Задача №2) Шифровать ли пароль прокси (DPAPI) или хранить plaintext.
    [ObservableProperty] private bool _encryptPassword = true;

    // (Задача №1) Требовать ли токен-аутентификацию на IPC-канале.
    [ObservableProperty] private bool _ipcAuthEnabled = true;

    // Уже сохранён ли пароль (в любом формате: DPAPI-encrypted ИЛИ plaintext).
    // Управляет плейсхолдером поля пароля в UI: показываем «****», если пароль
    // задан, а само поле остаётся пустым (пользователь вводит новый только при
    // необходимости заменить). Не помечает конфиг «грязным».
    [ObservableProperty] private bool _hasStoredPassword;

    /// <summary>
    /// Текст-плейсхолдер для поля пароля: маскировка «••••••••», если пароль
    /// уже сохранён, иначе пусто. Само поле Password остаётся пустым до ввода.
    /// </summary>
    public string PasswordPlaceholder => HasStoredPassword ? "••••••••" : "";

    partial void OnHasStoredPasswordChanged(bool value)
    {
        OnPropertyChanged(nameof(PasswordPlaceholder));
    }

    /// <summary>
    /// (Задача №3) Показывать ли поля Basic-аутентификации (Login, Password,
    /// «Шифровать пароль»). При включённом Kerberos эти поля не нужны (SSPI
    /// использует контекст текущей учётной записи) и скрываются; чекбокс
    /// «Use Kerberos» остаётся видимым, пока авторизация включена.
    /// </summary>
    public bool ShowBasicCredentials => AuthRequired && !KerberosEnabled;

    partial void OnKerberosEnabledChanged(bool value)
    {
        if (value && !AuthRequired)
            AuthRequired = true;
        OnPropertyChanged(nameof(ShowBasicCredentials));
        // (Phase 9) Видимость блока per-user-опций зависит от Kerberos.
        OnPropertyChanged(nameof(ShowPerUserOptions));
    }

    partial void OnAuthRequiredChanged(bool value)
    {
        if (!value && KerberosEnabled)
            KerberosEnabled = false;
        OnPropertyChanged(nameof(ShowBasicCredentials));
        // (Phase 9) Per-user-опции доступны только при включённом Kerberos,
        // который, в свою очередь, требует включённой авторизации.
        OnPropertyChanged(nameof(ShowPerUserOptions));
    }

    // ── Auth: per-user Kerberos helper (Phase 9) ─────
    // Четыре поля зеркалируют JSON-ключи auth.per_user_enabled / auth.spn /
    // auth.fallback_policy / auth.helper_timeout_ms, которые потребляет служба.
    // Дефолты совпадают с «фича выключена» для обратной совместимости.

    /// <summary>auth.per_user_enabled — включить per-user Kerberos.</summary>
    [ObservableProperty] private bool _perUserEnabled;

    /// <summary>auth.spn — явный SPN; пусто ⇒ авто HTTP/&lt;proxyhost&gt;.</summary>
    [ObservableProperty] private string _spn = "";

    /// <summary>
    /// auth.fallback_policy — «drop» | «system» | «error». Хранится и
    /// передаётся ПО ПРОВОДУ этими же строками; combobox биндится напрямую к
    /// <see cref="FallbackPolicies"/>.
    /// </summary>
    [ObservableProperty] private string _fallbackPolicy = "drop";

    /// <summary>auth.helper_timeout_ms — таймаут обмена токенами service↔helper.</summary>
    [ObservableProperty] private int _helperTimeoutMs = 5000;

    /// <summary>
    /// Допустимые значения fallback_policy в порядке отображения. Значения —
    /// это ровно те строки, что уходят в config.json ("drop"/"system"/"error"),
    /// поэтому SelectedItem можно биндить напрямую без конвертера.
    /// </summary>
    public IReadOnlyList<string> FallbackPolicies { get; } =
        ["drop", "system", "error"];

    /// <summary>
    /// (Phase 9) Показывать ли под-поля per-user-аутентификации (SPN,
    /// fallback-политика, таймаут). Они имеют смысл только при включённом
    /// Kerberos И включённом per-user-режиме, аналогично тому, как Basic-поля
    /// гейтятся на чекбоксе Kerberos.
    /// </summary>
    public bool ShowPerUserOptions => AuthRequired && KerberosEnabled && PerUserEnabled;

    partial void OnPerUserEnabledChanged(bool value)
    {
        OnPropertyChanged(nameof(ShowPerUserOptions));
    }

    // ── Rules (legacy — kept for IPC compat) ─────────
    [ObservableProperty] private ObservableCollection<Rule> _rules = [];
    [ObservableProperty] private Rule? _selectedRule;

    // ── WP5: Capture mode ────────────────────────────
    /// <summary>Enum values shown in the "Режим захвата" combo box.</summary>
    public IReadOnlyList<CaptureMode> CaptureModes { get; } =
        [CaptureMode.WinDivert, CaptureMode.Wintun];

    // Дефолт после установки — Wintun (embedded).
    [ObservableProperty] private CaptureMode _captureMode = CaptureMode.Wintun;

    /// <summary>Drives Visibility of the Wintun sub-panel (shown only in Wintun mode).</summary>
    public bool IsWintunSelected => CaptureMode == CaptureMode.Wintun;

    /// <summary>Drives Visibility of the WinDivert info block (shown only in WinDivert mode).</summary>
    public bool IsWinDivertSelected => CaptureMode == CaptureMode.WinDivert;

    partial void OnCaptureModeChanged(CaptureMode value)
    {
        OnPropertyChanged(nameof(IsWintunSelected));
        OnPropertyChanged(nameof(IsWinDivertSelected));
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

    public int WintunRouteLadderPrefix
    {
        get => Wintun.RouteLadderPrefix;
        set { if (Wintun.RouteLadderPrefix != value) { Wintun.RouteLadderPrefix = value; OnPropertyChanged(); } }
    }

    public bool WintunBlockIpv6
    {
        get => Wintun.BlockIpv6;
        set { if (Wintun.BlockIpv6 != value) { Wintun.BlockIpv6 = value; OnPropertyChanged(); } }
    }

    public bool WintunDirectPassthrough
    {
        get => Wintun.DirectPassthrough;
        set
        {
            if (Wintun.DirectPassthrough != value)
            {
                Wintun.DirectPassthrough = value;
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
        // B3 (QA audit): persist in the service's config-file scale, but keep
        // the live IPC push on the domain scale (see mapping notes below).
        _config.WriteInt("log", "level", DomainToFileLogLevel(value));
        try { if (_svc is { IsConnected: true }) _ = _svc.SetLogLevelAsync(value); } catch { }
    }

    [RelayCommand]
    private async Task SaveLogLevelAsync()
    {
        _config.WriteInt("log", "level", DomainToFileLogLevel(LogLevelFilter));
        try { if (_svc is { IsConnected: true }) await _svc.SetLogLevelAsync(LogLevelFilter); } catch { }
        LogMsg = Loc.T("Log.Saved");
        _ = ClearLogMsgAfterDelay();
    }

    // ── B3: log-level scale translation ──────────────
    // The GUI combo and the live IPC channel use the DOMAIN scale
    // (0=TRACE,1=DEBUG,2=INFO,3=WARN,4=ERROR — see domain::LogLevel and the
    // service's IPC set_log_level, which casts the int directly).
    // But config.json "log.level" is read by the service's
    // ConfigManager::GetLogLevel on a DIFFERENT, inverted scale
    // (0=ERROR,1=WARN,2=INFO,3=DEBUG,4=TRACE). Persisting the domain value
    // verbatim inverted the level (e.g. "errors only" became INFO, "all"
    // became errors-only). Translate at the config-file boundary only.
    //
    // (Задача №3) Служба теперь понимает файловый уровень 4=TRACE, поэтому
    // доменный TRACE(0) больше НЕ схлопывается в DEBUG — он честно
    // отображается в файловый 4 и обратно (полная трассировка, в т.ч.
    // Wintun rx-stats/PROXY-трейс).
    private static int DomainToFileLogLevel(int domain) => domain switch
    {
        0 => 4, // TRACE -> TRACE (file scale теперь имеет TRACE=4)
        1 => 3, // DEBUG -> DEBUG
        2 => 2, // INFO  -> INFO
        3 => 1, // WARN  -> WARN
        4 => 0, // ERROR -> ERROR
        _ => 2  // default INFO
    };

    private static int FileToDomainLogLevel(int file) => file switch
    {
        0 => 4, // ERROR
        1 => 3, // WARN
        2 => 2, // INFO
        3 => 1, // DEBUG
        4 => 0, // TRACE
        _ => 2  // default INFO
    };

    private async Task ClearLogMsgAfterDelay()
    {
        await Task.Delay(3000);
        LogMsg = "";
    }

    // ── Status ───────────────────────────────────────
    [ObservableProperty] private string _msg = "";
    public event Action? Saved;

    /// <summary>
    /// Reload settings from disk only if the user has no unsaved edits. Called
    /// by the shell after service start/stop; skipping when dirty prevents a
    /// reload from silently discarding an in-progress change (e.g. switching
    /// capture mode back).
    /// </summary>
    /// <returns>True if a reload happened, false if skipped due to unsaved edits.</returns>
    public bool ReloadFromConfigIfClean()
    {
        if (IsDirty) return false;
        LoadFromConfig();
        return true;
    }

    /// <summary>Load settings from config.json.</summary>
    public void LoadFromConfig()
    {
        _suppressChangeTracking = true;
        try
        {
            Host = _config.ReadString("proxy", "host", "127.0.0.1");
            // proxy.port is written as a JSON number; fall back to a string form
            // only for legacy configs that stored it as text.
            Port = _config.ReadInt("proxy", "port", int.MinValue);
            if (Port == int.MinValue)
            {
                var portStr = _config.ReadString("proxy", "port", "3128");
                Port = int.TryParse(portStr, out var p) ? p : 3128;
            }
            AuthRequired = _config.ReadBool("auth", "enabled");
            KerberosEnabled = _config.ReadBool("auth", "kerberos");
            Login = _config.ReadString("auth", "username");
            // (Задача №2) Режим хранения пароля. Дефолт true — обратная
            // совместимость со старыми конфигами без этого поля.
            EncryptPassword = _config.ReadBool("auth", "encryptPassword", true);
            // (Phase 9) Per-user Kerberos helper. Отсутствующие ключи → дефолты
            // «фича выключена» (обратная совместимость со старыми конфигами).
            PerUserEnabled = _config.ReadBool("auth", "per_user_enabled", false);
            Spn = _config.ReadString("auth", "spn", "");
            FallbackPolicy = NormalizeFallbackPolicy(
                _config.ReadString("auth", "fallback_policy", "drop"));
            HelperTimeoutMs = _config.ReadInt("auth", "helper_timeout_ms", 5000);
            // Поле ввода всегда пустое; наличие сохранённого пароля показываем
            // плейсхолдером. Пароль считается заданным, если непусто ЛЮБОЕ из
            // полей: encryptedPassword (DPAPI) ИЛИ password (plaintext).
            Password = "";
            HasStoredPassword =
                !string.IsNullOrEmpty(_config.ReadString("auth", "encryptedPassword"))
                || !string.IsNullOrEmpty(_config.ReadString("auth", "password"));
            // (Задача №1) IPC-аутентификация. Дефолт true — как в службе.
            IpcAuthEnabled = _config.ReadBool("ipc", "auth_enabled", true);
            // B3: config.json stores log.level in the service file scale; map it
            // back to the domain scale the combo/IPC use.
            LogLevelFilter = FileToDomainLogLevel(_config.ReadInt("log", "level", 2));

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
            OnPropertyChanged(nameof(WintunRouteLadderPrefix));
            OnPropertyChanged(nameof(WintunBlockIpv6));
            OnPropertyChanged(nameof(WintunDirectPassthrough));
            OnPropertyChanged(nameof(ExternalExecutable));
            OnPropertyChanged(nameof(ExternalSocks5Listen));
            OnPropertyChanged(nameof(ExternalRestartOnCrash));
            OnPropertyChanged(nameof(IsWintunSelected));
            // (Задача №3) Актуализировать видимость Basic-полей после загрузки.
            OnPropertyChanged(nameof(ShowBasicCredentials));
            // (Phase 9) Актуализировать видимость per-user-опций после загрузки.
            OnPropertyChanged(nameof(ShowPerUserOptions));

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
        finally
        {
            _suppressChangeTracking = false;
        }

        // Freshly loaded state is clean; validate so the UI reflects any
        // problems already present on disk.
        IsDirty = false;
        ValidateAll();
    }

    [RelayCommand]
    private async Task AddRule()
    {
        var dlg = new OpenFileDialog
        {
            Title = Loc.T("Dialog.SelectApp"),
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

        await SaveAllAsync();
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
        await SaveAllAsync();
    }

    [RelayCommand]
    private async Task DeleteRule(Rule? rule)
    {
        if (rule is null) return;
        Rules.Remove(rule);
        await SaveAllAsync();
    }

    /// <summary>Gate for <see cref="SaveAllCommand"/> — no validation errors anywhere.</summary>
    private bool CanSaveAll() => !HasErrors;

    /// <summary>
    /// "Save all" — persists every setting (proxy, auth, capture mode, Wintun,
    /// apps, log level) to config.json, then reads the file back to verify the
    /// write actually landed, and reports success/failure with the exact path.
    /// </summary>
    [RelayCommand(CanExecute = nameof(CanSaveAll))]
    private async Task SaveAllAsync()
    {
        try
        {
            // 0. Full validation — refuse to save while anything is invalid.
            ValidateAll();
            if (HasErrors)
            {
                Msg = Loc.T("Save.ErrorFix");
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
                    // Persist the password to config.json regardless of whether
                    // the service is running. Empty => the repository preserves
                    // the existing on-disk password (encrypted or plaintext).
                    Password = currentPwd,
                    KerberosEnabled = KerberosEnabled,
                    // (Задача №2/№1) режимы хранения пароля и IPC-аутентификации.
                    EncryptPassword = EncryptPassword,
                    IpcAuthEnabled = IpcAuthEnabled,
                    // (Phase 9) Per-user Kerberos helper.
                    PerUserEnabled = PerUserEnabled,
                    Spn = Spn,
                    FallbackPolicy = FallbackPolicy,
                    HelperTimeoutMs = HelperTimeoutMs
                },
                CaptureMode,
                Wintun,
                domainApps);

            if (!ok)
            {
                Msg = $"{Loc.T("Save.ErrorWrite")} {_config.ConfigPath}";
                _ = ClearMsgAfterDelay();
                return;
            }

            // 2a. Read-back verification — confirm the intended values are on disk
            //     in the SAME file the service reads.
            if (!VerifySavedConfig(CaptureMode, Wintun, Host, Port, domainApps.Count, out var mismatch))
            {
                Msg = $"{Loc.T("Save.VerifyFailed")} {mismatch} ({_config.ConfigPath})";
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
                        Password = currentPwd, KerberosEnabled = KerberosEnabled,
                        EncryptPassword = EncryptPassword,
                        IpcAuthEnabled = IpcAuthEnabled,
                        // (Phase 9) Per-user Kerberos helper.
                        PerUserEnabled = PerUserEnabled,
                        Spn = Spn,
                        FallbackPolicy = FallbackPolicy,
                        HelperTimeoutMs = HelperTimeoutMs
                    });
                }
                catch { /* IPC failure is non-fatal */ }
            }

            // Если пользователь ввёл новый пароль — теперь он сохранён; иначе
            // сохраняем прежнее состояние «есть/нет пароля».
            if (!string.IsNullOrEmpty(currentPwd))
                HasStoredPassword = true;

            Msg = $"{Loc.T("Save.Ok")} {_config.ConfigPath}";
            Password = "";
            IsDirty = false;
            Saved?.Invoke();
            _ = ClearMsgAfterDelay();
        }
        catch (Exception ex)
        {
            Msg = $"{Loc.T("Save.ErrorPrefix")} {ex.Message}";
            _ = ClearMsgAfterDelay();
        }
    }

    /// <summary>
    /// Re-reads config.json and confirms the key fields match what we just
    /// wrote. Guards against writing to a different file than the service reads.
    /// </summary>
    private bool VerifySavedConfig(
        CaptureMode expectedMode,
        WintunSettings expectedWintun,
        string expectedHost,
        int expectedPort,
        int expectedAppCount,
        out string mismatch)
    {
        mismatch = "";
        try
        {
            var mode = _config.ReadCaptureMode();
            if (mode != expectedMode)
            {
                mismatch = $"capture_mode на диске '{mode}' вместо '{expectedMode}'";
                return false;
            }

            var w = _config.ReadWintunSettings();
            if (w.Engine != expectedWintun.Engine)
            {
                mismatch = $"wintun.engine на диске '{w.Engine}' вместо '{expectedWintun.Engine}'";
                return false;
            }

            var host = _config.ReadString("proxy", "host", "");
            if (!string.Equals(host, expectedHost, StringComparison.Ordinal))
            {
                mismatch = $"proxy.host на диске '{host}' вместо '{expectedHost}'";
                return false;
            }

            var port = _config.ReadInt("proxy", "port", -1);
            if (port != expectedPort)
            {
                mismatch = $"proxy.port на диске '{port}' вместо '{expectedPort}'";
                return false;
            }

            var appCount = _config.ReadApps().Count;
            if (appCount != expectedAppCount)
            {
                mismatch = $"число приложений на диске {appCount} вместо {expectedAppCount}";
                return false;
            }

            return true;
        }
        catch (Exception ex)
        {
            mismatch = $"ошибка чтения: {ex.Message}";
            return false;
        }
    }

    private async Task ClearMsgAfterDelay()
    {
        await Task.Delay(6000);
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
