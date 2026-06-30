using TcpRedirectorGUI.Domain.Entities;

namespace TcpRedirectorGUI.Domain.Ports;

/// <summary>
/// Interface for reading/writing the TcpRedirector configuration file.
/// This is the PRIMARY config source; IPC is secondary for live sync.
/// </summary>
public interface IConfigRepository
{
    string ReadString(string section, string key, string defaultValue = "");
    bool ReadBool(string section, string key, bool defaultValue = false);
    int ReadPort(string key, int defaultValue = 3128);
    int ReadInt(string section, string key, int defaultValue = 0);
    List<Rule> ReadRules();
    void WriteFull(ProxyConfig config, string exePath, List<Rule> rules);
    void WriteInt(string section, string key, int value);
}