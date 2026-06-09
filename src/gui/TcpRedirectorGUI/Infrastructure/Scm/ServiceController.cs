using System.Runtime.InteropServices;
using System.ServiceProcess;
using TcpRedirectorGUI.Domain.Entities;
using TcpRedirectorGUI.Domain.Ports;

namespace TcpRedirectorGUI.Infrastructure.Scm;

/// <summary>
/// Driving adapter — controls Windows Service via SCM
/// </summary>
public class ServiceController : IServiceController
{
    private const string ServiceName = "TcpRedirectorService";

    public bool IsAdministrator()
    {
        var identity = System.Security.Principal.WindowsIdentity.GetCurrent();
        var principal = new System.Security.Principal.WindowsPrincipal(identity);
        return principal.IsInRole(
            System.Security.Principal.WindowsBuiltInRole.Administrator);
    }

    public async Task<bool> StartServiceAsync()
    {
        try
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
        catch
        {
            return false;
        }
    }

    public async Task<bool> StopServiceAsync()
    {
        try
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
        catch
        {
            return false;
        }
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
        catch
        {
            return ServiceState.Stopped;
        }
    }
}