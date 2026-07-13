using System.Diagnostics;
using System.IO;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using TcpRedirectorGUI.Domain.Entities;
using TcpRedirectorGUI.Domain.Ports;
using TcpRedirectorGUI.Infrastructure.Ipc;

namespace TcpRedirectorGUI.Adapters.Driving.Wpf.ViewModels;

/// <summary>
/// Orchestrates the entire UI: service lifecycle, polling, navigation.
/// </summary>
public partial class ShellViewModel : ObservableObject, IDisposable
{
    private readonly ITcpRedirectorService _svc;
    private readonly IServiceController _scm;
    private readonly IConfigRepository _config;
    private CancellationTokenSource? _timerCts;
    private bool _disposed;
    private readonly int _pollIntervalMs;

    public ShellViewModel(
        ITcpRedirectorService svc,
        IServiceController scm,
        IConfigRepository config,
        SettingsViewModel settings,
        StatsViewModel stats)
    {
        _svc = svc;
        _scm = scm;
        _config = config;
        Settings = settings;
        Stats = stats;

        _pollIntervalMs = Math.Max(200, config.ReadInt("stats", "updateIntervalMs", 1000));

        _svc.ConnectionStateChanged += OnConnectionStateChanged;

        Settings.LoadFromConfig();
        StatusText = "Configured";
        GuiDiagLog("ShellViewModel ctor: configured");
    }

    // ---- Status ----

    [ObservableProperty]
    private bool _isConnected;

    [ObservableProperty]
    private string _statusText = "Loading...";

    [ObservableProperty]
    private string _svcStatus = "Stopped";

    [ObservableProperty]
    private string _svcMsg = "";

    // ---- Stats ----

    [ObservableProperty]
    private string _totalTraffic = "0 B";

    // ---- Child ViewModels ----

    public SettingsViewModel Settings { get; }
    public StatsViewModel Stats { get; }

    // ---- Service lifecycle ----

    [RelayCommand]
    private async Task StartService()
    {
        try
        {
            SvcStatus = "Starting";
            SvcMsg = "";
            GuiDiagLog("StartService: user clicked Start");

            StopTimer();

            // Check if process is already running
            bool alreadyRunning = false;
            try
            {
                var procs = Process.GetProcessesByName("TcpRedirectorService");
                alreadyRunning = procs.Length > 0;
                GuiDiagLog($"StartService: processes found: {procs.Length}");
            }
            catch (Exception ex)
            {
                GuiDiagLog($"StartService: process check error: {ex.Message}");
            }

            if (!alreadyRunning)
            {
                SvcMsg = "Launching backend...";
                var ok = await _scm.StartServiceAsync();
                GuiDiagLog($"StartService: StartServiceAsync returned {ok}");

                if (!ok)
                {
                    SvcStatus = "Failed";
                    var diag = _scm.LastStartupError;
                    if (!string.IsNullOrEmpty(diag))
                    {
                        SvcMsg = diag;
                        GuiDiagLog($"StartService: backend error: {diag}");
                    }
                    else
                    {
                        SvcMsg = "✗ Start failed — backend not found or exited";
                        GuiDiagLog("StartService: start failed, no diagnostics");
                    }
                    return;
                }
            }
            else
            {
                GuiDiagLog("StartService: process already running, trying connect...");
            }

            // Try to connect to the pipe (with retries)
            SvcMsg = "Connecting to backend...";
            for (int i = 0; i < 10; i++)
            {
                try
                {
                    await _svc.ConnectAsync();
                    GuiDiagLog($"StartService: ConnectAsync attempt {i + 1}, IsConnected={_svc.IsConnected}");
                    if (_svc.IsConnected) break;
                }
                catch (Exception ex)
                {
                    GuiDiagLog($"StartService: ConnectAsync attempt {i + 1}: {ex.GetType().Name}: {ex.Message}");
                }
                await Task.Delay(500);
            }

            if (!_svc.IsConnected)
            {
                GuiDiagLog("StartService: not connected after retries");
                SvcStatus = "Failed";
                SvcMsg = "✗ Connect failed — pipe not available";
                try
                {
                    var procs = Process.GetProcessesByName("TcpRedirectorService");
                    if (procs.Length == 0) SvcMsg += " (backend exited)";
                }
                catch { }
                return;
            }

            StartTimer();
            Settings.LoadFromConfig();
            SvcStatus = "Running";
            SvcMsg = "✓ Started";
            GuiDiagLog("StartService: SUCCESS");
        }
        catch (Exception ex)
        {
            GuiDiagLog($"StartService: exception: {ex.GetType().Name}: {ex.Message}");
            SvcStatus = "Error";
            SvcMsg = $"✗ {ex.Message}";
        }
        finally
        {
            _ = ClearMsgAfterDelay();
        }
    }

