using System.Windows;
using Microsoft.Extensions.DependencyInjection;
using TcpRedirectorGUI.Adapters.Driving.Wpf.ViewModels;
using TcpRedirectorGUI.Domain.Ports;
using TcpRedirectorGUI.Infrastructure.Config;
using TcpRedirectorGUI.Infrastructure.Ipc;

namespace TcpRedirectorGUI;

public partial class App : Application
{
    public static IServiceProvider Services { get; private set; } = null!;

    protected override void OnStartup(StartupEventArgs e)
    {
        var services = new ServiceCollection();

        // Domain ports
        services.AddSingleton<IConfigRepository, JsonConfigRepository>();
        services.AddSingleton<ITcpRedirectorService, IpcClient>();

        // ViewModels
        services.AddSingleton<SettingsViewModel>();
        services.AddSingleton<StatsViewModel>();
        services.AddSingleton<ShellViewModel>();

        Services = services.BuildServiceProvider();

        var vm = Services.GetRequiredService<ShellViewModel>();
        var mainWindow = new MainWindow(vm);
        mainWindow.Show();

        // v1.1.0: Auto-connect to service on startup
        _ = Services.GetRequiredService<ShellViewModel>()
            .ConnectToServiceCommand.ExecuteAsync(null);
    }
}