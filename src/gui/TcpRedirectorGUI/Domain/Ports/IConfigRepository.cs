using TcpRedirectorGUI.Domain.Entities;

namespace TcpRedirectorGUI.Domain.Ports;

/// <summary>
/// Read-only config access for GUI bootstrap.
/// v1.1.0: all writes go through IPC (ITcpRedirectorService.SetConfigAsync).
/// </summary>
public interface IConfigRepository
{
    string ReadString(string section, string key, string defaultValue = "");
    bool ReadBool(string section, string key, bool defaultValue = false);
    int ReadPort(string key, int defaultValue = 3128);
    int ReadInt(string section, string key, int defaultValue = 0);
    List<Rule> ReadRules();
}