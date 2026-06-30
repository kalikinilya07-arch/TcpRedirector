using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using TcpRedirectorGUI.Domain.Entities;
using TcpRedirectorGUI.Domain.Ports;

namespace TcpRedirectorGUI.Infrastructure.Ipc;

/// <summary>
/// Thread-safe Named Pipe client for communicating with the TcpRedirector service.
/// All pipe operations are serialized via SemaphoreSlim.
/// Config file operations are delegated to IConfigRepository.
/// </summary>
public class IpcClient : ITcpRedirectorService, IDisposable
{
    private readonly IConfigRepository _config;
    private NamedPipeClientStream? _pipe;
    private readonly SemaphoreSlim _lock = new(1, 1);
    private readonly JsonSerializerOptions _json = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        PropertyNameCaseInsensitive = true
    };

    public IpcClient(IConfigRepository config)
    {
        _config = config;
    }

    public bool IsConnected => _pipe?.IsConnected ?? false;
    public event Action<bool>? ConnectionStateChanged;
    public event Action<List<ConnectionRecord>>? ConnectionsUpdated = delegate { };
    public event Action<LogEntry>? LogEntryReceived = delegate { };
    public event Action<ServiceStats>? StatsUpdated = delegate { };

    public async Task ConnectAsync()
    {
        await _lock.WaitAsync();
        try
        {
            _pipe?.Dispose();
            _pipe = new NamedPipeClientStream(".", "TcpRedirectorService",
                PipeDirection.InOut, PipeOptions.Asynchronous);
            await _pipe.ConnectAsync(2000);
            ConnectionStateChanged?.Invoke(true);
        }
        catch
        {
            ConnectionStateChanged?.Invoke(false);
        }
        finally
        {
            _lock.Release();
        }
    }

    public void Disconnect()
    {
        // Don't lock — in-flight Call() will fail with IOException, release lock itself
        try { _pipe?.Dispose(); } catch { }
        _pipe = null;
        ConnectionStateChanged?.Invoke(false);
    }

    public async Task<ProxyConfig?> GetConfigAsync()
    {
        var r = await Call("get_config");
        if (r?.TryGetProperty("data", out var d) != true) return null;

        var cfg = new ProxyConfig();
        if (d.TryGetProperty("proxy", out var p))
        {
            cfg.Host = p.GetProperty("host").GetString() ?? "";
            cfg.Port = (int)p.GetProperty("port").GetUInt16();
            cfg.AuthRequired = p.GetProperty("auth_required").GetBoolean();
            cfg.HasPassword = p.GetProperty("has_password").GetBoolean();
        }

        // Backend IPC doesn't return login/auth fields — read from config.json
        cfg.Login = _config.ReadString("auth", "username");
        cfg.KerberosEnabled = _config.ReadBool("auth", "kerberos");
        return cfg;
    }

    public async Task<bool> SetConfigAsync(ProxyConfig config)
    {
        var r = await Call("set_config", new
        {
            host = config.Host,
            port = config.Port,
            auth_required = config.AuthRequired,
            login = config.Login,
            set_password = !string.IsNullOrEmpty(config.Password),
            password = config.Password,
            kerberos = config.KerberosEnabled,
            auth_username = config.Login
        });
        return r?.GetProperty("status").GetString() == "success";
    }

    public async Task<List<Rule>> GetRulesAsync()
    {
        var r = await Call("get_rules");
        var list = new List<Rule>();
        if (r?.TryGetProperty("data", out var d) == true &&
            d.TryGetProperty("rules", out var a))
        {
            foreach (var i in a.EnumerateArray())
            {
                list.Add(new Rule
                {
                    Id = i.GetProperty("id").GetString() ?? "",
                    Pattern = i.GetProperty("pattern").GetString() ?? "",
                    Description = i.GetProperty("description").GetString() ?? "",
                    Priority = i.GetProperty("priority").GetInt32(),
                    Enabled = i.GetProperty("enabled").GetBoolean(),
                    Type = (RuleType)i.GetProperty("type").GetInt32(),
                    Action = (RuleAction)i.GetProperty("action").GetInt32()
                });
            }
        }
        return list;
    }

    public async Task<bool> SetRulesAsync(List<Rule> rules)
    {
        var r = await Call("set_rules", new
        {
            rules = rules.Select(x => new
            {
                id = x.Id,
                pattern = x.Pattern,
                description = x.Description,
                priority = x.Priority,
                enabled = x.Enabled,
                type = (int)x.Type,
                action = (int)x.Action
            })
        });
        return r?.GetProperty("status").GetString() == "success";
    }

    public async Task<List<ConnectionRecord>> GetConnectionsAsync()
    {
        var r = await Call("get_connections");
        var list = new List<ConnectionRecord>();
        if (r?.TryGetProperty("data", out var d) == true &&
            d.TryGetProperty("connections", out var a))
        {
            foreach (var i in a.EnumerateArray())
            {
                list.Add(new ConnectionRecord
                {
                    Id = i.GetProperty("id").GetUInt64(),
                    Pid = i.GetProperty("pid").GetUInt32(),
                    ProcessName = i.GetProperty("process_name").GetString() ?? "",
                    DestinationHost = i.GetProperty("destination_host").GetString() ?? "",
                    DestinationIp = i.GetProperty("destination_ip").GetString() ?? "",
                    DestinationPort = i.GetProperty("destination_port").GetUInt16(),
                    RxBytes = i.GetProperty("rx_bytes").GetUInt64(),
                    TxBytes = i.GetProperty("tx_bytes").GetUInt64(),
                    DurationMs = i.GetProperty("duration_ms").GetInt64(),
                    State = (ConnectionState)i.GetProperty("state").GetInt32(),
                    ProxyEnabled = i.GetProperty("proxy_enabled").GetBoolean()
                });
            }
        }
        return list;
    }

    public async Task<List<LogEntry>> GetLogsAsync()
    {
        var r = await Call("get_logs");
        var list = new List<LogEntry>();
        if (r?.TryGetProperty("data", out var d) == true &&
            d.TryGetProperty("logs", out var a))
        {
            foreach (var i in a.EnumerateArray())
            {
                list.Add(new LogEntry
                {
                    Timestamp = DateTimeOffset
                        .FromUnixTimeMilliseconds(i.GetProperty("timestamp").GetInt64())
                        .DateTime,
                    Level = i.GetProperty("level").GetInt32(),
                    Logger = i.GetProperty("logger").GetString() ?? "",
                    Message = i.GetProperty("message").GetString() ?? ""
                });
            }
        }
        return list;
    }

    public async Task<ServiceStats?> GetStatsAsync()
    {
        var r = await Call("get_stats");
        if (r?.TryGetProperty("data", out var d) == true)
        {
            return new ServiceStats
            {
                TotalConnections = d.GetProperty("total_connections").GetUInt64(),
                ActiveConnections = d.GetProperty("active_connections").GetUInt32(),
                TotalRxBytes = d.GetProperty("total_rx_bytes").GetUInt64(),
                TotalTxBytes = d.GetProperty("total_tx_bytes").GetUInt64()
            };
        }
        return null;
    }

    public async Task<ServiceStatus?> GetServiceStatusAsync()
    {
        var r = await Call("service_status");
        if (r?.TryGetProperty("data", out var d) == true)
        {
            return new ServiceStatus
            {
                Running = d.GetProperty("running").GetBoolean(),
                Initialized = d.GetProperty("initialized").GetBoolean()
            };
        }
        return null;
    }

    public async Task<bool> SetLogLevelAsync(int level)
    {
        var r = await Call("set_log_level", new { level });
        return r?.GetProperty("status").GetString() == "success";
    }

    private async Task<JsonElement?> Call(string method, object? p = null)
    {
        await _lock.WaitAsync();
        try
        {
            if (_pipe is null || !_pipe.IsConnected) return null;

            var paramsJson = p is not null
                ? JsonSerializer.Serialize(p, _json)
                : "{}";

            var req = JsonSerializer.Serialize(new
            {
                type = "request",
                id = Guid.NewGuid().ToString(),
                method,
                @params = paramsJson
            });

            var buf = Encoding.UTF8.GetBytes(req);
            await _pipe.WriteAsync(buf);
            await _pipe.FlushAsync();

            var rbuf = new byte[65536];
            using var cts = new CancellationTokenSource(5000);
            var n = await _pipe.ReadAsync(rbuf, 0, rbuf.Length, cts.Token);

            return JsonSerializer.Deserialize<JsonElement>(
                Encoding.UTF8.GetString(rbuf, 0, n), _json);
        }
        catch
        {
            return null;
        }
        finally
        {
            _lock.Release();
        }
    }

    public void Dispose()
    {
        Disconnect();
        GC.SuppressFinalize(this);
    }
}