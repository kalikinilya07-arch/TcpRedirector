using System.Collections.ObjectModel;
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
    }

    // ── Proxy ────────────────────────────────────────
    [ObservableProperty] private string _host = "";
    [ObservableProperty] private int _port = 3128;
    [ObservableProperty] private bool _authRequired;

    // ── Auth ─────────────────────────────────────────
    [ObservableProperty] private bool _authEnabled = true;
    [ObservableProperty] private string _login = "";
    [ObservableProperty] private string _password = "";
    [ObservableProperty] private bool _kerberosEnabled;

    // ── Rules ────────────────────────────────────────
    [ObservableProperty] private ObservableCollection<Rule> _rules = [];
    [ObservableProperty] private Rule? _selectedRule;

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
        AuthEnabled = _config.ReadBool("auth", "kerberos");
        KerberosEnabled = _config.ReadBool("auth", "kerberos");
        Login = _config.ReadString("auth", "username");
        Password = "";

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
                        AuthEnabled = AuthEnabled,
                        KerberosEnabled = KerberosEnabled
                    });
                }
                catch
                {
                    // IPC failure is non-fatal; config.json will be used on restart
                }
            }

            // Write full config to disk (primary persistence)
            _config.WriteFull(
                new ProxyConfig
                {
                    Host = Host,
                    Port = Port,
                    AuthRequired = AuthRequired,
                    Login = Login,
                    AuthEnabled = AuthEnabled,
                    KerberosEnabled = KerberosEnabled
                },
                exePath ?? "",
                [.. Rules]);

            Msg = $"\u2713 Saved (exePath: {exePath ?? "(none)"})";
            Password = "";
            Saved?.Invoke();
            _ = ClearMsgAfterDelay();
        }
        catch (Exception ex)
        {
            Msg = $"\u2717 Save failed: {ex.Message}";
        }
    }

    private async Task ClearMsgAfterDelay()
    {
        await Task.Delay(3000);
        if (Msg.Contains('\u2713'))
            Msg = "";
    }
}