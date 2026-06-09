namespace TcpRedirectorGUI.Domain.Entities;

public class ProxyConfig
{
    public string Host { get; set; } = "proxy.example.com";
    public int Port { get; set; } = 3128;
    public bool AuthRequired { get; set; }
    public string Login { get; set; } = string.Empty;
    public string Password { get; set; } = string.Empty;
    public bool HasPassword { get; set; }
}

public enum RuleType
{
    ProcessName = 0,
    ProcessPath = 1,
    Global = 2
}

public enum RuleAction
{
    Proxy = 0,
    Direct = 1,
    Block = 2
}

public class Rule
{
    public string Id { get; set; } = Guid.NewGuid().ToString();
    public RuleType Type { get; set; } = RuleType.ProcessName;
    public RuleAction Action { get; set; } = RuleAction.Proxy;
    public string Pattern { get; set; } = string.Empty;
    public string Description { get; set; } = string.Empty;
    public int Priority { get; set; }
    public bool Enabled { get; set; } = true;
    public DateTime Created { get; set; } = DateTime.UtcNow;
    public DateTime Modified { get; set; } = DateTime.UtcNow;
}

public enum ConnectionState
{
    Redirecting = 0,
    ConnectingToProxy = 1,
    TunnelEstablished = 2,
    Closing = 3,
    Closed = 4,
    Error = 5
}

public class ConnectionRecord
{
    public ulong Id { get; set; }
    public uint Pid { get; set; }
    public string ProcessName { get; set; } = string.Empty;
    public string DestinationHost { get; set; } = string.Empty;
    public string DestinationIp { get; set; } = string.Empty;
    public ushort DestinationPort { get; set; }
    public DateTime StartTime { get; set; } = DateTime.Now;
    public long DurationMs { get; set; }
    public ulong RxBytes { get; set; }
    public ulong TxBytes { get; set; }
    public bool ProxyEnabled { get; set; } = true;
    public ConnectionState State { get; set; } = ConnectionState.Redirecting;

    public string DurationDisplay => TimeSpan.FromMilliseconds(DurationMs).ToString(@"hh\:mm\:ss");
    public string RxDisplay => FormatBytes(RxBytes);
    public string TxDisplay => FormatBytes(TxBytes);
    public string DisplayDestination => !string.IsNullOrEmpty(DestinationHost) ? DestinationHost : DestinationIp;

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

public class LogEntry
{
    public DateTime Timestamp { get; set; }
    public int Level { get; set; }
    public string Logger { get; set; } = string.Empty;
    public string Message { get; set; } = string.Empty;

    public string LevelDisplay => Level switch
    {
        0 => "TRACE",
        1 => "DEBUG",
        2 => "INFO",
        3 => "WARN",
        4 => "ERROR",
        _ => "UNKNOWN"
    };

    public string TimeDisplay => Timestamp.ToString("HH:mm:ss.fff");
}

public class ServiceStats
{
    public ulong TotalConnections { get; set; }
    public uint ActiveConnections { get; set; }
    public ulong TotalRxBytes { get; set; }
    public ulong TotalTxBytes { get; set; }
    public ulong ProxyErrors { get; set; }
}

public class ServiceStatus
{
    public bool Running { get; set; }
    public bool Initialized { get; set; }
}

public enum ServiceState
{
    Stopped,
    Running,
    Starting,
    Stopping,
    Error
}