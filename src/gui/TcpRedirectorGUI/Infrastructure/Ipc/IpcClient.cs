using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using TcpRedirectorGUI.Domain.Entities;
using TcpRedirectorGUI.Domain.Ports;

namespace TcpRedirectorGUI.Infrastructure.Ipc;

/// <summary>
/// Thread-safe Named Pipe client for communicating with the TcpRedirector service.
/// Protocol: JSON over Named Pipe (\\.\pipe\TcpRedirectorService).
/// Pipe is secured with ACL: only Administrators and LOCAL_SYSTEM may connect.
/// All I/O operations are serialized via SemaphoreSlim.
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

    private const string PipeName = "TcpRedirectorService";

    public IpcClient(IConfigRepository config)
    {
        _config = config;
    }

    public bool IsConnected => _pipe?.IsConnected ?? false;
    public event Action<bool>? ConnectionStateChanged;
    public event Action<List<ConnectionRecord>>? ConnectionsUpdated = delegate { };
    public event Action<LogEntry>? LogEntryReceived = delegate { };
    public event Action<ServiceStats>? StatsUpdated = delegate { };
    public string? LastRawResponse { get; set; }

    private static void DiagLog(string msg)
    {
        try
        {
            var dir = AppDomain.CurrentDomain.BaseDirectory;
            var path = Path.Combine(dir, "gui_diag.log");
            File.AppendAllText(path, $"{DateTime.Now:HH:mm:ss.fff} [IPC] {msg}\n");
        }
        catch { }
    }

    public async Task ConnectAsync()
    {
        DiagLog($"ConnectAsync: connecting to pipe \\\\.\\pipe\\{PipeName}...");
        await _lock.WaitAsync();
        try
        {
            DisconnectInternal();
            _pipe = new NamedPipeClientStream(
                ".",
                PipeName,
                PipeDirection.InOut,
                PipeOptions.Asynchronous);

            DiagLog("ConnectAsync: calling ConnectAsync(5000)...");
            await _pipe.ConnectAsync(5000);
            DiagLog($"ConnectAsync: connected OK, IsConnected={_pipe.IsConnected}");
            ConnectionStateChanged?.Invoke(true);
        }
        catch (TimeoutException ex)
        {
            DiagLog($"ConnectAsync: TIMEOUT — {ex.Message}");
            ConnectionStateChanged?.Invoke(false);
        }
        catch (UnauthorizedAccessException ex)
        {
            DiagLog($"ConnectAsync: ACCESS DENIED — {ex.Message}");
            ConnectionStateChanged?.Invoke(false);
        }
        catch (IOException ex)
        {
            DiagLog($"ConnectAsync: IO ERROR — {ex.GetType().Name}: {ex.Message}");
            ConnectionStateChanged?.Invoke(false);
        }
        catch (Exception ex)
        {
            DiagLog($"ConnectAsync: FAILED — {ex.GetType().Name}: {ex.Message}");
            if (ex.InnerException != null)
                DiagLog($"ConnectAsync: inner: {ex.InnerException.GetType().Name}: {ex.InnerException.Message}");
            ConnectionStateChanged?.Invoke(false);
        }
        finally
        {
            _lock.Release();
        }
    }

    public void Disconnect()
    {
        DisconnectInternal();
        ConnectionStateChanged?.Invoke(false);
    }

    private void DisconnectInternal()
    {
        try { _pipe?.Dispose(); } catch { }
        _pipe = null;
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
            kerberos = config.KerberosEnabled,
            login = config.Login ?? "",
            set_password = !string.IsNullOrEmpty(config.Password)
        });
        return r?.GetProperty("status").GetString() == "success";
    }

    public async Task<List<Rule>> GetRulesAsync()
    {
        var r = await Call("get_rules");
        if (r?.TryGetProperty("data", out var d) != true) return new();
        if (!d.TryGetProperty("rules", out var arr)) return new();

        var list = new List<Rule>();
        foreach (var item in arr.EnumerateArray())
        {
            list.Add(new Rule
            {
                Id = item.GetProperty("id").GetString() ?? "",
                Pattern = item.GetProperty("pattern").GetString() ?? "",
                Description = item.GetProperty("description").GetString() ?? "",
                Priority = item.GetProperty("priority").GetInt32(),
                Enabled = item.GetProperty("enabled").GetBoolean(),
                Type = (RuleType)item.GetProperty("type").GetInt32(),
                Action = (RuleAction)item.GetProperty("action").GetInt32()
            });
        }
        return list;
    }

    public async Task<bool> SetRulesAsync(List<Rule> rules)
    {
        var r = await Call("set_rules", new { rules });
        return r?.GetProperty("status").GetString() == "success";
    }

    public async Task<List<ConnectionRecord>> GetConnectionsAsync()
    {
        var r = await Call("get_connections");
        if (r?.TryGetProperty("data", out var d) != true) return new();
        if (!d.TryGetProperty("connections", out var arr)) return new();

        var list = new List<ConnectionRecord>();
        foreach (var item in arr.EnumerateArray())
        {
            list.Add(new ConnectionRecord
            {
                Id = ulong.TryParse(item.GetProperty("id").GetString(), out var parsedId) ? parsedId : 0,
                Pid = item.GetProperty("pid").GetUInt32(),
                ProcessName = item.GetProperty("process_name").GetString() ?? "",
                DestinationHost = item.GetProperty("destination_host").GetString() ?? "",
                DestinationIp = item.GetProperty("destination_ip").GetString() ?? "",
                DestinationPort = item.GetProperty("destination_port").GetUInt16(),
                RxBytes = item.GetProperty("rx_bytes").GetUInt64(),
                TxBytes = item.GetProperty("tx_bytes").GetUInt64(),
                DurationMs = (long)item.GetProperty("duration_ms").GetUInt64(),
                State = (ConnectionState)item.GetProperty("state").GetInt32(),
                ProxyEnabled = item.GetProperty("proxy_enabled").GetBoolean()
            });
        }
        return list;
    }

    public async Task<List<LogEntry>> GetLogsAsync()
    {
        var r = await Call("get_logs");
        if (r?.TryGetProperty("data", out var d) != true) return new();
        if (!d.TryGetProperty("logs", out var arr)) return new();

        var list = new List<LogEntry>();
        foreach (var item in arr.EnumerateArray())
        {
            list.Add(new LogEntry
            {
                Timestamp = DateTimeOffset.FromUnixTimeMilliseconds((long)item.GetProperty("timestamp").GetUInt64()).DateTime,
                Level = item.GetProperty("level").GetInt32(),
                Logger = item.GetProperty("logger").GetString() ?? "",
                Message = item.GetProperty("message").GetString() ?? ""
            });
        }
        return list;
    }

    public async Task<ServiceStats?> GetStatsAsync()
    {
        var r = await Call("get_stats");
        if (r?.TryGetProperty("data", out var d) == true)
        {
            try
            {
                return new ServiceStats
                {
                    ActiveConnections = d.GetProperty("active_connections").GetUInt32(),
                    TotalRxBytes = d.GetProperty("total_rx_bytes").GetUInt64(),
                    TotalTxBytes = d.GetProperty("total_tx_bytes").GetUInt64(),
                    ProxyErrors = d.TryGetProperty("proxy_errors", out var pe) ? pe.GetUInt64() : 0,
                    AvgLatencyMs = d.TryGetProperty("avg_latency_ms", out var al) ? al.GetDouble() : 0.0,
                    UptimeSeconds = d.TryGetProperty("uptime_seconds", out var up) ? up.GetUInt64() : 0
                };
            }
            catch { return null; }
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
            if (_pipe is null || !_pipe.IsConnected)
            {
                DiagLog($"Call({method}): not connected");
                return null;
            }

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

            // Named Pipe in message mode: no newline delimiter needed.
            var reqBytes = Encoding.UTF8.GetBytes(req);
            await _pipe.WriteAsync(reqBytes, 0, reqBytes.Length);
            await _pipe.FlushAsync();

            // Read complete response message from the pipe.
            using var cts = new CancellationTokenSource(5000);
            var response = await ReadMessageAsync(_pipe, cts.Token);

            if (string.IsNullOrEmpty(response))
            {
                DiagLog($"Call({method}): empty response");
                return null;
            }

            LastRawResponse = response.Length > 200 ? response[..200] + "..." : response;
            if (method == "get_stats")
                DiagLog($"Call(get_stats) raw: {response}");

            return JsonSerializer.Deserialize<JsonElement>(response, _json);
        }
        catch (Exception ex)
        {
            DiagLog($"Call({method}): FAILED — {ex.GetType().Name}: {ex.Message}");
            return null;
        }
        finally
        {
            _lock.Release();
        }
    }

    /// <summary>
    /// Read a complete response message from the Named Pipe.
    /// The server uses message-mode pipes (PIPE_TYPE_MESSAGE), so
    /// a single ReadAsync should return the complete message.
    /// Falls back to buffer-accumulation for large messages.
    /// </summary>
    private static async Task<string?> ReadMessageAsync(
        Stream stream, CancellationToken ct)
    {
        var sb = new StringBuilder();
        var buf = new byte[4096];

        while (!ct.IsCancellationRequested)
        {
            int n;
            try
            {
                n = await stream.ReadAsync(buf, 0, buf.Length, ct);
            }
            catch (OperationCanceledException)
            {
                return sb.Length > 0 ? sb.ToString() : null;
            }

            if (n == 0) break; // EOF / pipe closed

            sb.Append(Encoding.UTF8.GetString(buf, 0, n));

            // In message mode, a partial read (n < buf.Length) means
            // the message is complete. Otherwise continue accumulating.
            if (n < buf.Length)
                return sb.ToString();
        }

        return sb.Length > 0 ? sb.ToString() : null;
    }

    public void Dispose()
    {
        Disconnect();
        GC.SuppressFinalize(this);
    }
}
