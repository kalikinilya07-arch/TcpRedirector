using Moq;
using TcpRedirectorGUI.Adapters.Driving.Wpf.ViewModels;
using TcpRedirectorGUI.Domain.Entities;
using TcpRedirectorGUI.Domain.Ports;

namespace TcpRedirectorGUI.Tests;

public class SettingsViewModelTests
{
    private readonly Mock<ITcpRedirectorService> _svcMock;
    private readonly Mock<IConfigRepository> _configMock;
    private readonly SettingsViewModel _vm;

    public SettingsViewModelTests()
    {
        _svcMock = new Mock<ITcpRedirectorService>();
        _configMock = new Mock<IConfigRepository>();

        // Default config values
        _configMock.Setup(c => c.ReadString("proxy", "host", "127.0.0.1")).Returns("proxy.example.com");
        _configMock.Setup(c => c.ReadPort("port", 3128)).Returns(8080);
        _configMock.Setup(c => c.ReadBool("auth", "required", false)).Returns(true);
        _configMock.Setup(c => c.ReadBool("auth", "kerberos", false)).Returns(false);
        _configMock.Setup(c => c.ReadString("auth", "login", "")).Returns("user1");
        _configMock.Setup(c => c.ReadString("auth", "password", "")).Returns("pass1");
        _configMock.Setup(c => c.ReadInt("log", "level", 2)).Returns(2);
        _configMock.Setup(c => c.ReadRules()).Returns(new List<Rule>());

        _vm = new SettingsViewModel(_configMock.Object, _svcMock.Object);
    }

    // ── Initial state ────────────────────────────────

    [Fact]
    public void Constructor_InitializesDefaults()
    {
        Assert.Equal("", _vm.Host);
        Assert.Equal(3128, _vm.Port);
        Assert.False(_vm.AuthRequired);
        Assert.False(_vm.KerberosEnabled);
        Assert.Equal("", _vm.Login);
        Assert.Equal(2, _vm.LogLevelFilter);
        Assert.NotNull(_vm.Rules);
        Assert.Equal(0, _vm.AuthModeIndex);
    }

    // ── LoadFromConfig ───────────────────────────────

    [Fact]
    public void LoadFromConfig_LoadsProxySettings()
    {
        _vm.LoadFromConfig();

        Assert.Equal("proxy.example.com", _vm.Host);
        Assert.Equal(8080, _vm.Port);
    }

    [Fact]
    public void LoadFromConfig_LoadsAuthSettings_Basic()
    {
        _vm.LoadFromConfig();

        Assert.True(_vm.AuthRequired);
        Assert.Equal("user1", _vm.Login);
        Assert.Equal(0, _vm.AuthModeIndex); // Basic
        Assert.False(_vm.KerberosEnabled);
    }

    [Fact]
    public void LoadFromConfig_LoadsAuthSettings_Kerberos()
    {
        _configMock.Setup(c => c.ReadBool("auth", "kerberos", false)).Returns(true);
        var vm = new SettingsViewModel(_configMock.Object, _svcMock.Object);
        vm.LoadFromConfig();

        Assert.True(vm.AuthRequired);
        Assert.Equal(1, vm.AuthModeIndex); // Kerberos
        Assert.True(vm.KerberosEnabled);
    }

    [Fact]
    public void LoadFromConfig_LoadsLogLevel()
    {
        _vm.LoadFromConfig();

        Assert.Equal(2, _vm.LogLevelFilter);
    }

    [Fact]
    public void LoadFromConfig_LoadsRules()
    {
        var rules = new List<Rule>
        {
            new() { Pattern = "chrome.exe", Action = RuleAction.Proxy, Type = RuleType.ProcessName, Enabled = true },
            new() { Pattern = "firefox.exe", Action = RuleAction.Direct, Type = RuleType.ProcessName, Enabled = true }
        };
        _configMock.Setup(c => c.ReadRules()).Returns(rules);
        var vm = new SettingsViewModel(_configMock.Object, _svcMock.Object);
        vm.LoadFromConfig();

        Assert.Equal(2, vm.Rules.Count);
        Assert.Equal("chrome.exe", vm.Rules[0].Pattern);
        Assert.Equal("firefox.exe", vm.Rules[1].Pattern);
    }

    // ── AuthModeIndex ────────────────────────────────

    [Fact]
    public void AuthModeIndex_SetToKerberos_EnablesKerberos()
    {
        _vm.AuthModeIndex = 1;

        Assert.True(_vm.KerberosEnabled);
        Assert.False(_vm.IsBasicAuth);
    }

