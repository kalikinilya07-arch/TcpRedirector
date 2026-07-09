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
            // ВСЕГДА запускаем как --console, даже если сервис установлен в SCM.
            // Причина: SCM запускает сервис в session 0, а test_proxy.py (прокси)
            // работает в session 1. Loopback (127.0.0.1) изолирован по сессиям,
            // поэтому relay НЕ может соединиться с прокси из session 0.
            // Console-mode запускает сервис в той же сессии, что и GUI.
            return await Task.Run(() =>
            {
                if (IsProcessRunning()) return true;

                var exePath = FindServiceExe();
                if (exePath == null) return false;

                var proc = new Process
                {
                    StartInfo = new ProcessStartInfo
                    {
                        FileName = exePath,
                        Arguments = "--console",
                        UseShellExecute = true,
                        CreateNoWindow = false
                    }
                };
                proc.Start();
                Thread.Sleep(2000);
                return IsProcessRunning();
            });
        }
        catch { return false; }
    }

    public async Task<bool> StopServiceAsync()
    {
        try
        {
            // ВСЕГДА убиваем процесс напрямую (console mode).
            // См. StartServiceAsync — сервис всегда запускается как --console.
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
            // IsServiceInstalled() на thread pool — не блокируем UI
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
            // build/ dir — новейшая сборка (приоритет выше deploy/)
            Path.Combine(guiDir, "..", "..", "build", "TcpRedirectorService.exe"),
            // deploy/ dir — старая сборка (фолбэк)
            Path.Combine(guiDir, "..", "..", "deploy", "TcpRedirectorService.exe"),
        };

        foreach (var p in paths)
        {
            var full = Path.GetFullPath(p);
            if (File.Exists(full)) return full;
        }
        return null;
    }
}
