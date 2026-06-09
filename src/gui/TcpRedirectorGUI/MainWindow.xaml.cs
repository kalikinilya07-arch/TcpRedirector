using System.Globalization;
using System.Windows;
using System.Windows.Data;
using System.Windows.Media;
using Microsoft.Win32;
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
    }

    private void SaveProxy_Click(object sender, RoutedEventArgs e)
    {
        _vm.Proxy.Password = PwdBox.Password;
        _vm.Proxy.SaveCommand.Execute(null);
    }

    private void AddRule_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog { Title = "Select Application", Filter = "Executables (*.exe)|*.exe|All files (*.*)|*.*", CheckFileExists = true };
        if (dlg.ShowDialog() == true) _vm.Rules.Add(dlg.FileName);
    }
}

public class ViewToVisConverter : IValueConverter
{
    public object Convert(object value, Type t, object p, CultureInfo c) => value is string s && p is string v && s == v ? Visibility.Visible : Visibility.Collapsed;
    public object ConvertBack(object value, Type t, object p, CultureInfo c) => throw new NotImplementedException();
}

public class BoolToColorConverter : IValueConverter
{
    public object Convert(object value, Type t, object p, CultureInfo c) => (value is bool b && b) ? Color.FromRgb(0x10, 0x7C, 0x10) : Color.FromRgb(0xD1, 0x34, 0x38);
    public object ConvertBack(object value, Type t, object p, CultureInfo c) => throw new NotImplementedException();
}