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
    private readonly IConfigRepository _config;
    private CancellationTokenSource? _timerCts;
    private bool _disposed;
    private readonly int _pollIntervalMs;

    public ShellViewModel(
        ITcpRedirectorService svc,
        IConfigRepository config,
        SettingsViewModel settings,
        StatsViewModel stats)
    {
        _svc = svc;
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
    private bool _isServiceAvailable;

    [ObservableProperty]
    private string _statusText = "Loading...";

    [ObservableProperty]
    private string _svcStatus = "Disconnected";

    [ObservableProperty]
    private string _svcMsg = "";

    [ObservableProperty]
    private string _serviceStatusText = "";

    // ---- Capture control ----

    [ObservableProperty]
    private bool _isCaptureRunning;

    [ObservableProperty]
    private string _captureMsg = "";

    // ---- Stats ----

    [ObservableProperty]
    private string _totalTraffic = "0 B";

    // ---- Child ViewModels ----

    public SettingsViewModel Settings { get; }
    public StatsViewModel Stats { get; }

    // ---- Service lifecycle ----

    /// v1.1.0: GUI is a pure IPC client. Service is managed by Windows SCM.
    [RelayCommand]
    private async Task ConnectToServiceAsync()
    {
        try
        {
            SvcStatus = "Connecting";
            SvcMsg = "";
            GuiDiagLog("ConnectToService: attempting connection...");

            StopTimer();

            for (int i = 0; i < 10; i++)
            {
                try
                {
                    await _svc.ConnectAsync();
                    GuiDiagLog($"ConnectToService: attempt {i + 1}, IsConnected={_svc.IsConnected}");
                    if (_svc.IsConnected) break;
                }
                catch (Exception ex)
                {
                    GuiDiagLog($"ConnectToService: attempt {i + 1}: {ex.GetType().Name}: {ex.Message}");
                }
                await Task.Delay(500);
            }

            if (!_svc.IsConnected)
            {
                GuiDiagLog("ConnectToService: not connected after retries");
                SvcStatus = "Unavailable";
                IsServiceAvailable = false;
                ServiceStatusText = "🔴 Service unavailable";
                SvcMsg = "Служба TcpRedirector недоступна. Обратитесь к системному администратору.";
                return;
            }

            StartTimer();
            Settings.LoadFromConfig();
            IsCaptureRunning = true;  // service starts capture automatically
            SvcStatus = "Running";
            IsServiceAvailable = true;
            ServiceStatusText = "🟢 Service running";
            SvcMsg = "✓ Connected";
            GuiDiagLog("ConnectToService: SUCCESS");
        }
        catch (Exception ex)
        {
            GuiDiagLog($"ConnectToService: exception: {ex.GetType().Name}: {ex.Message}");
            SvcStatus = "Error";
            IsServiceAvailable = false;
            ServiceStatusText = "🔴 Service unavailable";
            SvcMsg = $"✗ {ex.Message}";
        }
        finally
        {
            _ = ClearMsgAfterDelay();
        }
    }

    // ---- Capture control ----

    /// <summary>Start WinDivert packet capture (service is already running).</summary>
    [RelayCommand]
    private async Task StartCaptureAsync()
    {
        try
        {
            CaptureMsg = "Starting...";
            var ok = await _svc.StartCaptureAsync();
            if (ok)
            {
                IsCaptureRunning = true;
                CaptureMsg = "✓ Capture running";
                GuiDiagLog("StartCapture: OK");
            }
            else
            {
                CaptureMsg = "✗ Failed to start";
                GuiDiagLog("StartCapture: FAILED");
            }
        }
        catch (Exception ex)
        {
            CaptureMsg = $"✗ {ex.Message}";
            GuiDiagLog($"StartCapture: exception: {ex.Message}");
        }
        finally { _ = ClearCaptureMsgAfterDelay(); }
    }

    /// <summary>Stop WinDivert packet capture (service keeps running).</summary>
    [RelayCommand]
    private async Task StopCaptureAsync()
    {
        try
        {
            CaptureMsg = "Stopping...";
            var ok = await _svc.StopCaptureAsync();
            if (ok)
            {
                IsCaptureRunning = false;
                CaptureMsg = "✓ Capture stopped";
                GuiDiagLog("StopCapture: OK");
            }
            else
            {
                CaptureMsg = "✗ Failed to stop";
                GuiDiagLog("StopCapture: FAILED");
            }
        }
        catch (Exception ex)
        {
            CaptureMsg = $"✗ {ex.Message}";
            GuiDiagLog($"StopCapture: exception: {ex.Message}");
        }
        finally { _ = ClearCaptureMsgAfterDelay(); }
    }

    /// <summary>Reload config.json + reapply rules/proxy without service restart.</summary>
    [RelayCommand]
    private async Task ReloadConfigAsync()
    {
        try
        {
            CaptureMsg = "Reloading...";
            var ok = await _svc.ReloadConfigAsync();
            if (ok)
            {
                CaptureMsg = "✓ Config reloaded";
                Settings.LoadFromConfig();
                GuiDiagLog("ReloadConfig: OK");
            }
            else
            {
                CaptureMsg = "✗ Reload failed";
                GuiDiagLog("ReloadConfig: FAILED");
            }
        }
        catch (Exception ex)
        {
            CaptureMsg = $"✗ {ex.Message}";
            GuiDiagLog($"ReloadConfig: exception: {ex.Message}");
        }
        finally { _ = ClearCaptureMsgAfterDelay(); }
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
        IsServiceAvailable = connected;
        ServiceStatusText = connected ? "🟢 Service running" : "🔴 Service unavailable";
        StatusText = connected ? "Connected" : "Disconnected";
        if (!connected)
        {
            StopTimer();
            SvcStatus = "Unavailable";
        }
        GuiDiagLog($"ConnectionStateChanged: connected={connected}");
    }

    private async Task ClearMsgAfterDelay()
    {
        await Task.Delay(5000);
        if (SvcMsg.Contains('✓'))
            SvcMsg = "";
    }

    private async Task ClearCaptureMsgAfterDelay()
    {
        await Task.Delay(4000);
        if (CaptureMsg.Contains('✓'))
            CaptureMsg = "";
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
