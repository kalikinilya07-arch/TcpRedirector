using System.Diagnostics;
using System.IO;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using TcpRedirectorGUI.Domain.Entities;
using TcpRedirectorGUI.Domain.Ports;
using TcpRedirectorGUI.Infrastructure.Ipc;
using TcpRedirectorGUI.Infrastructure.Localization;

namespace TcpRedirectorGUI.Adapters.Driving.Wpf.ViewModels;

/// <summary>
/// Orchestrates the entire UI: service lifecycle, polling, navigation.
/// On startup, finds and launches the backend (TcpRedirectorService.exe --console)
/// automatically — no manual service installation needed.
/// </summary>
public partial class ShellViewModel : ObservableObject, IDisposable
{
    private readonly ITcpRedirectorService _svc;
    private readonly IServiceController _scm;
    private readonly IConfigRepository _config;
    private readonly LocalizationService _loc;
    private CancellationTokenSource? _timerCts;
    private Process? _backendProcess;
    private bool _disposed;
    private readonly int _pollIntervalMs;

    // (Задача №1) Семантические ключи текущего статуса/сообщения, чтобы
    // перелокализовать их при смене языка «на лету».
    private string _svcStatusKey = "Svc.Status.Stopped";
    private string _statusTextKey = "Status.Configured";

    // Task 2: семантический ключ статуса Kerberos-компонента (для перелокализации).
    private string _kerberosStatusKey = "Auth.Kerberos.Disabled";

    public ShellViewModel(
        ITcpRedirectorService svc,
        IServiceController scm,
        IConfigRepository config,
        SettingsViewModel settings,
        StatsViewModel stats,
        TraceViewModel trace,
        LocalizationService loc)
    {
        _svc = svc;
        _scm = scm;
        _config = config;
        _loc = loc;
        Settings = settings;
        Stats = stats;
        Trace = trace;

        // Read poll interval from config (default 1000ms)
        _pollIntervalMs = Math.Max(200, config.ReadInt("stats", "updateIntervalMs", 1000));

        // React to connection state changes
        _svc.ConnectionStateChanged += OnConnectionStateChanged;

        // (Задача №1) Инициализация переключателя языка из уже применённого
        // службой языка; подписка на смену для перелокализации статусов.
        _language = _loc.CurrentLanguage;
        _loc.LanguageChanged += OnServiceLanguageChanged;

        // Load config from disk synchronously (blocking in ctor is OK — tiny file)
        Settings.LoadFromConfig();
        SetSvcStatusKey("Svc.Status.Stopped");
        SetStatusTextKey("Status.Configured");

        // Task 2: the Kerberos-auth indicator's visibility follows the config
        // toggles (auth.kerberos + auth.per_user_enabled). Bind to the live
        // Settings properties so the indicator appears/disappears the moment the
        // user toggles Kerberos/per-user, and also after a settings save/reload
        // (LoadFromConfig raises the same PropertyChanged notifications).
        Settings.PropertyChanged += OnSettingsPropertyChanged;
        Settings.Saved += RefreshKerberosIndicatorVisibility;
        RefreshKerberosIndicatorVisibility();
        SetKerberosStatusKey("Auth.Kerberos.Disabled");
    }

