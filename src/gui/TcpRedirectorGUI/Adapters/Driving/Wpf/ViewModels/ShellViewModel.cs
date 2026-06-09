using System.Collections.ObjectModel;
using System.IO;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using TcpRedirectorGUI.Domain.Entities;
using TcpRedirectorGUI.Domain.Ports;
using TcpRedirectorGUI.Infrastructure.Ipc;
using TcpRedirectorGUI.Infrastructure.Scm;

namespace TcpRedirectorGUI.Adapters.Driving.Wpf.ViewModels;

public partial class ShellViewModel : ObservableObject
{
    private readonly ITcpRedirectorService _svc;
    private readonly IServiceController _scm;

    public ShellViewModel(ITcpRedirectorService svc, IServiceController scm)
    {
        _svc = svc;
        _scm = scm;
        _svc.ConnectionStateChanged += v => { IsConnected = v; StatusText = v ? "Connected" : "Disconnected"; };
        Proxy = new ProxySettingsVM(_svc);
        Rules = new RulesVM(_svc);
        Service = new ServiceVM(_svc, _scm);
        _ = Init();
    }

    [ObservableProperty] private bool _isConnected;
    [ObservableProperty] private string _statusText = "Starting...";
    [ObservableProperty] private bool _isServiceRunning;
    [ObservableProperty] private string _activeView = "Settings";
    [ObservableProperty] private uint _activeConnections;
    [ObservableProperty] private string _totalTraffic = "0 B";
    [ObservableProperty] private ObservableCollection<ConnectionRecord> _connList = new();

    public ProxySettingsVM Proxy { get; }
    public RulesVM Rules { get; }
    public ServiceVM Service { get; }

    private async Task Init()
    {
        try
        {
            await _svc.ConnectAsync();
            if (_svc.IsConnected) await Proxy.Load();
            _ = RunTimer();
        }
        catch { StatusText = "Connection failed"; }
    }

    private async Task RunTimer()
    {
        while (true)
        {
            try
            {
                await Task.Delay(2000);
                if (!_svc.IsConnected) { await _svc.ConnectAsync(); continue; }
                var s = await _svc.GetStatsAsync();
                if (s != null) { ActiveConnections = s.ActiveConnections; TotalTraffic = Fmt(s.TotalRxBytes + s.TotalTxBytes); }
                var c = await _svc.GetConnectionsAsync();
                if (c.Count > 0) { ConnList.Clear(); foreach (var x in c) ConnList.Add(x); }
                var st = await _svc.GetServiceStatusAsync();
                if (st != null) IsServiceRunning = st.Running;
            }
            catch { }
        }
    }

    [RelayCommand] private void Nav(string v) => ActiveView = v;
    private static string Fmt(ulong b) => b >= 1073741824 ? $"{b/1073741824.0:F1} GB" : b >= 1048576 ? $"{b/1048576.0:F1} MB" : b >= 1024 ? $"{b/1024.0:F1} KB" : $"{b} B";
}

public partial class ProxySettingsVM : ObservableObject
{
    private readonly ITcpRedirectorService _svc;
    public ProxySettingsVM(ITcpRedirectorService svc) { _svc = svc; }
    [ObservableProperty] private string _host = "";
    [ObservableProperty] private int _port = 3128;
    [ObservableProperty] private bool _auth;
    [ObservableProperty] private string _login = "";
    public string Password { get; set; } = "";
    [ObservableProperty] private string _msg = "";

    public async Task Load()
    {
        try
        {
            var c = await _svc.GetConfigAsync();
            if (c != null) { Host = c.Host; Port = c.Port; Auth = c.AuthRequired; Login = c.Login; }
        }
        catch { Msg = "Load error"; }
    }

    [RelayCommand]
    private async Task Save()
    {
        try
        {
            var ok = await _svc.SetConfigAsync(new ProxyConfig { Host = Host, Port = Port, AuthRequired = Auth, Login = Login, Password = Password });
            Msg = ok ? "\u2713 Saved" : "\u2717 Error";
            Password = "";
            await Task.Delay(3000);
            if (Msg.Contains("Saved")) Msg = "";
        }
        catch { Msg = "\u2717 Error"; }
    }
}

public partial class RulesVM : ObservableObject
{
    private readonly ITcpRedirectorService _svc;
    public RulesVM(ITcpRedirectorService svc) { _svc = svc; }
    [ObservableProperty] private ObservableCollection<Rule> _rules = new();
    [ObservableProperty] private Rule? _sel;
    [ObservableProperty] private string _msg = "";

    public void Add(string path)
    {
        try
        {
            Rules.Add(new Rule { Id = Guid.NewGuid().ToString(), Pattern = path, Description = Path.GetFileName(path), Priority = Rules.Count + 1, Enabled = true });
            _ = Save();
        }
        catch { }
    }

    [RelayCommand] private async Task Load() { try { var l = await _svc.GetRulesAsync(); Rules.Clear(); foreach (var r in l) Rules.Add(r); } catch { } }

    [RelayCommand]
    private async Task Save()
    {
        try
        {
            await _svc.SetRulesAsync(Rules.ToList());
            Msg = "\u2713 Saved";
            await Task.Delay(3000);
            if (Msg.Contains("Saved")) Msg = "";
        }
        catch { Msg = "\u2717 Error"; }
    }

    [RelayCommand] private async Task Delete(Rule? r) { if (r == null) return; try { Rules.Remove(r); await _svc.SetRulesAsync(Rules.ToList()); } catch { } }
}

public partial class ServiceVM : ObservableObject
{
    private readonly ITcpRedirectorService _svc;
    private readonly IServiceController _scm;
    public ServiceVM(ITcpRedirectorService svc, IServiceController scm) { _svc = svc; _scm = scm; }
    [ObservableProperty] private string _status = "Unknown";
    [ObservableProperty] private string _msg = "";

    [RelayCommand] private async Task Start() { try { Status = "Starting"; Status = await _scm.StartServiceAsync() ? "Running" : "Failed"; Msg = Status == "Running" ? "\u2713 Started" : "\u2717 Failed"; await ClearMsg(); } catch { Status = "Error"; } }
    [RelayCommand] private async Task Stop() { try { Status = "Stopping"; Status = await _scm.StopServiceAsync() ? "Stopped" : "Failed"; Msg = Status == "Stopped" ? "\u2713 Stopped" : "\u2717 Failed"; await ClearMsg(); } catch { Status = "Error"; } }
    [RelayCommand] private async Task Restart() { try { Status = "Restarting"; Status = await _scm.RestartServiceAsync() ? "Running" : "Failed"; Msg = Status == "Running" ? "\u2713 Restarted" : "\u2717 Failed"; await ClearMsg(); } catch { Status = "Error"; } }

    private async Task ClearMsg() { await Task.Delay(3000); if (Msg.Contains("\u2713")) Msg = ""; }
}