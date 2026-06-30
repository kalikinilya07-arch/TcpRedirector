using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using TcpRedirectorGUI.Adapters.Driving.Wpf.Controls;
using TcpRedirectorGUI.Domain.Entities;
using TcpRedirectorGUI.Domain.Ports;

namespace TcpRedirectorGUI.Adapters.Driving.Wpf.ViewModels;

public partial class StatsViewModel : ObservableObject
{
    private readonly ITcpRedirectorService _svc;
    private readonly IConfigRepository _config;
    private const int MaxLogEntries = 500;
    private const int MaxGraphPoints = 60; // 60 seconds @ 1 point/sec

    public StatsViewModel(ITcpRedirectorService svc, IConfigRepository config)
    {
        _svc = svc;
        _config = config;

        // Load saved log level from config.json
        LogLevelFilter = _config.ReadInt("log", "level", 2);
    }

    [ObservableProperty]
    private string _msg = "";

    [RelayCommand]
    private async Task SaveLogLevelAsync()
    {
        _config.WriteInt("log", "level", LogLevelFilter);
        try
        {
            if (_svc.IsConnected)
                await _svc.SetLogLevelAsync(LogLevelFilter);
        }
        catch { }
        Msg = "✓ Saved";
        _ = ClearMsgAfterDelay();
    }

    private async Task ClearMsgAfterDelay()
    {
        await Task.Delay(3000);
        Msg = "";
    }

    // ── Logs ─────────────────────────────────────────
    [ObservableProperty]
    private ObservableCollection<LogEntry> _logEntries = new();

    [ObservableProperty]
    private int _logLevelFilter = 2; // default: INFO and above (0=TRACE..4=ERROR)

    [ObservableProperty]
    private string _logFilterLabel = "INFO+";

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
            if (_svc.IsConnected)
            {
                _ = _svc.SetLogLevelAsync(value);
            }
        }
        catch
        {
            // IPC not available yet — level will apply on next IPC call
        }
    }

    // ── Traffic Graph ────────────────────────────────
    [ObservableProperty]
    private ObservableCollection<TrafficPoint> _rxPoints = new();

    [ObservableProperty]
    private ObservableCollection<TrafficPoint> _txPoints = new();

    [ObservableProperty]
    private string _totalRx = "0 B";

    [ObservableProperty]
    private string _totalTx = "0 B";

    [ObservableProperty]
    private uint _activeConnections;

    [ObservableProperty]
    private ulong _totalConnections;

    private ulong _prevRx;
    private ulong _prevTx;

    // ── Dedup log tracking ───────────────────────────
    private DateTime _lastLogTimestamp = DateTime.MinValue;
    private readonly object _logLock = new();

    // ── Public methods called from ShellViewModel timer ──

    /// <summary>
    /// Push new logs from polling. Deduplicates by timestamp.
    /// Applies LogLevelFilter: only entries with Level >= filter are shown.
    /// </summary>
    public void PushLogs(List<LogEntry> logs)
    {
        if (logs.Count == 0) return;

        lock (_logLock)
        {
            // Find the first index that is newer than last seen timestamp
            int startIdx = 0;
            for (int i = logs.Count - 1; i >= 0; i--)
            {
                if (logs[i].Timestamp <= _lastLogTimestamp)
                {
                    startIdx = i + 1;
                    break;
                }
            }

            if (startIdx >= logs.Count) return;

            // Update last timestamp from the newest log in this batch
            _lastLogTimestamp = logs[logs.Count - 1].Timestamp;

            // Add new logs via dispatcher, applying filter
            _ = App.Current.Dispatcher.BeginInvoke(() =>
            {
                for (int i = startIdx; i < logs.Count; i++)
                {
                    var entry = logs[i];
                    // LogLevelFilter: 0=TRACE+, 1=DEBUG+, 2=INFO+, 3=WARN+, 4=ERROR+
                    // Show entry if its level >= filter value
                    if (entry.Level >= LogLevelFilter)
                    {
                        LogEntries.Add(entry);
                    }
                }

                // Trim to MaxLogEntries
                while (LogEntries.Count > MaxLogEntries)
                    LogEntries.RemoveAt(0);
            });
        }
    }

    /// <summary>
    /// Push new stats from polling. Updates graph and totals.
    /// </summary>
    public void PushStats(ServiceStats stats)
    {
        _ = App.Current.Dispatcher.BeginInvoke(() =>
        {
            ActiveConnections = stats.ActiveConnections;
            TotalConnections = stats.TotalConnections;
            TotalRx = FormatBytes(stats.TotalRxBytes);
            TotalTx = FormatBytes(stats.TotalTxBytes);

            // Calculate bytes per second delta
            var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            var rxDelta = stats.TotalRxBytes - _prevRx;
            var txDelta = stats.TotalTxBytes - _prevTx;
            _prevRx = stats.TotalRxBytes;
            _prevTx = stats.TotalTxBytes;

            // Approximate B/s (polling every ~2s)
            var rxRate = rxDelta / 2;
            var txRate = txDelta / 2;

            RxPoints.Add(new TrafficPoint(now, rxRate));
            TxPoints.Add(new TrafficPoint(now, txRate));

            while (RxPoints.Count > MaxGraphPoints)
                RxPoints.RemoveAt(0);
            while (TxPoints.Count > MaxGraphPoints)
                TxPoints.RemoveAt(0);
        });
    }

    // ── Helpers ──────────────────────────────────────
    private static string FormatBytes(ulong bytes)
    {
        return bytes switch
        {
            >= 1_073_741_824 => $"{bytes / 1_073_741_824.0:F1} GB",
            >= 1_048_576 => $"{bytes / 1_048_576.0:F1} MB",
            >= 1_024 => $"{bytes / 1_024.0:F1} KB",
            _ => $"{bytes} B"
        };
    }
}