    // Task 2: recompute the Kerberos-indicator visibility when the relevant
    // config toggles change on the Settings VM.
    private void OnSettingsPropertyChanged(object? sender,
        System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(SettingsViewModel.KerberosEnabled)
                or nameof(SettingsViewModel.PerUserEnabled)
                or nameof(SettingsViewModel.AuthRequired))
        {
            RefreshKerberosIndicatorVisibility();
        }
    }

    // Task 2: the per-user Kerberos auth component is only wired by the service
    // when auth is required AND Kerberos AND per-user are enabled — mirror that
    // condition here so the indicator is shown exactly when the component exists.
    private void RefreshKerberosIndicatorVisibility()
    {
        IsKerberosIndicatorVisible =
            Settings.AuthRequired && Settings.KerberosEnabled && Settings.PerUserEnabled;
    }

    // ── Language switch (Задача №1) ──────────────────
    public IReadOnlyList<AppLanguage> Languages { get; } =
        [AppLanguage.Russian, AppLanguage.English];

    [ObservableProperty] private AppLanguage _language;

    partial void OnLanguageChanged(AppLanguage value)
    {
        if (_loc.CurrentLanguage == value) return;
        _loc.SetLanguage(value);
        _config.WriteString("gui", "language", LocalizationService.ToCode(value));
    }

    private void OnServiceLanguageChanged(AppLanguage lang)
    {
        // Перелокализовать динамические статусы после смены словаря.
        SvcStatus = Loc.T(_svcStatusKey);
        StatusText = Loc.T(_statusTextKey);
        // Task 2: also re-localize the Kerberos indicator label.
        KerberosStatus = Loc.T(_kerberosStatusKey);
    }

    // Устанавливает статус службы по ключу (с запоминанием для перелокализации).
    private void SetSvcStatusKey(string key)
    {
        _svcStatusKey = key;
        SvcStatus = Loc.T(key);
    }

    private void SetStatusTextKey(string key)
    {
        _statusTextKey = key;
        StatusText = Loc.T(key);
    }

    // Task 2: set the Kerberos-indicator label by localization key (remembered
    // for re-localization on language switch).
    private void SetKerberosStatusKey(string key)
    {
        _kerberosStatusKey = key;
        KerberosStatus = Loc.T(key);
    }

    // Task 2: map the service-reported auth_status string to a localization key
    // and health flag, and apply them to the indicator.
    private void ApplyKerberosStatus(string authStatus)
    {
        var (key, healthy) = authStatus switch
        {
            "active"    => ("Auth.Kerberos.Active", true),
            "no_helper" => ("Auth.Kerberos.NoHelper", false),
            "error"     => ("Auth.Kerberos.Error", false),
            _           => ("Auth.Kerberos.Disabled", false),
        };
        IsKerberosHealthy = healthy;
        SetKerberosStatusKey(key);
    }

    // ── Status ───────────────────────────────────────

    [ObservableProperty]
    private bool _isConnected;

    [ObservableProperty]
    private string _statusText = "Loading...";

    [ObservableProperty]
    private string _svcStatus = "Stopped";

    [ObservableProperty]
    private string _svcMsg = "";

    // ── Kerberos auth component status (Task 2) ──────

    /// <summary>
    /// Localized text of the second (Kerberos auth) indicator, e.g. "Active",
    /// "No helper", "Error", "Disabled".
    /// </summary>
    [ObservableProperty]
    private string _kerberosStatus = "";

    /// <summary>
    /// True when the second (Kerberos auth) indicator should be shown. Driven by
    /// config: auth required + Kerberos + per-user enabled. Hidden otherwise.
    /// </summary>
    [ObservableProperty]
    private bool _isKerberosIndicatorVisible;

    /// <summary>
    /// True when the Kerberos component is healthy (auth_status == "active"),
    /// used to color the indicator dot green vs. red (reuses BoolToColor).
    /// </summary>
    [ObservableProperty]
    private bool _isKerberosHealthy;

    // ── Stats ────────────────────────────────────────

    [ObservableProperty]
    private string _totalTraffic = "0 B";

    // ── Child ViewModels ─────────────────────────────

    public SettingsViewModel Settings { get; }
    public StatsViewModel Stats { get; }
    public TraceViewModel Trace { get; }

    // ── Service lifecycle ────────────────────────────

    [RelayCommand]
    private async Task StartService()
    {
        try
        {
            SetSvcStatusKey("Svc.Status.Starting");
            SvcMsg = "";

            StopTimer();

            var ok = await _scm.StartServiceAsync();
            if (!ok)
            {
                SetSvcStatusKey("Svc.Status.Failed");
                SvcMsg = Loc.T("Svc.Msg.StartFailed");
                return;
            }

            await _svc.ConnectAsync();
            StartTimer();
            // Reload config from disk — service loaded it at startup. Skip when
            // the user has unsaved edits so a reload doesn't discard them.
            if (!Settings.ReloadFromConfigIfClean())
                SvcMsg = Loc.T("Svc.Msg.StartedUnsaved");
            SetSvcStatusKey("Svc.Status.Running");
            if (string.IsNullOrEmpty(SvcMsg)) SvcMsg = Loc.T("Svc.Msg.Started");
        }
        catch (Exception ex)
        {
            SetSvcStatusKey("Svc.Status.Error");
            SvcMsg = $"\u2717 {ex.Message}";
        }
        finally
        {
            _ = ClearMsgAfterDelay();
        }
    }

    [RelayCommand]
    private async Task StopService()
    {
        try
        {
            SetSvcStatusKey("Svc.Status.Stopping");
            SvcMsg = "";

            StopTimer();
            _svc.Disconnect();

            // Task 1: STOP must be authoritative and permanent. The controller
            // performs a clean SCM Stop when the service is installed
            // (SERVICE_STOPPED, exit 0 → NO failure-action restart) and only
            // Kill()s a process when the service is NOT SCM-managed.
            //
            // We deliberately DO NOT taskkill here on timeout: force-killing an
            // SCM-managed, auto-start service triggers its failure actions
            // (SC_ACTION_RESTART), which is exactly the auto-restart-after-stop
            // bug this task fixes. The controller already bounds each SCM
            // Stop/WaitForStatus with its own 15 s timeout.
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(30));
            try
            {
                var ok = await Task.Run(() => _scm.StopServiceAsync(), cts.Token);
                SetSvcStatusKey(ok ? "Svc.Status.Stopped" : "Svc.Status.Failed");
                SvcMsg = ok ? Loc.T("Svc.Msg.Stopped") : Loc.T("Svc.Msg.StopFailed");
            }
            catch (OperationCanceledException)
            {
                // Stop is taking unusually long. Report failure rather than
                // force-killing (a Kill would re-trigger SCM failure actions).
                SetSvcStatusKey("Svc.Status.Failed");
                SvcMsg = Loc.T("Svc.Msg.StopFailed");
            }
        }
        catch
        {
            SetSvcStatusKey("Svc.Status.Error");
        }
        finally
        {
            // Reload config from disk — may have been modified externally. Skip
            // when the user has unsaved edits so we don't discard them.
            Settings.ReloadFromConfigIfClean();
            _ = ClearMsgAfterDelay();
        }
    }

    // ── Auto-start backend ───────────────────────────

    /// <summary>
    /// Tries to connect to a running backend. If none is found,
    /// locates TcpRedirectorService.exe next to the GUI (or one level up)
    /// and launches it in console mode, then connects.
    /// </summary>
    private async Task AutoStartAndConnectAsync()
    {
        // 1. Try connecting to an already-running service
        try
        {
            await _svc.ConnectAsync();
            if (_svc.IsConnected)
            {
                StatusText = "Connected (existing)";
                SvcStatus = "Running";
                StartTimer();
                return;
            }
        }
        catch
        {
            // Not running — proceed to launch
        }

        // 2. Find the backend exe
        var exePath = FindBackendExe();
        if (string.IsNullOrEmpty(exePath))
        {
            SetStatusTextKey("Svc.Status.Error");
            SetSvcStatusKey("Svc.Status.Error");
            SvcMsg = Loc.T("Svc.Msg.BackendNotFound");
            return;
        }

        // 3. Launch backend process
        try
        {
            SvcStatus = "Starting";
            StatusText = "Starting backend...";

            var psi = new ProcessStartInfo
            {
                FileName = exePath,
                Arguments = "--console",
                UseShellExecute = false,
                CreateNoWindow = true,
                // Redirect both stdout and stderr for diagnostics;
                // stdout is read and discarded to prevent pipe buffer from filling.
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                WindowStyle = ProcessWindowStyle.Hidden,
                WorkingDirectory = Path.GetDirectoryName(exePath)
            };

            _backendProcess = new Process { StartInfo = psi };
            // Capture both stdout and stderr asynchronously for diagnostics
            var outBuilder = new System.Text.StringBuilder();
            var errBuilder = new System.Text.StringBuilder();
            _backendProcess.OutputDataReceived += (_, e) =>
            {
                if (e.Data != null)
                    lock (outBuilder) outBuilder.AppendLine(e.Data);
            };
            _backendProcess.ErrorDataReceived += (_, e) =>
            {
                if (e.Data != null)
                    lock (errBuilder) errBuilder.AppendLine(e.Data);
            };
            _backendProcess.Start();
            _backendProcess.BeginOutputReadLine();
            _backendProcess.BeginErrorReadLine();

            // 4. Wait for the pipe to become available (max 10 seconds)
            for (int i = 0; i < 20; i++)
            {
                await Task.Delay(500);
                try
                {
                    await _svc.ConnectAsync();
                    if (_svc.IsConnected)
                    {
                        StatusText = "Connected";
                        SvcStatus = "Running";
                        StartTimer();
                        return;
                    }
                }
                catch
                {
                    // Not ready yet
                }
            }

            // Timed out — read stdout + stderr for diagnosis
            string diag;
            lock (errBuilder) diag = errBuilder.ToString();
            lock (outBuilder)
            {
                var outText = outBuilder.ToString();
                if (!string.IsNullOrWhiteSpace(outText))
                    diag = (string.IsNullOrWhiteSpace(diag) ? "" : diag + "\n") + outText.TrimEnd();
            }
            if (!string.IsNullOrWhiteSpace(diag))
            {
                SvcMsg = diag.TrimEnd();
            }
            else
            {
                SvcMsg = "Backend exited without error message. Check WinDivert driver installation.";
            }
            SvcStatus = "Error";
            StatusText = "Start failed";
        }
        catch (Exception ex)
        {
            SvcStatus = "Error";
            StatusText = "Start failed";
            SvcMsg = ex.Message;
        }
    }

    private static string? FindBackendExe()
    {
        // Delegate to the single shared resolver so the launched service and the
        // config-file anchor (AppPaths.GetConfigPath) always agree on which
        // TcpRedirectorService.exe / directory is authoritative.
        return TcpRedirectorGUI.Infrastructure.Config.AppPaths.GetServiceExePath();
    }

    // ── Polling timer ────────────────────────────────

    private void StartTimer()
    {
        StopTimer();
        // Fresh session — clear derived baselines so the graph doesn't spike.
        Stats.ResetBaselines();
        Trace.Clear();
        _timerCts = new CancellationTokenSource();
        _ = PollLoopAsync(_timerCts.Token);
    }

    private void StopTimer()
    {
        try { _timerCts?.Cancel(); } catch { }
        _timerCts?.Dispose();
        _timerCts = null;
    }

    private async Task PollLoopAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(_pollIntervalMs, ct);
                if (!_svc.IsConnected)
                {
                    Trace.Clear();
                    continue;
                }

                var stats = await _svc.GetStatsAsync();
                if (stats is not null)
                {
                    TotalTraffic = FormatBytes(stats.TotalRxBytes + stats.TotalTxBytes);
                    Stats.PushStats(stats);
                }
                else if (_svc is IpcClient ipc)
                {
                    var raw = ipc.LastRawResponse;
                    if (!string.IsNullOrEmpty(raw))
                        SvcMsg = $"IPC raw: {raw}";
                }

                // Packet/connection trace.
                var connections = await _svc.GetConnectionsAsync();
                Trace.PushConnections(connections);

                var status = await _svc.GetServiceStatusAsync();
                if (status is not null)
                {
                    SetSvcStatusKey(status.Running ? "Svc.Status.Running" : "Svc.Status.Stopped");
                    // Task 2: update the Kerberos auth-component indicator from
                    // the service-reported auth_status.
                    ApplyKerberosStatus(status.AuthStatus);
                }
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (Exception ex)
            {
                // Show IPC errors in status bar for diagnostics
                SvcMsg = $"IPC err: {ex.GetType().Name}";
            }
        }
    }

    // ── Helpers ──────────────────────────────────────

    private void OnConnectionStateChanged(bool connected)
    {
        IsConnected = connected;
        SetStatusTextKey(connected ? "Status.Connected" : "Status.Disconnected");
        if (!connected)
        {
            Trace.Clear();
            Stats.ResetBaselines();
        }
    }

    private async Task ClearMsgAfterDelay()
    {
        await Task.Delay(5000);
        if (SvcMsg.Contains('\u2713'))
            SvcMsg = "";
    }

    private static string FormatBytes(ulong b) => b switch
    {
        >= 1_073_741_824 => $"{b / 1_073_741_824.0:F1} GB",
        >= 1_048_576 => $"{b / 1_048_576.0:F1} MB",
        >= 1_024 => $"{b / 1_024.0:F1} KB",
        _ => $"{b} B"
    };

    // ── IDisposable ──────────────────────────────────

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;

        _svc.ConnectionStateChanged -= OnConnectionStateChanged;
        _loc.LanguageChanged -= OnServiceLanguageChanged;
        Settings.PropertyChanged -= OnSettingsPropertyChanged;
        Settings.Saved -= RefreshKerberosIndicatorVisibility;
        StopTimer();
        _svc.Disconnect();

        // Kill the backend process we started
        if (_backendProcess is not null && !_backendProcess.HasExited)
        {
            try { _backendProcess.Kill(); } catch { }
            _backendProcess.Dispose();
            _backendProcess = null;
        }

        GC.SuppressFinalize(this);
    }
}