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
    private const int MaxGraphPoints = 60; // 60 seconds @ 1 point/sec

    public StatsViewModel(ITcpRedirectorService svc, IConfigRepository config)
    {
        _svc = svc;
        _config = config;
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

            // Polling every ~1s, so delta == bytes/second
            var rxRate = rxDelta;
            var txRate = txDelta;

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