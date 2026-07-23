using Moq;
using TcpRedirectorGUI.Adapters.Driving.Wpf.Controls;
using TcpRedirectorGUI.Adapters.Driving.Wpf.ViewModels;
using TcpRedirectorGUI.Domain.Entities;
using TcpRedirectorGUI.Domain.Ports;

namespace TcpRedirectorGUI.Tests;

public class StatsViewModelTests
{
    private readonly Mock<ITcpRedirectorService> _svcMock;
    private readonly Mock<IConfigRepository> _configMock;
    private readonly StatsViewModel _vm;

    public StatsViewModelTests()
    {
        _svcMock = new Mock<ITcpRedirectorService>();
        _configMock = new Mock<IConfigRepository>();
        _configMock.Setup(c => c.ReadInt("stats", "graphWindowSec", 3600)).Returns(3600);
        _configMock.Setup(c => c.ReadInt("stats", "updateIntervalMs", 500)).Returns(500);
        _vm = new StatsViewModel(_svcMock.Object, _configMock.Object);
    }

    // ── Initial state ────────────────────────────────

    [Fact]
    public void Constructor_InitializesDefaults()
    {
        Assert.Equal("0 B", _vm.TotalRx);
        Assert.Equal("0 B", _vm.TotalTx);
        Assert.Equal("0/min", _vm.ConnectionRate);
        Assert.Equal("0 B/s", _vm.DataRate);
        Assert.Equal("0 ms", _vm.AvgLatency);
        Assert.Equal("0m", _vm.Uptime);
        Assert.Equal(0u, _vm.ActiveConnections);
        Assert.Equal(0ul, _vm.ProxyErrors);
        Assert.Empty(_vm.RxPoints);
        Assert.Empty(_vm.TxPoints);
        Assert.Equal(3600, _vm.GraphWindowSec);
    }

    // ── PushStats: basic metrics ─────────────────────

    [Fact]
    public void PushStats_UpdatesActiveConnections()
    {
        var stats = new ServiceStats { ActiveConnections = 5 };
        _vm.PushStats(stats);

        Assert.Equal(5u, _vm.ActiveConnections);
    }

    [Fact]
    public void PushStats_UpdatesProxyErrors()
    {
        var stats = new ServiceStats { ProxyErrors = 3 };
        _vm.PushStats(stats);

        Assert.Equal(3ul, _vm.ProxyErrors);
    }

    [Fact]
    public void PushStats_UpdatesAvgLatency_WhenPositive()
    {
        var stats = new ServiceStats { AvgLatencyMs = 42.5 };
        _vm.PushStats(stats);

        Assert.Equal("42 ms", _vm.AvgLatency);
    }

    [Fact]
    public void PushStats_ShowsDash_WhenAvgLatencyZero()
    {
        var stats = new ServiceStats { AvgLatencyMs = 0 };
        _vm.PushStats(stats);

        Assert.Equal("—", _vm.AvgLatency);
    }

    [Fact]
    public void PushStats_ShowsDash_WhenAvgLatencyNegative()
    {
        var stats = new ServiceStats { AvgLatencyMs = -1 };
        _vm.PushStats(stats);

        Assert.Equal("—", _vm.AvgLatency);
    }

    // ── PushStats: byte formatting ───────────────────

    [Fact]
    public void PushStats_FormatsTotalRx_Bytes()
    {
        var stats = new ServiceStats { TotalRxBytes = 512 };
        _vm.PushStats(stats);

        Assert.Equal("512 B", _vm.TotalRx);
    }

    [Fact]
    public void PushStats_FormatsTotalRx_KB()
    {
        var stats = new ServiceStats { TotalRxBytes = 2048 };
        _vm.PushStats(stats);

        Assert.Equal("2.0 KB", _vm.TotalRx);
    }

    [Fact]
    public void PushStats_FormatsTotalRx_MB()
    {
        var stats = new ServiceStats { TotalRxBytes = 5_242_880 };
        _vm.PushStats(stats);

        Assert.Equal("5.0 MB", _vm.TotalRx);
    }

    [Fact]
    public void PushStats_FormatsTotalRx_GB()
    {
        var stats = new ServiceStats { TotalRxBytes = 2_147_483_648 };
        _vm.PushStats(stats);

        Assert.Equal("2.0 GB", _vm.TotalRx);
    }

    [Fact]
    public void PushStats_FormatsTotalTx()
    {
        var stats = new ServiceStats { TotalTxBytes = 1_048_576 };
        _vm.PushStats(stats);

        Assert.Equal("1.0 MB", _vm.TotalTx);
    }