    [RelayCommand]
    private async Task StopService()
    {
        try
        {
            SvcStatus = "Stopping";
            SvcMsg = "";

            StopTimer();
            _svc.Disconnect();

            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            try
            {
                var ok = await Task.Run(() => _scm.StopServiceAsync(), cts.Token);
                SvcStatus = ok ? "Stopped" : "Failed";
                SvcMsg = ok ? "✓ Stopped" : "✗ Stop failed";
            }
            catch (OperationCanceledException)
            {
                KillServiceProcess();
                SvcStatus = "Stopped";
                SvcMsg = "✓ Stopped (forced)";
            }
        }
        catch
        {
            SvcStatus = "Error";
        }
        finally
        {
            Settings.LoadFromConfig();
            _ = ClearMsgAfterDelay();
        }
    }

    // ---- Polling timer ----

    private void StartTimer()
    {
        StopTimer();
        _timerCts = new CancellationTokenSource();
        _ = PollLoopAsync(_timerCts.Token);
    }

    private void StopTimer()
    {
        try { _timerCts?.Cancel(); } catch { }
        _timerCts?.Dispose();
        _timerCts = null;
    }

    private async Task PollLoopAsync(CancellationToken ct)
    {
        GuiDiagLog("PollLoop: started");
        while (!ct.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(_pollIntervalMs, ct);
                if (!_svc.IsConnected) continue;

                var stats = await _svc.GetStatsAsync();
                if (stats is not null)
                {
                    TotalTraffic = FormatBytes(stats.TotalRxBytes + stats.TotalTxBytes);
                    Stats.PushStats(stats);
                }
                else if (_svc is IpcClient ipc)
                {
                    var raw = ipc.LastRawResponse;
                    if (!string.IsNullOrEmpty(raw))
                        SvcMsg = $"IPC raw: {raw}";
                }

                var status = await _svc.GetServiceStatusAsync();
                if (status is not null)
                    SvcStatus = status.Running ? "Running" : "Stopped";
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (Exception ex)
            {
                SvcMsg = $"IPC err: {ex.GetType().Name}";
                GuiDiagLog($"PollLoop: IPC error: {ex.GetType().Name}: {ex.Message}");
            }
        }
        GuiDiagLog("PollLoop: stopped");
    }

    // ---- Helpers ----

    private void OnConnectionStateChanged(bool connected)
    {
        IsConnected = connected;
        StatusText = connected ? "Connected" : "Disconnected";
        GuiDiagLog($"ConnectionStateChanged: connected={connected}");
    }

    private static void KillServiceProcess()
    {
        try
        {
            Process.Start("taskkill", "/f /im TcpRedirectorService.exe");
        }
        catch { }
    }

    private async Task ClearMsgAfterDelay()
    {
        await Task.Delay(5000);
        if (SvcMsg.Contains('✓'))
            SvcMsg = "";
    }

    private static string FormatBytes(ulong b) => b switch
    {
        >= 1_073_741_824 => $"{b / 1_073_741_824.0:F1} GB",
        >= 1_048_576 => $"{b / 1_048_576.0:F1} MB",
        >= 1_024 => $"{b / 1_024.0:F1} KB",
        _ => $"{b} B"
    };

    // ---- Diagnostic logging ----

    private static void GuiDiagLog(string msg)
    {
        try
        {
            var dir = AppDomain.CurrentDomain.BaseDirectory;
            var path = Path.Combine(dir, "gui_diag.log");

            try
            {
                var fi = new FileInfo(path);
                if (fi.Exists && fi.Length > 1_000_000)
                {
                    var backup = Path.Combine(dir, "gui_diag_prev.log");
                    if (File.Exists(backup)) File.Delete(backup);
                    File.Move(path, backup);
                }
            }
            catch { }

            File.AppendAllText(path, $"{DateTime.Now:HH:mm:ss.fff} [GUI] {msg}\n");
        }
        catch { }
    }

    // ---- IDisposable ----

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;

        GuiDiagLog("ShellViewModel: disposing...");
        _svc.ConnectionStateChanged -= OnConnectionStateChanged;
        StopTimer();
        _svc.Disconnect();

        GC.SuppressFinalize(this);
        GuiDiagLog("ShellViewModel: disposed");
    }
}
