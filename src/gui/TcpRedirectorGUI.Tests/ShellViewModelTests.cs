using Moq;
using TcpRedirectorGUI.Adapters.Driving.Wpf.ViewModels;
using TcpRedirectorGUI.Domain.Entities;
using TcpRedirectorGUI.Domain.Ports;

namespace TcpRedirectorGUI.Tests;

public class ShellViewModelTests
{
    private readonly Mock<ITcpRedirectorService> _svcMock;
    private readonly Mock<IConfigRepository> _configMock;
    private readonly SettingsViewModel _settingsVm;
    private readonly StatsViewModel _statsVm;

    public ShellViewModelTests()
    {
        _svcMock = new Mock<ITcpRedirectorService>();
        _configMock = new Mock<IConfigRepository>();

        // Setup config defaults for SettingsViewModel.LoadFromConfig
        _configMock.Setup(c => c.ReadString(It.IsAny<string>(), It.IsAny<string>(), It.IsAny<string>())).Returns("");
        _configMock.Setup(c => c.ReadBool(It.IsAny<string>(), It.IsAny<string>(), It.IsAny<bool>())).Returns(false);
        _configMock.Setup(c => c.ReadInt(It.IsAny<string>(), It.IsAny<string>(), It.IsAny<int>())).Returns(2);
        _configMock.Setup(c => c.ReadRules()).Returns(new List<Rule>());

        _settingsVm = new SettingsViewModel(_configMock.Object, _svcMock.Object);
        _statsVm = new StatsViewModel(_svcMock.Object, _configMock.Object);
    }

    private ShellViewModel CreateVm()
    {
        return new ShellViewModel(
            _svcMock.Object,
            _configMock.Object,
            _settingsVm,
            _statsVm);
    }

    // ── Constructor ──────────────────────────────────

    [Fact]
    public void Constructor_SetsVersionText()
    {
        var vm = CreateVm();

        Assert.StartsWith("TcpRedirector v", vm.VersionText);
    }

    [Fact]
    public void Constructor_SetsUserName()
    {
        var vm = CreateVm();

        Assert.False(string.IsNullOrWhiteSpace(vm.UserName));
        Assert.Equal(Environment.UserName, vm.UserName);
    }

    [Fact]
    public void Constructor_SetsInitialStatus()
    {
        var vm = CreateVm();

        Assert.Equal("Configured", vm.StatusText);
        Assert.False(vm.IsConnected);
        Assert.False(vm.IsServiceAvailable);
        Assert.False(vm.IsCaptureRunning);
    }

    [Fact]
    public void Constructor_SetsChildViewModels()
    {
        var vm = CreateVm();

        Assert.Same(_settingsVm, vm.Settings);
        Assert.Same(_statsVm, vm.Stats);
    }

    // ── Connection state ─────────────────────────────

    [Fact]
    public void OnConnectionStateChanged_Connected_UpdatesState()
    {
        var vm = CreateVm();

        // Simulate connection state change via the event
        _svcMock.Raise(s => s.ConnectionStateChanged += null, true);

        Assert.True(vm.IsConnected);
        Assert.True(vm.IsServiceAvailable);
        Assert.Equal("Connected", vm.StatusText);
    }

    [Fact]
    public void OnConnectionStateChanged_Disconnected_UpdatesState()
    {
        var vm = CreateVm();
        _svcMock.Raise(s => s.ConnectionStateChanged += null, true);
        _svcMock.Raise(s => s.ConnectionStateChanged += null, false);

        Assert.False(vm.IsConnected);
        Assert.False(vm.IsServiceAvailable);
        Assert.Equal("Disconnected", vm.StatusText);
    }

    // ── TotalTraffic formatting ──────────────────────

    [Fact]
    public void TotalTraffic_DefaultsToZero()
    {
        var vm = CreateVm();

        Assert.Equal("0 B", vm.TotalTraffic);
    }

    // ── VersionText ──────────────────────────────────

    [Fact]
    public void VersionText_ContainsAppName()
    {
        var vm = CreateVm();

        Assert.Contains("TcpRedirector", vm.VersionText);
    }

    [Fact]
    public void VersionText_ContainsVersionNumber()
    {
        var vm = CreateVm();

        // Should match pattern "TcpRedirector vX.Y.Z"
        Assert.Matches(@"TcpRedirector v\d+\.\d+\.\d+", vm.VersionText);
    }

    // ── UserName ─────────────────────────────────────

    [Fact]
    public void UserName_MatchesEnvironment()
    {
        var vm = CreateVm();

        Assert.Equal(Environment.UserName, vm.UserName);
    }

    [Fact]
    public void UserName_IsNotEmpty()
    {
        var vm = CreateVm();

        Assert.NotEmpty(vm.UserName);
    }

    // ── Dispose ──────────────────────────────────────

    [Fact]
    public void Dispose_UnsubscribesFromEvents()
    {
        var vm = CreateVm();
        vm.Dispose();

        // After dispose, connection state changes should not throw
        _svcMock.Raise(s => s.ConnectionStateChanged += null, false);
        Assert.False(vm.IsConnected);
    }

    [Fact]
    public void Dispose_CanBeCalledMultipleTimes()
    {
        var vm = CreateVm();
        vm.Dispose();
        var ex = Record.Exception(() => vm.Dispose());

        Assert.Null(ex);
    }
}