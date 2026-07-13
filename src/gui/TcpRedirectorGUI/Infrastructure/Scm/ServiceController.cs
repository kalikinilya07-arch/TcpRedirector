using System.Diagnostics;
using System.IO;
using System.Linq;
using System.ServiceProcess;
using TcpRedirectorGUI.Domain.Entities;
using TcpRedirectorGUI.Domain.Ports;

namespace TcpRedirectorGUI.Infrastructure.Scm;

/// <summary>
/// Driving adapter — controls the service via SCM (if installed as Windows Service)
/// or via direct process management (console mode).
/// </summary>
public class ServiceController : IServiceController
{
    private const string ServiceName = "TcpRedirectorService";
    private const string ProcessName = "TcpRedirectorService";

    public bool IsAdministrator()
    {
        var identity = System.Security.Principal.WindowsIdentity.GetCurrent();
        var principal = new System.Security.Principal.WindowsPrincipal(identity);
        return principal.IsInRole(
            System.Security.Principal.WindowsBuiltInRole.Administrator);
    }

    private static bool IsServiceInstalled()
    {
        try
        {
            using var sc = System.ServiceProcess.ServiceController
                .GetServices()
                .FirstOrDefault(s => s.ServiceName == ServiceName);
            return sc != null;
        }
        catch { return false; }
    }

    public async Task<bool> StartServiceAsync()
    {
        try
        {
            return await Task.Run(() =>
            {
                if (IsProcessRunning())
                {
                    DiagLog("StartServiceAsync: process already running, skip launch");
                    return true;
                }

                var exePath = FindServiceExe();
                if (exePath == null)
                {
                    DiagLog("StartServiceAsync: backend exe NOT FOUND");
                    return false;
                }
                DiagLog($"StartServiceAsync: launching {exePath} --console");

                var psi = new ProcessStartInfo
                {
                    FileName = exePath,
                    Arguments = "--console",
                    UseShellExecute = false,
                    CreateNoWindow = false,
                    // Only redirect stderr for diagnostics.
                    // DO NOT redirect stdout — the backend writes extensive logs
                    // to stdout and will deadlock if the buffer fills up with no reader.
                    RedirectStandardOutput = false,
                    RedirectStandardError = true,
                    WorkingDirectory = Path.GetDirectoryName(exePath)
                };

                var proc = new Process { StartInfo = psi };
                var errBuilder = new System.Text.StringBuilder();

                proc.ErrorDataReceived += (_, e) =>
                {
                    if (e.Data != null)
                        lock (errBuilder) errBuilder.AppendLine(e.Data);
                };

                try
                {
                    proc.Start();
                    proc.BeginErrorReadLine();
                }
                catch (Exception ex)
                {
                    DiagLog($"StartServiceAsync: process start FAILED: {ex.Message}");
                    return false;
                }

                // Wait for the backend to initialize (up to 8 seconds)
                for (int i = 0; i < 16; i++)
                {
                    Thread.Sleep(500);
                    if (proc.HasExited)
                    {
                        string diag;
                        lock (errBuilder) diag = errBuilder.ToString();
                        DiagLog($"StartServiceAsync: backend EXITED early (code {proc.ExitCode}): {diag.TrimEnd()}");
                        // Store diagnostics for the ViewModel to display
                        _lastStartupError = diag.TrimEnd();
                        return false;
                    }
                }

                bool running = IsProcessRunning();
                DiagLog($"StartServiceAsync: after 8s wait, process running={running}");
                return running;
            });
        }
        catch (Exception ex)
        {
            DiagLog($"StartServiceAsync: exception: {ex.Message}");
            return false;
        }
    }

    /// <summary>
    /// Last startup error captured from backend stderr. Read by ShellViewModel
    /// to display when start fails.
    /// </summary>
    public string? LastStartupError
    {
        get { var err = _lastStartupError; _lastStartupError = null; return err; }
    }
    private volatile string? _lastStartupError;

    public async Task<bool> StopServiceAsync()
    {
        try
        {
            // Always kill the process directly (console mode).
            return await Task.Run(() =>
            {
                var procs = Process.GetProcessesByName(ProcessName);
                foreach (var p in procs)
                {
                    try
                    {
                        p.Kill();
                        p.WaitForExit(5000);
                    }
                    finally
                    {
                        p.Dispose();  // M12: prevent handle leak
                    }
                }
                return !IsProcessRunning();
            });
        }
        catch { return false; }
    }

    public async Task<bool> RestartServiceAsync()
    {
        if (await StopServiceAsync())
        {
            await Task.Delay(1000);
            return await StartServiceAsync();
        }
        return false;
    }

    public async Task<ServiceState> GetStateAsync()
    {
        try
        {
            return await Task.Run(() =>
            {
                if (IsServiceInstalled())
                {
                    using var controller = new System.ServiceProcess.ServiceController(ServiceName);
                    return controller.Status switch
                    {
                        ServiceControllerStatus.Running => ServiceState.Running,
                        ServiceControllerStatus.StartPending => ServiceState.Starting,
                        ServiceControllerStatus.StopPending => ServiceState.Stopping,
                        ServiceControllerStatus.Stopped => ServiceState.Stopped,
                        _ => ServiceState.Error
                    };
                }

                return IsProcessRunning() ? ServiceState.Running : ServiceState.Stopped;
            });
        }
        catch { return ServiceState.Stopped; }
    }

    private static bool IsProcessRunning()
    {
        var procs = Process.GetProcessesByName(ProcessName);
        return procs.Length > 0;
    }

    private static string? FindServiceExe()
    {
        // Search near the GUI executable
        var guiDir = AppDomain.CurrentDomain.BaseDirectory;

        var paths = new[]
        {
            // Same dir as GUI (e.g. deploy_new/ or build/gui/)
            Path.Combine(guiDir, "TcpRedirectorService.exe"),
            // Parent dir (e.g. build/ when GUI is in build/gui/)
            Path.Combine(guiDir, "..", "TcpRedirectorService.exe"),
            // build/ dir — priority build (MSBuild output)
            Path.Combine(guiDir, "..", "..", "build", "TcpRedirectorService.exe"),
            // deploy/ dir — release deploy (CI/CD)
            Path.Combine(guiDir, "..", "..", "deploy", "TcpRedirectorService.exe"),
        };

        foreach (var p in paths)
        {
            var full = Path.GetFullPath(p);
            if (File.Exists(full)) return full;
        }
        return null;
    }

    /// <summary>
    /// Write diagnostic log to gui_diag.log next to the GUI executable.
    /// </summary>
    private static void DiagLog(string msg)
    {
        try
        {
            var dir = AppDomain.CurrentDomain.BaseDirectory;
            var path = Path.Combine(dir, "gui_diag.log");
            File.AppendAllText(path, $"{DateTime.Now:HH:mm:ss.fff} [SCM] {msg}\n");
        }
        catch { }
    }
}
