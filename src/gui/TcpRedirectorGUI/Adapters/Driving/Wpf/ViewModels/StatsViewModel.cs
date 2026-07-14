using System.Collections.ObjectModel;
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

    private ulong _prevRx;
    private ulong _prevTx;
    private uint _prevActive;
    private double _prevConnRateTime;
    private long _prevTrafficTimeMs;
    private bool _hasPrevTraffic;

    /// <summary>
    /// Reset the derived-metric baselines. Call when the service restarts or the
    /// connection drops so the next sample doesn't produce a bogus spike.
    /// </summary>
    public void ResetBaselines()
    {
        _ = App.Current.Dispatcher.BeginInvoke(() =>
        {
            _prevRx = 0;
            _prevTx = 0;
            _prevActive = 0;
            _prevConnRateTime = 0;
            _prevTrafficTimeMs = 0;
            _hasPrevTraffic = false;
            RxPoints.Clear();
            TxPoints.Clear();
        });
    }

    /// <summary>
    /// Push new stats from polling. Updates graph, totals, and derived metrics.
    /// </summary>
    public void PushStats(ServiceStats stats)
    {
        _ = App.Current.Dispatcher.BeginInvoke(() =>
        {
            ActiveConnections = stats.ActiveConnections;
            ProxyErrors = stats.ProxyErrors;
            AvgLatency = stats.AvgLatencyMs > 0 ? $"{stats.AvgLatencyMs:F0} ms" : "—";
            TotalRx = FormatBytes(stats.TotalRxBytes);
            TotalTx = FormatBytes(stats.TotalTxBytes);

            // Connection rate: delta active per minute (signed, no uint underflow).
            var nowSec = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() / 1000.0;
            if (_prevConnRateTime > 0) {
                double dt = nowSec - _prevConnRateTime;
                if (dt > 0.1) {
                    long activeDelta = (long)stats.ActiveConnections - _prevActive;
                    double ratePerMin = (activeDelta / dt) * 60.0;
                    ConnectionRate = $"{(ratePerMin >= 0 ? "+" : "")}{ratePerMin:F0}/min";
                }
            }
            _prevActive = stats.ActiveConnections;
            _prevConnRateTime = nowSec;
            Uptime = FormatUptime(stats.UptimeSeconds);

            // Bytes/second — normalised by the real elapsed time, guarding against
            // the first sample (no baseline yet) and counter resets on service
            // restart (totals go backwards → treat as 0, never underflow).
            var nowMs = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            if (!_hasPrevTraffic)
            {
                // First sample: establish the baseline, plot a flat zero point.
                _hasPrevTraffic = true;
                RxPoints.Add(new TrafficPoint(nowMs, 0));
                TxPoints.Add(new TrafficPoint(nowMs, 0));
            }
            else
            {
                double dtSec = (nowMs - _prevTrafficTimeMs) / 1000.0;
                if (dtSec <= 0) dtSec = 1.0;

                ulong rxDelta = stats.TotalRxBytes >= _prevRx ? stats.TotalRxBytes - _prevRx : 0;
                ulong txDelta = stats.TotalTxBytes >= _prevTx ? stats.TotalTxBytes - _prevTx : 0;

                ulong rxRate = (ulong)(rxDelta / dtSec);
                ulong txRate = (ulong)(txDelta / dtSec);

                RxPoints.Add(new TrafficPoint(nowMs, rxRate));
                TxPoints.Add(new TrafficPoint(nowMs, txRate));
            }

            _prevRx = stats.TotalRxBytes;
            _prevTx = stats.TotalTxBytes;
            _prevTrafficTimeMs = nowMs;

            while (RxPoints.Count > _maxGraphPoints)
                RxPoints.RemoveAt(0);
            while (TxPoints.Count > _maxGraphPoints)
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

    private static string FormatUptime(ulong seconds)
    {
        if (seconds < 60) return $"{seconds}s";
        if (seconds < 3600) return $"{seconds / 60}m {seconds % 60}s";
        return $"{seconds / 3600}h {(seconds % 3600) / 60}m";
    }
}