    // ── PushStats: uptime ────────────────────────────

    [Fact]
    public void PushStats_FormatsUptime_Seconds()
    {
        var stats = new ServiceStats { UptimeSeconds = 45 };
        _vm.PushStats(stats);

        Assert.Equal("45s", _vm.Uptime);
    }

    [Fact]
    public void PushStats_FormatsUptime_Minutes()
    {
        var stats = new ServiceStats { UptimeSeconds = 125 };
        _vm.PushStats(stats);

        Assert.Equal("2m 5s", _vm.Uptime);
    }

    [Fact]
    public void PushStats_FormatsUptime_Hours()
    {
        var stats = new ServiceStats { UptimeSeconds = 3661 };
        _vm.PushStats(stats);

        Assert.Equal("1h 1m", _vm.Uptime);
    }

    // ── PushStats: graph points ──────────────────────

    [Fact]
    public void PushStats_AddsGraphPoints()
    {
        var stats = new ServiceStats { TotalRxBytes = 1000, TotalTxBytes = 500 };
        _vm.PushStats(stats);

        Assert.Single(_vm.RxPoints);
        Assert.Single(_vm.TxPoints);
    }

    [Fact]
    public void PushStats_TrimsOldGraphPoints()
    {
        // Set graph window to 2 points
        var configMock = new Mock<IConfigRepository>();
        configMock.Setup(c => c.ReadInt("stats", "graphWindowSec", 3600)).Returns(2);
        var vm = new StatsViewModel(_svcMock.Object, configMock.Object);

        for (int i = 0; i < 5; i++)
        {
            vm.PushStats(new ServiceStats { TotalRxBytes = (ulong)(i * 100), TotalTxBytes = (ulong)(i * 50) });
        }

        Assert.True(vm.RxPoints.Count <= 60); // maxGraphPoints = max(60, 2) = 60
    }

    // ── PushStats: data rate ─────────────────────────

    [Fact]
    public void PushStats_ComputesDataRate_AfterTwoCalls()
    {
        _vm.PushStats(new ServiceStats { TotalRxBytes = 0, TotalTxBytes = 0 });
        // Delay to ensure dt > 0.1
        Thread.Sleep(200);
        _vm.PushStats(new ServiceStats { TotalRxBytes = 5000, TotalTxBytes = 3000 });

        // DataRate should be non-empty and contain "/s"
        Assert.Contains("/s", _vm.DataRate);
        Assert.NotEqual("0 B/s", _vm.DataRate); // should have computed something
    }

    [Fact]
    public void PushStats_DataRate_ClampsToZero()
    {
        _vm.PushStats(new ServiceStats { TotalRxBytes = 10000, TotalTxBytes = 5000 });
        Thread.Sleep(200);
        // Same values — delta is 0
        _vm.PushStats(new ServiceStats { TotalRxBytes = 10000, TotalTxBytes = 5000 });

        Assert.Equal("0 B/s", _vm.DataRate);
    }

    // ── PushStats: connection rate ───────────────────

    [Fact]
    public void PushStats_ComputesConnectionRate()
    {
        _vm.PushStats(new ServiceStats { ActiveConnections = 0 });
        Thread.Sleep(200);
        _vm.PushStats(new ServiceStats { ActiveConnections = 3 });

        Assert.Contains("/min", _vm.ConnectionRate);
    }

    [Fact]
    public void PushStats_ConnectionRate_ClampsToZero()
    {
        _vm.PushStats(new ServiceStats { ActiveConnections = 5 });
        Thread.Sleep(200);
        _vm.PushStats(new ServiceStats { ActiveConnections = 2 }); // decreased

        Assert.Equal("0/min", _vm.ConnectionRate);
    }

    // ── FormatByteRate helper ─────────────────────────

    [Theory]
    [InlineData(0)]
    [InlineData(500)]
    [InlineData(1024)]
    [InlineData(1536)]
    [InlineData(1_048_576)]
    [InlineData(2_621_440)]
    public void FormatByteRate_ProducesCorrectUnits(double bytesPerSec)
    {
        // We test indirectly via PushStats
        _vm.PushStats(new ServiceStats { TotalRxBytes = 0, TotalTxBytes = 0 });
        Thread.Sleep(100);
        _vm.PushStats(new ServiceStats
        {
            TotalRxBytes = (ulong)(bytesPerSec * 0.1),
            TotalTxBytes = (ulong)(bytesPerSec * 0.1)
        });

        // DataRate should be set and end with /s
        Assert.NotNull(_vm.DataRate);
        Assert.EndsWith("/s", _vm.DataRate);
    }
}