using TcpRedirectorGUI.Domain.Entities;

namespace TcpRedirectorGUI.Domain.Ports;

/// <summary>
/// Inbound port — primary interface to the TcpRedirector service
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

    // Service control
    Task<ServiceStatus?> GetServiceStatusAsync();

    // Log level
    Task<bool> SetLogLevelAsync(int level);

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

/// <summary>
/// Outbound port — service control via SCM
/// </summary>
public interface IServiceController
{
    Task<bool> StartServiceAsync();
    Task<bool> StopServiceAsync();
    Task<bool> RestartServiceAsync();
    Task<ServiceState> GetStateAsync();
    bool IsAdministrator();
    /// <summary>
    /// Last startup error captured from backend stderr.
    /// Returns null after being read (one-shot).
    /// </summary>
    string? LastStartupError { get; }
}