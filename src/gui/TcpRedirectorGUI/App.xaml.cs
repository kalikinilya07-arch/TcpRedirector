using System.Windows;
using Microsoft.Extensions.DependencyInjection;
using TcpRedirectorGUI.Domain.Ports;
using TcpRedirectorGUI.Infrastructure.Config;
using TcpRedirectorGUI.Infrastructure.Ipc;
using TcpRedirectorGUI.Infrastructure.Scm;
using TcpRedirectorGUI.Adapters.Driving.Wpf.ViewModels;

namespace TcpRedirectorGUI;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        var services = new ServiceCollection();

        // Infrastructure
        services.AddSingleton<IConfigRepository, JsonConfigRepository>();
        services.AddSingleton<ITcpRedirectorService, IpcClient>();
        services.AddSingleton<IServiceController, ServiceController>();

        // ViewModels
        services.AddSingleton<SettingsViewModel>();
        services.AddSingleton<StatsViewModel>();
        services.AddSingleton<TraceViewModel>();
        services.AddSingleton<ShellViewModel>();
        services.AddSingleton<MainWindow>();

        var sp = services.BuildServiceProvider();
        var wnd = sp.GetRequiredService<MainWindow>();
        wnd.Show();
    }
}