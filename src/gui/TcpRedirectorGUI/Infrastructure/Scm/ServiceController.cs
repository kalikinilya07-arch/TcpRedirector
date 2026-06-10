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
    private Process? _process;

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
            // If installed as Windows Service, use SCM
            if (IsServiceInstalled())
            {
                using var controller = new System.ServiceProcess.ServiceController(ServiceName);
                if (controller.Status == ServiceControllerStatus.Stopped)
                {
                    controller.Start();
                    await Task.Run(() => controller.WaitForStatus(
                        ServiceControllerStatus.Running, TimeSpan.FromSeconds(30)));
                }
                return controller.Status == ServiceControllerStatus.Running;
            }

            // Console mode — start process directly
            if (IsProcessRunning()) return true;

            var exePath = FindServiceExe();
            if (exePath == null) return false;

            _process = new Process
            {
                StartInfo = new ProcessStartInfo
                {
                    FileName = exePath,
                    Arguments = "--console",
                    UseShellExecute = true,
                    CreateNoWindow = false
                }
            };
            _process.Start();
            // Give it a moment to initialize
            await Task.Delay(2000);
            return IsProcessRunning();
        }
        catch { return false; }
    }

    public async Task<bool> StopServiceAsync()
    {
        try
        {
            // If installed as Windows Service, use SCM
            if (IsServiceInstalled())
            {
                using var controller = new System.ServiceProcess.ServiceController(ServiceName);
                if (controller.Status == ServiceControllerStatus.Running)
                {
                    controller.Stop();
                    await Task.Run(() => controller.WaitForStatus(
                        ServiceControllerStatus.Stopped, TimeSpan.FromSeconds(30)));
                }
                return controller.Status == ServiceControllerStatus.Stopped;
            }

            // Console mode — kill process
            var procs = Process.GetProcessesByName(ProcessName);
            foreach (var p in procs)
            {
                p.Kill();
                await Task.Run(() => p.WaitForExit(5000));
            }
            return !IsProcessRunning();
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
            if (IsServiceInstalled())
            {
                using var controller = new System.ServiceProcess.ServiceController(ServiceName);
                return await Task.FromResult(controller.Status switch
                {
                    ServiceControllerStatus.Running => ServiceState.Running,
                    ServiceControllerStatus.StartPending => ServiceState.Starting,
                    ServiceControllerStatus.StopPending => ServiceState.Stopping,
                    ServiceControllerStatus.Stopped => ServiceState.Stopped,
                    _ => ServiceState.Error
                });
            }

            return IsProcessRunning() ? ServiceState.Running : ServiceState.Stopped;
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

        // Try build/service/ relative to GUI
        var paths = new[]
        {
            Path.Combine(guiDir, "..", "service", "TcpRedirectorService.exe"),
            Path.Combine(guiDir, "..", "..", "build", "service", "TcpRedirectorService.exe"),
            Path.Combine(guiDir, "TcpRedirectorService.exe"),
        };

        foreach (var p in paths)
        {
            var full = Path.GetFullPath(p);
            if (File.Exists(full)) return full;
        }
        return null;
    }
}
