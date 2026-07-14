using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;
using TcpRedirectorGUI.Domain.Entities;
using TcpRedirectorGUI.Domain.Ports;

namespace TcpRedirectorGUI.Infrastructure.Config;

/// <summary>
/// Reads/writes the TcpRedirector config.json in %ProgramData%\TcpRedirector\.
/// Thread-safe: re-reads from disk on every call (config is small, <1KB).
/// Creates file + directory automatically if missing.
/// Maps between backend JSON schema and .NET entity types.
/// </summary>
public sealed class JsonConfigRepository : IConfigRepository
{
    private readonly string _configPath;

    public JsonConfigRepository()
    {
        _configPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
            "TcpRedirector", "config.json");
    }

    // ── Read ─────────────────────────────────────────

    public string ReadString(string section, string key, string defaultValue = "")
    {
        try
        {
            var j = Load();
            return j[section]?[key]?.GetValue<string>() ?? defaultValue;
        }
        catch
        {
            return defaultValue;
        }
    }

    public bool ReadBool(string section, string key, bool defaultValue = false)
    {
        try
        {
            var j = Load();
            return j[section]?[key]?.GetValue<bool>() ?? defaultValue;
        }
        catch
        {
            return defaultValue;
        }
    }

    public int ReadPort(string key, int defaultValue = 3128)
    {
        var s = ReadString("proxy", key, defaultValue.ToString());
        return int.TryParse(s, out var p) ? p : defaultValue;
    }

    public int ReadInt(string section, string key, int defaultValue = 0)
    {
        try
        {
            var j = Load();
            return j[section]?[key]?.GetValue<int>() ?? defaultValue;
        }
        catch
        {
            return defaultValue;
        }
    }

    public List<Rule> ReadRules()
    {
        try
        {
            var j = Load();
            var arr = j["rules"]?.AsArray();
            if (arr is null || arr.Count == 0) return [];

            var list = new List<Rule>(arr.Count);
            foreach (var item in arr)
            {
                var obj = item?.AsObject();
                if (obj is null) continue;

                list.Add(new Rule
                {
                    Id = obj["id"]?.GetValue<string>() ?? Guid.NewGuid().ToString(),
                    Pattern = obj["pattern"]?.GetValue<string>() ?? "",
                    Description = obj["description"]?.GetValue<string>() ?? "",
                    Priority = obj["priority"]?.GetValue<int>() ?? 0,
                    Enabled = obj["enabled"]?.GetValue<bool>() ?? true,
                    Type = (RuleType)MapTypeFromJson(obj["type"]),
                    Action = (RuleAction)MapActionFromJson(obj["action"])
                });
            }
            return list;
        }
        catch
        {
            return [];
        }
    }

    // v1.1.0: Write methods removed — all config changes go through IPC

    // ── Private helpers ──────────────────────────────

    private JsonObject Load()
    {
        EnsureFileExists();
        var text = File.ReadAllText(_configPath);
        if (string.IsNullOrWhiteSpace(text))
            return ResetToEmpty();
        return JsonNode.Parse(text)?.AsObject() ?? new JsonObject();
    }

    private void Save(JsonObject j)
    {
        var dir = Path.GetDirectoryName(_configPath);
        if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir))
            Directory.CreateDirectory(dir);

        var json = j.ToJsonString(new JsonSerializerOptions { WriteIndented = true });
        // Atomic write: write to temp, then rename
        var tmp = _configPath + ".tmp";
        File.WriteAllText(tmp, json);
        File.Move(tmp, _configPath, overwrite: true);
    }

    private void EnsureFileExists()
    {
        var dir = Path.GetDirectoryName(_configPath);
        if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir))
            Directory.CreateDirectory(dir);

        if (!File.Exists(_configPath))
            File.WriteAllText(_configPath, "{ }");
    }

    private JsonObject ResetToEmpty()
    {
        var root = new JsonObject();
        Save(root);
        return root;
    }

    /// <summary>Map JSON value (int or string) to RuleType int.</summary>
    private static int MapTypeFromJson(JsonNode? node)
    {
        if (node is JsonValue val)
        {
            if (val.TryGetValue<int>(out var i)) return i;
            if (val.TryGetValue<string>(out var s))
            {
                return s.ToLowerInvariant() switch
                {
                    "process_path" => 1,
                    "global" => 2,
                    _ => 0 // process_name
                };
            }
        }
        return 0;
    }

    /// <summary>Map JSON value (int or string) to RuleAction int.</summary>
    private static int MapActionFromJson(JsonNode? node)
    {
        if (node is JsonValue val)
        {
            if (val.TryGetValue<int>(out var i)) return i;
            if (val.TryGetValue<string>(out var s))
            {
                return s.ToLowerInvariant() switch
                {
                    "direct" => 1,
                    "block" => 2,
                    _ => 0 // proxy
                };
            }
        }
        return 0;
    }
}