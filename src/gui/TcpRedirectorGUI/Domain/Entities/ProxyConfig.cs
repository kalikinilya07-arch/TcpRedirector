namespace TcpRedirectorGUI.Domain.Entities;

public class ProxyConfig
{
    public string Host { get; set; } = "proxy.example.com";
    public int Port { get; set; } = 3128;
    public bool AuthRequired { get; set; }
    public string Login { get; set; } = string.Empty;
    public string Password { get; set; } = string.Empty;
    public bool HasPassword { get; set; }

    // (Задача №2) Шифровать ли пароль прокси через DPAPI.
    //  • true  — прежнее поведение: auth.encryptedPassword (DPAPI+Base64).
    //  • false — пароль хранится/используется «как есть» в auth.password.
    public bool EncryptPassword { get; set; } = true;

    // (Задача №1) Требовать ли токен-аутентификацию на IPC-канале (ipc.auth_enabled).
    //  • true  — прежнее поведение: служба требует .ipc_token.
    //  • false — любой локальный клиент управляет службой без токена.
    // Переносится через ProxyConfig, т.к. WriteFullV2 принимает именно его как
    // «конфиг-носитель» от ViewModel.
    public bool IpcAuthEnabled { get; set; } = true;

    // Capture target — путь к приложению, чей трафик перехватывается
    // (WinDivert фильтрует по этому пути, НЕ через RuleEngine)
    public string ExePath { get; set; } = string.Empty;

    // Authentication (Kerberos/SSPI)
    public bool KerberosEnabled { get; set; }

    // ── Per-user Kerberos auth helper (Phase 9) ──────────────────────────
    // These mirror the JSON keys added to the "auth" block in Phase 0 and are
    // consumed by the C++ service. All default to the backward-compatible
    // "feature OFF" values so pre-Phase-0 configs load unchanged.

    /// <summary>
    /// JSON: <c>auth.per_user_enabled</c> (default false). Enables per-user
    /// Kerberos auth — each proxied app authenticates as the user who launched
    /// it, via a per-user helper process, instead of the machine account.
    /// </summary>
    public bool PerUserEnabled { get; set; }

    /// <summary>
    /// JSON: <c>auth.spn</c> (default ""). Optional explicit SPN override for
    /// the proxy. Empty ⇒ the service auto-derives <c>HTTP/&lt;proxyhost&gt;</c>.
    /// </summary>
    public string Spn { get; set; } = string.Empty;

    /// <summary>
    /// JSON: <c>auth.fallback_policy</c> (enum "drop" | "system" | "error",
    /// default "drop"). What to do when per-user auth cannot be performed:
    ///   • <c>drop</c>   — drop the connection (never use the machine account);
    ///   • <c>system</c> — fall back to the service/machine account auth;
    ///   • <c>error</c>  — drop + emit an error log.
    /// The value is kept verbatim on the wire (exactly one of the three).
    /// </summary>
    public string FallbackPolicy { get; set; } = "drop";

    /// <summary>
    /// JSON: <c>auth.helper_timeout_ms</c> (default 5000). Timeout in
    /// milliseconds for the service↔helper token exchange.
    /// </summary>
    public int HelperTimeoutMs { get; set; } = 5000;
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
    public uint ActiveConnections { get; set; }
    public ulong TotalRxBytes { get; set; }
    public ulong TotalTxBytes { get; set; }
    public ulong ProxyErrors { get; set; }
    public double AvgLatencyMs { get; set; }
    public ulong UptimeSeconds { get; set; }
}

public class ServiceStatus
{
    public bool Running { get; set; }
    public bool Initialized { get; set; }

    /// <summary>
    /// Task 2: per-user Kerberos auth-component health, reported by the service
    /// in the <c>service_status</c> IPC response field <c>auth_status</c>.
    /// One of: "disabled", "active", "no_helper", "error". "disabled" means the
    /// feature is off and the GUI hides the Kerberos indicator.
    /// </summary>
    public string AuthStatus { get; set; } = "disabled";
}

public enum ServiceState
{
    Stopped,
    Running,
    Starting,
    Stopping,
    Error
}