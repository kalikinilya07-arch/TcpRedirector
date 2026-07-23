using System.Collections.ObjectModel;
using System.Globalization;
using System.Windows;
using CommunityToolkit.Mvvm.ComponentModel;
using TcpRedirectorGUI.Adapters.Driving.Wpf.Controls;
using TcpRedirectorGUI.Domain.Entities;
using TcpRedirectorGUI.Domain.Ports;

namespace TcpRedirectorGUI.Adapters.Driving.Wpf.ViewModels;

public partial class StatsViewModel : ObservableObject
{
    private readonly ITcpRedirectorService _svc;
    private readonly IConfigRepository _config;
    private int _maxGraphPoints;

    public StatsViewModel(ITcpRedirectorService svc, IConfigRepository config)
    {
        _svc = svc;
        _config = config;
        _graphWindowSec = config.ReadInt("stats", "graphWindowSec", 3600);
        _maxGraphPoints = Math.Max(60, _graphWindowSec); // at least 60 points
    }

    // ── Traffic Graph ────────────────────────────────
    [ObservableProperty]
    private ObservableCollection<TrafficPoint> _rxPoints = new();

    [ObservableProperty]
    private ObservableCollection<TrafficPoint> _txPoints = new();

    /// <summary>Graph time window in seconds (from config).</summary>
    private readonly int _graphWindowSec;
    public int GraphWindowSec => _graphWindowSec;

    [ObservableProperty]
    private string _totalRx = "0 B";

    [ObservableProperty]
    private string _totalTx = "0 B";

    [ObservableProperty]
    private uint _activeConnections;

    [ObservableProperty]
    private ulong _proxyErrors;

    [ObservableProperty]
    private string _avgLatency = "0 ms";

    [ObservableProperty]
    private string _uptime = "0m";

    [ObservableProperty]
    private string _connectionRate = "0/min";

    [ObservableProperty]
    private string _dataRate = "0 B/s";

    private ulong _prevRx;
    private ulong _prevTx;
    private uint _prevActive;
    private double _prevConnRateTime;
    private double _prevDataRateTime;

    /// <summary>
    /// Push new stats from polling. Updates graph, totals, and derived metrics.
    /// </summary>
    public void PushStats(ServiceStats stats)
    {
        var dispatcher = Application.Current?.Dispatcher;
        if (dispatcher != null)
        {
            _ = dispatcher.BeginInvoke(() => ApplyStats(stats));
        }
        else
        {
            ApplyStats(stats);
        }
    }

    private void ApplyStats(ServiceStats stats)
    {
        ActiveConnections = stats.ActiveConnections;
        ProxyErrors = stats.ProxyErrors;
        AvgLatency = stats.AvgLatencyMs > 0 ? $"{stats.AvgLatencyMs:F0} ms" : "—";
        TotalRx = FormatBytes(stats.TotalRxBytes);
        TotalTx = FormatBytes(stats.TotalTxBytes);

        // Connection rate: new connections per minute (clamped to non-negative)
        var nowSec = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() / 1000.0;
        if (_prevConnRateTime > 0) {
            double dt = nowSec - _prevConnRateTime;
            if (dt > 0.1) {
                double ratePerSec = (double)(int)(stats.ActiveConnections - _prevActive) / dt;
                double ratePerMin = Math.Max(0, ratePerSec * 60.0);
                ConnectionRate = $"{ratePerMin:F0}/min";
            }
        }
        _prevActive = stats.ActiveConnections;
        _prevConnRateTime = nowSec;

        // Data rate: bytes per second with human-readable units
        if (_prevDataRateTime > 0) {
            double dt = nowSec - _prevDataRateTime;
            if (dt > 0.1) {
                ulong totalDelta = (stats.TotalRxBytes - _prevRx) + (stats.TotalTxBytes - _prevTx);
                double bytesPerSec = Math.Max(0, (double)totalDelta / dt);
                DataRate = FormatByteRate(bytesPerSec);
            }
        }
        _prevDataRateTime = nowSec;

        Uptime = FormatUptime(stats.UptimeSeconds);

        // Calculate bytes per second delta
        var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var rxDelta = stats.TotalRxBytes - _prevRx;
        var txDelta = stats.TotalTxBytes - _prevTx;
        _prevRx = stats.TotalRxBytes;
        _prevTx = stats.TotalTxBytes;

        // Polling every ~1s, so delta == bytes/second
        var rxRate = rxDelta;
        var txRate = txDelta;

        RxPoints.Add(new TrafficPoint(now, rxRate));
        TxPoints.Add(new TrafficPoint(now, txRate));

        while (RxPoints.Count > _maxGraphPoints)
            RxPoints.RemoveAt(0);
        while (TxPoints.Count > _maxGraphPoints)
            TxPoints.RemoveAt(0);
    }

    // ── Helpers ──────────────────────────────────────
    private static string FormatBytes(ulong bytes)
    {
        return bytes switch
        {
            >= 1_073_741_824 => (bytes / 1_073_741_824.0).ToString("F1", CultureInfo.InvariantCulture) + " GB",
            >= 1_048_576 => (bytes / 1_048_576.0).ToString("F1", CultureInfo.InvariantCulture) + " MB",
            >= 1_024 => (bytes / 1_024.0).ToString("F1", CultureInfo.InvariantCulture) + " KB",
            _ => bytes.ToString(CultureInfo.InvariantCulture) + " B"
        };
    }

    private static string FormatByteRate(double bytesPerSec)
    {
        return bytesPerSec switch
        {
            >= 1_048_576 => (bytesPerSec / 1_048_576.0).ToString("F1", CultureInfo.InvariantCulture) + " MB/s",
            >= 1_024 => (bytesPerSec / 1_024.0).ToString("F1", CultureInfo.InvariantCulture) + " KB/s",
            >= 1 => bytesPerSec.ToString("F0", CultureInfo.InvariantCulture) + " B/s",
            _ => "0 B/s"
        };
    }

    private static string FormatUptime(ulong seconds)
    {
        if (seconds < 60) return $"{seconds}s";
        if (seconds < 3600) return $"{seconds / 60}m {seconds % 60}s";
        return $"{seconds / 3600}h {(seconds % 3600) / 60}m";
    }
}