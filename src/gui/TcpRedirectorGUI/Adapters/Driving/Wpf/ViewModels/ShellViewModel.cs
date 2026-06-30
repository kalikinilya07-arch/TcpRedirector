using System.Collections.ObjectModel;
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
/// On startup, finds and launches the backend (TcpRedirectorService.exe --console)
/// automatically — no manual service installation needed.
/// </summary>
public partial class ShellViewModel : ObservableObject, IDisposable
{
    private readonly ITcpRedirectorService _svc;
    private readonly IServiceController _scm;
    private CancellationTokenSource? _timerCts;
    private Process? _backendProcess;
    private bool _disposed;

    public ShellViewModel(
        ITcpRedirectorService svc,
        IServiceController scm,
        SettingsViewModel settings,
        StatsViewModel stats)
    {
        _svc = svc;
        _scm = scm;
        Settings = settings;
        Stats = stats;

        // React to connection state changes
        _svc.ConnectionStateChanged += OnConnectionStateChanged;

        // Load config from disk synchronously (blocking in ctor is OK — tiny file)
        Settings.LoadFromConfig();
        StatusText = "Configured";

        // Auto-start backend and connect
        _ = AutoStartAndConnectAsync();
    }

    // ── Navigation ───────────────────────────────────

    [ObservableProperty]
    private string _activeTab = "Settings";

    [RelayCommand]
    private void Nav(string tab) => ActiveTab = tab;

    // ── Status ───────────────────────────────────────

    [ObservableProperty]
    private bool _isConnected;

    [ObservableProperty]
    private string _statusText = "Loading...";

    [ObservableProperty]
    private string _svcStatus = "Stopped";

    [ObservableProperty]
    private string _svcMsg = "";

    // ── Stats (top-level) ────────────────────────────

    [ObservableProperty]
    private uint _activeConnections;

    [ObservableProperty]
    private string _totalTraffic = "0 B";

    // ── Child ViewModels ─────────────────────────────

    public SettingsViewModel Settings { get; }
    public StatsViewModel Stats { get; }

    // ── Service lifecycle ────────────────────────────

    [RelayCommand]
    private async Task StartService()
    {
        try
        {
            SvcStatus = "Starting";
            SvcMsg = "";

            StopTimer();

            var ok = await _scm.StartServiceAsync();
            if (!ok)
            {
                SvcStatus = "Failed";
                SvcMsg = "\u2717 Start failed";
                return;
            }

            await _svc.ConnectAsync();
            StartTimer();
            SvcStatus = "Running";
            SvcMsg = "\u2713 Started";
        }
        catch (Exception ex)
        {
            SvcStatus = "Error";
            SvcMsg = $"\u2717 {ex.Message}";
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

            // Graceful stop with timeout; force-kill if timeout exceeded
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            try
            {
                var ok = await Task.Run(() => _scm.StopServiceAsync(), cts.Token);
                SvcStatus = ok ? "Stopped" : "Failed";
                SvcMsg = ok ? "\u2713 Stopped" : "\u2717 Stop failed";
            }
            catch (OperationCanceledException)
            {
                // Service is stuck — force kill
                KillServiceProcess();
                SvcStatus = "Stopped";
                SvcMsg = "\u2713 Stopped (forced)";
            }
        }
        catch
        {
            SvcStatus = "Error";
        }
        finally
        {
            _ = ClearMsgAfterDelay();
        }
    }

    // ── Auto-start backend ───────────────────────────