    [Fact]
    public void AuthModeIndex_SetToBasic_DisablesKerberos()
    {
        _vm.AuthModeIndex = 1; // first set to Kerberos
        _vm.AuthModeIndex = 0; // then back to Basic

        Assert.False(_vm.KerberosEnabled);
        Assert.True(_vm.IsBasicAuth);
    }

    [Fact]
    public void IsBasicAuth_RequiresAuthRequired()
    {
        _vm.AuthRequired = false;
        _vm.AuthModeIndex = 0;

        Assert.False(_vm.IsBasicAuth);
    }

    [Fact]
    public void IsBasicAuth_True_WhenAuthRequiredAndBasic()
    {
        _vm.AuthRequired = true;
        _vm.AuthModeIndex = 0;

        Assert.True(_vm.IsBasicAuth);
    }

    // ── KerberosEnabled ──────────────────────────────

    [Fact]
    public void KerberosEnabled_UpdatesAuthModeIndex()
    {
        _vm.KerberosEnabled = true;

        Assert.Equal(1, _vm.AuthModeIndex);
    }

    [Fact]
    public void KerberosEnabled_False_UpdatesAuthModeIndex()
    {
        _vm.KerberosEnabled = true;
        _vm.KerberosEnabled = false;

        Assert.Equal(0, _vm.AuthModeIndex);
    }

    // ── AuthRequired ─────────────────────────────────

    [Fact]
    public void AuthRequired_False_ResetsAuthMode()
    {
        _vm.AuthRequired = true;
        _vm.AuthModeIndex = 1; // Kerberos
        _vm.AuthRequired = false;

        Assert.Equal(0, _vm.AuthModeIndex);
        Assert.False(_vm.KerberosEnabled);
    }

    // ── Rules management ─────────────────────────────

    [Fact]
    public void AddRuleCommand_AddsNewRule()
    {
        _vm.AddRuleCommand.Execute(null);

        Assert.Single(_vm.Rules);
        Assert.Equal("*", _vm.Rules[0].Pattern);
        Assert.Equal(RuleAction.Proxy, _vm.Rules[0].Action);
        Assert.Equal(RuleType.ProcessName, _vm.Rules[0].Type);
        Assert.True(_vm.Rules[0].Enabled);
    }

    [Fact]
    public void DeleteRuleCommand_RemovesSelectedRule()
    {
        _vm.AddRuleCommand.Execute(null);
        _vm.AddRuleCommand.Execute(null);
        Assert.Equal(2, _vm.Rules.Count);

        _vm.SelectedRule = _vm.Rules[0];
        _vm.DeleteRuleCommand.Execute(_vm.Rules[0]);

        Assert.Single(_vm.Rules);
    }

    [Fact]
    public void DeleteRuleCommand_DoesNothing_WhenNoSelection()
    {
        _vm.AddRuleCommand.Execute(null);
        _vm.SelectedRule = null;

        _vm.DeleteRuleCommand.Execute(null);

        Assert.Single(_vm.Rules);
    }

    // ── LogLevelFilter ───────────────────────────────

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(3)]
    [InlineData(4)]
    public void LogLevelFilter_AcceptsValidValues(int level)
    {
        _vm.LogLevelFilter = level;

        Assert.Equal(level, _vm.LogLevelFilter);
    }

    // ── Save command ─────────────────────────────────

    [Fact]
    public async Task SaveCommand_WhenNotConnected_ShowsError()
    {
        _svcMock.Setup(s => s.IsConnected).Returns(false);

        await _vm.SaveCommand.ExecuteAsync(null);

        Assert.Contains("connect", _vm.Msg.ToLower());
    }

    [Fact]
    public async Task SaveCommand_WhenConnected_CallsSetConfig()
    {
        _svcMock.Setup(s => s.IsConnected).Returns(true);
        _svcMock.Setup(s => s.SetConfigAsync(It.IsAny<ProxyConfig>())).ReturnsAsync(true);
        _svcMock.Setup(s => s.SetRulesAsync(It.IsAny<List<Rule>>())).ReturnsAsync(true);
        _svcMock.Setup(s => s.SetLogLevelAsync(It.IsAny<int>())).ReturnsAsync(true);

        await _vm.SaveCommand.ExecuteAsync(null);

        _svcMock.Verify(s => s.SetConfigAsync(It.IsAny<ProxyConfig>()), Times.Once);
        _svcMock.Verify(s => s.SetRulesAsync(It.IsAny<List<Rule>>()), Times.Once);
        _svcMock.Verify(s => s.SetLogLevelAsync(It.IsAny<int>()), Times.Once);
    }
}