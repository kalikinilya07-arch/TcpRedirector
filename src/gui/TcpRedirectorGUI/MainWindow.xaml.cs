using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Media;
using TcpRedirectorGUI.Adapters.Driving.Wpf.ViewModels;

namespace TcpRedirectorGUI;

public partial class MainWindow : Window
{
    private readonly ShellViewModel _vm;

    public MainWindow(ShellViewModel vm)
    {
        InitializeComponent();
        _vm = vm;
        DataContext = vm;

        PwdBox.PasswordChanged += (_, _) =>
        {
            _vm.Settings.Password = PwdBox.Password;
            UpdatePwdPlaceholder();
        };

        // Clear PasswordBox when Password is set to empty after Save
        _vm.Settings.Saved += () =>
        {
            PwdBox.Clear();
            UpdatePwdPlaceholder();
        };

        // Плейсхолдер «••••••••» показываем только когда поле пустое (и в VM
        // есть сохранённый пароль). При наборе символов — скрываем.
        Loaded += (_, _) => UpdatePwdPlaceholder();
    }

    // Скрывает overlay-плейсхолдер, как только в PasswordBox есть символы.
    // Наличие сохранённого пароля отражает Settings.PasswordPlaceholder
    // (пусто, если пароль не задан) — здесь только гасим его при вводе.
    private void UpdatePwdPlaceholder()
    {
        if (PwdPlaceholder is null) return;
        PwdPlaceholder.Visibility =
            string.IsNullOrEmpty(PwdBox.Password)
                ? Visibility.Visible
                : Visibility.Collapsed;
    }
}

/// <summary>
/// Converts active tab name to Visibility.
/// Usage: Visibility="{Binding ActiveTab, Converter={StaticResource ViewToVis}, ConverterParameter=Settings}"
/// </summary>
public class ViewToVisConverter : IValueConverter
{
    public object Convert(object value, Type t, object p, CultureInfo c)
    {
        if (value is string s && p is string v)
        {
            return s == v ? Visibility.Visible : Visibility.Collapsed;
        }
        return Visibility.Collapsed;
    }

    public object ConvertBack(object value, Type t, object p, CultureInfo c)
        => throw new NotImplementedException();
}

/// <summary>
/// Converts bool (connected) to color (green/red).
/// </summary>
public class BoolToColorConverter : IValueConverter
{
    public object Convert(object value, Type t, object p, CultureInfo c)
    {
        return (value is bool b && b)
            ? Color.FromRgb(0x4E, 0xC9, 0xB0)  // green
            : Color.FromRgb(0xF4, 0x47, 0x47); // red
    }

    public object ConvertBack(object value, Type t, object p, CultureInfo c)
        => throw new NotImplementedException();
}

/// <summary>
/// Converts bool to Visibility. true=Visible, false=Collapsed.
/// </summary>
public class BoolToVisConverter : IValueConverter
{
    public object Convert(object value, Type t, object p, CultureInfo c)
    {
        return (value is bool b && b) ? Visibility.Visible : Visibility.Collapsed;
    }

    public object ConvertBack(object value, Type t, object p, CultureInfo c)
        => throw new NotImplementedException();
}