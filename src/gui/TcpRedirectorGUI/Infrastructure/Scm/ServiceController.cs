using System.Diagnostics;
using System.IO;
using System.Linq;
using System.ServiceProcess;
using TcpRedirectorGUI.Domain.Entities;
using TcpRedirectorGUI.Domain.Ports;

namespace TcpRedirectorGUI.Infrastructure.Scm;

/// <summary>
/// Driving adapter — controls the service.
///
/// Task 1 (service-control fix): control model is now UNIFIED and STOP is
/// authoritative.
///
/// Root cause fixed here:
///   Previously Start ALWAYS spawned a second "--console" child process (with a
///   VISIBLE window) while the service was ALSO installed as a SERVICE_AUTO_START
///   Windows service running under LocalSystem in session 0. Stop then did
///   Process.Kill() on EVERY TcpRedirectorService process — including the
///   SCM-managed one. Killing the SCM-managed process is an *unexpected* exit,
///   so the SCM failure actions configured at --install
///   (SC_ACTION_RESTART x3 + fFailureActionsOnNonCrashFailures=TRUE) relaunched
///   it after 5 s. That is the "keeps auto-restarting after Stop" bug.
///
/// New model:
///   * When the service is INSTALLED in SCM (normal production/deploy case):
///     start/stop go through SCM. A clean SCM Stop reports SERVICE_STOPPED with
///     exit code 0, which does NOT trigger failure actions — so the service
///     stays stopped (authoritative). SCM Start shows NO window at all.
///   * When the service is NOT installed (dev/portable case): fall back to a
///     WINDOWLESS console process (UseShellExecute=false, CreateNoWindow=true).
///     In this mode there is no SCM instance, so a direct Kill() cannot trigger
///     any failure-action restart.
///
/// Session-0 loopback note (assumption): the old "--console" workaround comment
/// referred to a LOCAL session-1 test proxy (test_proxy.py) that is unreachable
/// from an SCM session-0 service over 127.0.0.1. In PRODUCTION the proxy is
/// REMOTE (proxy.host:port), so loopback session isolation does not apply and
/// SCM control is correct. If a purely-local loopback test proxy is used, run
/// the service uninstalled so the windowless-console fallback path is taken.
/// </summary>
public class ServiceController : IServiceController
{
    private const string ServiceName = "TcpRedirectorService";
    private const string ProcessName = "TcpRedirectorService";

    private static readonly TimeSpan ScmTimeout = TimeSpan.FromSeconds(15);

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
                // Preferred path: authoritative SCM control. Start via SCM shows
                // NO console window and reaches a real SERVICE_RUNNING state.
                if (IsServiceInstalled())
                {
                    return StartViaScm();
                }

                // Fallback: no SCM registration → windowless console process.
                return StartViaConsole();
            });
        }
        catch { return false; }
    }

    private static bool StartViaScm()
    {
        try
        {
            using var controller = new System.ServiceProcess.ServiceController(ServiceName);
            controller.Refresh();

            if (controller.Status == ServiceControllerStatus.Running)
                return true;

            if (controller.Status is ServiceControllerStatus.StopPending)
                controller.WaitForStatus(ServiceControllerStatus.Stopped, ScmTimeout);

            if (controller.Status is not (ServiceControllerStatus.StartPending
                or ServiceControllerStatus.Running))
            {
                controller.Start();
            }

            controller.WaitForStatus(ServiceControllerStatus.Running, ScmTimeout);
            controller.Refresh();
            return controller.Status == ServiceControllerStatus.Running;
        }
        catch
        {
            return false;
        }
    }

    private static bool StartViaConsole()
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
                // Task 1 (#3): NO visible console window on Start.
                UseShellExecute = false,
                CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden,
                WorkingDirectory = Path.GetDirectoryName(exePath)
            }
        };
        proc.Start();
        Thread.Sleep(2000);
        return IsProcessRunning();
    }

    public async Task<bool> StopServiceAsync()
    {
        try
        {
            return await Task.Run(() =>
            {
                // Preferred path: authoritative SCM Stop. A clean stop reports
                // SERVICE_STOPPED (exit 0) and does NOT trigger the SCM failure
                // actions, so the service stays DOWN — no auto-restart.
                if (IsServiceInstalled())
                {
                    return StopViaScm();
                }

                // Fallback: not SCM-managed → kill the console process directly.
                // Without an SCM registration there are no failure actions, so a
                // Kill() here cannot cause an auto-restart.
                return StopViaKill();
            });
        }
        catch { return false; }
    }

    private static bool StopViaScm()
    {
        try
        {
            using var controller = new System.ServiceProcess.ServiceController(ServiceName);
            controller.Refresh();

            if (controller.Status is ServiceControllerStatus.Stopped)
                return true;

            if (controller.CanStop)
            {
                controller.Stop();
                controller.WaitForStatus(ServiceControllerStatus.Stopped, ScmTimeout);
            }

            controller.Refresh();
            return controller.Status == ServiceControllerStatus.Stopped;
        }
        catch
        {
            return false;
        }
    }

    private static bool StopViaKill()
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
