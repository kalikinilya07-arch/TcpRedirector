using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.ComponentModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Microsoft.Win32;
using TcpRedirectorGUI.Domain.Entities;
using TcpRedirectorGUI.Domain.Ports;

namespace TcpRedirectorGUI.Adapters.Driving.Wpf.ViewModels;

/// <summary>
/// Settings ViewModel — manages proxy configuration, authentication, and rules.
/// Reads/writes via IConfigRepository (config.json on disk).
/// Optionally syncs via IPC when service is connected.
/// </summary>
public partial class SettingsViewModel : ObservableObject
{
    private readonly IConfigRepository _config;
    private readonly ITcpRedirectorService? _svc;

    public SettingsViewModel(IConfigRepository config, ITcpRedirectorService? svc = null)
    {
        _config = config;
        _svc = svc;
        // Auto-save when any rule's Enabled property changes
        Rules.CollectionChanged += OnRulesCollectionChanged;
    }

    // ── Proxy ────────────────────────────────────────
    [ObservableProperty] private string _host = "";
    [ObservableProperty] private int _port = 3128;
    [ObservableProperty] private bool _authRequired;

    // ── Auth ─────────────────────────────────────────
    [ObservableProperty] private string _login = "";
    [ObservableProperty] private string _password = "";
    [ObservableProperty] private bool _kerberosEnabled;

    // ── Rules ────────────────────────────────────────
    [ObservableProperty] private ObservableCollection<Rule> _rules = [];
    [ObservableProperty] private Rule? _selectedRule;

    private void OnRulesCollectionChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        if (e.NewItems is not null)
        {
            foreach (Rule rule in e.NewItems)
                rule.PropertyChanged += OnRulePropertyChanged;
        }
        if (e.OldItems is not null)
        {
            foreach (Rule rule in e.OldItems)
                rule.PropertyChanged -= OnRulePropertyChanged;
        }
    }

    private async void OnRulePropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(Rule.Enabled))
            await SaveAsync();
    }

    // ── Log Level ────────────────────────────────────
    [ObservableProperty] private int _logLevelFilter = 2; // 0=TRACE..4=ERROR
    [ObservableProperty] private string _logFilterLabel = "INFO+";
    [ObservableProperty] private string _logMsg = "";

    partial void OnLogLevelFilterChanged(int value)
    {
        LogFilterLabel = value switch
        {
            0 => "TRACE+",
            1 => "DEBUG+",
            2 => "INFO+",
            3 => "WARN+",
            4 => "ERROR+",
            _ => "INFO+"
        };

        // Persist to config.json
        _config.WriteInt("log", "level", value);

        // Auto-apply log level to backend service when connected
        try
        {
            if (_svc is { IsConnected: true })
            {
                _ = _svc.SetLogLevelAsync(value);
            }
        }
        catch
        {
            // IPC not available yet — level will apply on next IPC call
        }
    }

    [RelayCommand]
    private async Task SaveLogLevelAsync()
    {
        _config.WriteInt("log", "level", LogLevelFilter);
        try
        {
            if (_svc is { IsConnected: true })
                await _svc.SetLogLevelAsync(LogLevelFilter);
        }
        catch { }
        LogMsg = "✓ Saved";
        _ = ClearLogMsgAfterDelay();
    }

    private async Task ClearLogMsgAfterDelay()
    {
        await Task.Delay(3000);
        LogMsg = "";
    }

    // ── Status ───────────────────────────────────────
    [ObservableProperty] private string _msg = "";

    /// <summary>Fired after successful save (for UI cleanup).</summary>
    public event Action? Saved;

    /// <summary>Load settings from config.json. Safe to call from constructor.</summary>
    public void LoadFromConfig()
    {
        Host = _config.ReadString("proxy", "host", "127.0.0.1");
        var portStr = _config.ReadString("proxy", "port", "3128");
        Port = int.TryParse(portStr, out var p) ? p : 3128;
        AuthRequired = _config.ReadBool("auth", "enabled");
        KerberosEnabled = _config.ReadBool("auth", "kerberos");
        Login = _config.ReadString("auth", "username");
        Password = "";

        // Load log level from config
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
    private void LoadRules()
    {
        var fileRules = _config.ReadRules();
        Rules.Clear();
        foreach (var r in fileRules)
            Rules.Add(r);
    }

    [RelayCommand]
    private async Task SaveAsync()
    {
        try
        {
            // Determine exePath for WinDivert target
            var exePath = Rules
                .FirstOrDefault(r => r.Enabled && r.Type == RuleType.ProcessPath)
                ?.Pattern;

            if (string.IsNullOrEmpty(exePath))
                exePath = _config.ReadString("app", "exePath");

            var currentPwd = Password;

            // IPC sync to running service
            if (_svc is { IsConnected: true })
            {
                try
                {
                    await _svc.SetRulesAsync([.. Rules]);
                    await _svc.SetConfigAsync(new ProxyConfig
                    {
                        Host = Host,
                        Port = Port,
                        AuthRequired = AuthRequired,
                        Login = Login,
                        Password = currentPwd,
                        KerberosEnabled = KerberosEnabled
                    });
                }
                catch
                {
                    // IPC failure is non-fatal; config.json will be used on restart
                }
            }

            // Write full config to disk (primary persistence)
            var ok = _config.WriteFull(
                new ProxyConfig
                {
                    Host = Host,
                    Port = Port,
                    AuthRequired = AuthRequired,
                    Login = Login,
                    KerberosEnabled = KerberosEnabled
                },
                exePath ?? "",
                [.. Rules]);

            if (ok)
            {
                Msg = $"\u2713 Saved";
                Password = "";
                Saved?.Invoke();
            }
            else
            {
                Msg = "\u2717 Error: failed to write config.json";
            }
            _ = ClearMsgAfterDelay();
        }
        catch (Exception ex)
        {
            Msg = $"\u2717 Error: {ex.Message}";
        }
    }

    private async Task ClearMsgAfterDelay()
    {
        await Task.Delay(3000);
        if (Msg.Contains('\u2713'))
            Msg = "";
    }
}