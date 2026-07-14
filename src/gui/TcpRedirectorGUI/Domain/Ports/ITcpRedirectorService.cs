using TcpRedirectorGUI.Domain.Entities;

namespace TcpRedirectorGUI.Domain.Ports;

/// <summary>
/// Inbound port — primary interface to the TcpRedirector service via IPC.
/// v1.1.0: GUI is a pure IPC client. Service lifecycle is managed by Windows SCM.
/// </summary>
public interface ITcpRedirectorService
{
    // Configuration
    Task<ProxyConfig?> GetConfigAsync();
    Task<bool> SetConfigAsync(ProxyConfig config);

    // Rules
    Task<List<Rule>> GetRulesAsync();
    Task<bool> SetRulesAsync(List<Rule> rules);

    // Monitoring
    Task<List<ConnectionRecord>> GetConnectionsAsync();
    Task<List<LogEntry>> GetLogsAsync();
    Task<ServiceStats?> GetStatsAsync();

    // Service status (v1.1.0: extended)
    Task<ServiceStatus?> GetServiceStatusAsync();
    Task<bool> PingAsync();
    Task<ServiceStatus?> GetStatusAsync();   // extended status with driver/capture info
    Task<string?> GetVersionAsync();

    // Log level
    Task<bool> SetLogLevelAsync(int level);

    // Capture control (v1.1.0: non-admin start/stop capture)
    Task<bool> StartCaptureAsync();
    Task<bool> StopCaptureAsync();

    // Config reload (v1.1.0: apply config changes without service restart)
    Task<bool> ReloadConfigAsync();

    // Connection state
    bool IsConnected { get; }
    event Action<bool>? ConnectionStateChanged;

    // Push notifications
    event Action<List<ConnectionRecord>>? ConnectionsUpdated;
    event Action<LogEntry>? LogEntryReceived;
    event Action<ServiceStats>? StatsUpdated;

    Task ConnectAsync();
    void Disconnect();
}