    /// <summary>
    /// Tries to connect to a running backend. If none is found,
    /// locates TcpRedirectorService.exe next to the GUI (or one level up)
    /// and launches it in console mode, then connects.
    /// </summary>
    private async Task AutoStartAndConnectAsync()
    {
        // 1. Try connecting to an already-running service
        try
        {
            await _svc.ConnectAsync();
            if (_svc.IsConnected)
            {
                StatusText = "Connected (existing)";
                SvcStatus = "Running";
                StartTimer();
                return;
            }
        }
        catch
        {
            // Not running — proceed to launch
        }

        // 2. Find the backend exe
        var exePath = FindBackendExe();
        if (string.IsNullOrEmpty(exePath))
        {
            StatusText = "Backend not found";
            SvcStatus = "Error";
            SvcMsg = "TcpRedirectorService.exe not found";
            return;
        }

        // 3. Launch backend process
        try
        {
            SvcStatus = "Starting";
            StatusText = "Starting backend...";

            var psi = new ProcessStartInfo
            {
                FileName = exePath,
                Arguments = "--console",
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                WindowStyle = ProcessWindowStyle.Hidden
            };

            _backendProcess = new Process { StartInfo = psi };
            // Capture stderr asynchronously for diagnostics
            var stderrBuilder = new System.Text.StringBuilder();
            _backendProcess.ErrorDataReceived += (_, e) =>
            {
                if (e.Data != null)
                    lock (stderrBuilder) stderrBuilder.AppendLine(e.Data);
            };
            _backendProcess.Start();
            _backendProcess.BeginErrorReadLine();

            // 4. Wait for the pipe to become available (max 10 seconds)
            for (int i = 0; i < 20; i++)
            {
                await Task.Delay(500);
                try
                {
                    await _svc.ConnectAsync();
                    if (_svc.IsConnected)
                    {
                        StatusText = "Connected";
                        SvcStatus = "Running";
                        StartTimer();
                        return;
                    }
                }
                catch
                {
                    // Not ready yet
                }
            }

            // Timed out — read stderr for diagnosis
            string diag;
            lock (stderrBuilder) diag = stderrBuilder.ToString();
            if (!string.IsNullOrWhiteSpace(diag))
            {
                SvcMsg = diag.TrimEnd();
            }
            else
            {
                SvcMsg = "Backend exited without error message. Check WinDivert driver installation.";
            }
            SvcStatus = "Error";
            StatusText = "Start failed";
        }
        catch (Exception ex)
        {
            SvcStatus = "Error";
            StatusText = "Start failed";
            SvcMsg = ex.Message;
        }
    }

    private static string? FindBackendExe()
    {
        // Look next to GUI executable first
        var guiDir = AppDomain.CurrentDomain.BaseDirectory;
        var candidates = new[]
        {
            Path.Combine(guiDir, "TcpRedirectorService.exe"),
            Path.Combine(guiDir, "..", "TcpRedirectorService.exe"),
            Path.Combine(guiDir, "..", "..", "TcpRedirectorService.exe"),
        };

        foreach (var c in candidates)
        {
            var full = Path.GetFullPath(c);
            if (File.Exists(full))
                return full;
        }

        return null;
    }

    // ── Polling timer ────────────────────────────────

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
        while (!ct.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(2000, ct);
                if (!_svc.IsConnected) continue;

                var stats = await _svc.GetStatsAsync();
                if (stats is not null)
                {
                    ActiveConnections = stats.ActiveConnections;
                    TotalTraffic = FormatBytes(stats.TotalRxBytes + stats.TotalTxBytes);
                    Stats.PushStats(stats);
                }

                var status = await _svc.GetServiceStatusAsync();
                if (status is not null)
                    SvcStatus = status.Running ? "Running" : "Stopped";

                var logs = await _svc.GetLogsAsync();
                if (logs.Count > 0)
                    Stats.PushLogs(logs);
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch
            {
                // Transient IPC errors — retry on next poll
            }
        }
    }

    // ── Helpers ──────────────────────────────────────

    private void OnConnectionStateChanged(bool connected)
    {
        IsConnected = connected;
        StatusText = connected ? "Connected" : "Disconnected";
    }

    private static void KillServiceProcess()
    {
        try
        {
            Process.Start("taskkill", "/f /im TcpRedirectorService.exe");
        }
        catch
        {
            // Best-effort
        }
    }

    private async Task ClearMsgAfterDelay()
    {
        await Task.Delay(5000);
        if (SvcMsg.Contains('\u2713'))
            SvcMsg = "";
    }

    private static string FormatBytes(ulong b) => b switch
    {
        >= 1_073_741_824 => $"{b / 1_073_741_824.0:F1} GB",
        >= 1_048_576 => $"{b / 1_048_576.0:F1} MB",
        >= 1_024 => $"{b / 1_024.0:F1} KB",
        _ => $"{b} B"
    };

    // ── IDisposable ──────────────────────────────────

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;

        _svc.ConnectionStateChanged -= OnConnectionStateChanged;
        StopTimer();
        _svc.Disconnect();

        // Kill the backend process we started
        if (_backendProcess is not null && !_backendProcess.HasExited)
        {
            try { _backendProcess.Kill(); } catch { }
            _backendProcess.Dispose();
            _backendProcess = null;
        }

        GC.SuppressFinalize(this);
    }
}