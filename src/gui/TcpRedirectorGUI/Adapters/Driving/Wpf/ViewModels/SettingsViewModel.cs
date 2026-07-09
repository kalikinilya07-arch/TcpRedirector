using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Microsoft.Win32;
using TcpRedirectorGUI.Domain.Entities;
using TcpRedirectorGUI.Domain.Ports;

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

    // ── Rules ────────────────────────────────────────
    [ObservableProperty] private ObservableCollection<Rule> _rules = [];
    [ObservableProperty] private Rule? _selectedRule;

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
            Title = "Select Application for Rule",
            Filter = "Executables (*.exe)|*.exe|All files (*.*)|*.*",
            CheckFileExists = true
        };
        if (dlg.ShowDialog() != true) return;

        Rules.Add(new Rule
        {
            Id = Guid.NewGuid().ToString(),
            Type = RuleType.ProcessPath,
            Action = RuleAction.Proxy,
            Pattern = dlg.FileName,
            Description = System.IO.Path.GetFileName(dlg.FileName),
            Priority = Rules.Count + 1,
            Enabled = true
        });

        await SaveAsync();
    }

    [RelayCommand]
    private async Task DeleteRule(Rule? rule)
    {
        if (rule is null) return;
        Rules.Remove(rule);
        await SaveAsync();
    }

    [RelayCommand]
    private async Task SaveAsync()
    {
        try
        {
            var exePath = Rules.FirstOrDefault(r => r.Type == RuleType.ProcessPath)?.Pattern;
            if (string.IsNullOrEmpty(exePath))
                exePath = _config.ReadString("app", "exePath");

            var currentPwd = Password;

            // 1. Write to disk
            var ok = _config.WriteFull(
                new ProxyConfig
                {
                    Host = Host, Port = Port,
                    AuthRequired = AuthRequired, Login = Login,
                    KerberosEnabled = KerberosEnabled
                },
                exePath ?? "",
                [.. Rules]);

            if (!ok)
            {
                Msg = "\u2717 Error: failed to write config.json";
                _ = ClearMsgAfterDelay();
                return;
            }

            // 2. IPC sync (best-effort)
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

            Msg = "\u2713 Saved";
            Password = "";
            Saved?.Invoke();
            _ = ClearMsgAfterDelay();
        }
        catch (Exception ex)
        {
            Msg = $"\u2717 Error: {ex.Message}";
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
