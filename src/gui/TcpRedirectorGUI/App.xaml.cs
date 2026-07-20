using System.Windows;
using Microsoft.Extensions.DependencyInjection;
using TcpRedirectorGUI.Domain.Ports;
using TcpRedirectorGUI.Infrastructure.Config;
using TcpRedirectorGUI.Infrastructure.Ipc;
using TcpRedirectorGUI.Infrastructure.Localization;
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
        // (Задача №1) Служба локализации (RU/EN).
        services.AddSingleton<LocalizationService>();

        // ViewModels
        services.AddSingleton<SettingsViewModel>();
        services.AddSingleton<StatsViewModel>();
        services.AddSingleton<TraceViewModel>();
        services.AddSingleton<ShellViewModel>();
        services.AddSingleton<MainWindow>();

        var sp = services.BuildServiceProvider();

        // (Задача №1) Применяем язык из config.json (gui.language, дефолт RU)
        // ДО показа окна, чтобы весь интерфейс отрисовался на выбранном языке.
        var loc = sp.GetRequiredService<LocalizationService>();
        var cfg = sp.GetRequiredService<IConfigRepository>();
        var langCode = cfg.ReadString("gui", "language", "ru");
        loc.SetLanguage(LocalizationService.ParseLanguage(langCode));

        var wnd = sp.GetRequiredService<MainWindow>();
        wnd.Show();
    }
}