using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using TcpRedirectorGUI.Domain.Entities;
using TcpRedirectorGUI.Domain.Ports;

namespace TcpRedirectorGUI.Infrastructure.Config;

/// <summary>
/// Reads/writes the TcpRedirector <c>config.json</c> from the install
/// directory shared with the Windows service (resolved via
/// <see cref="AppPaths.GetConfigPath"/>). Runs a one-shot migration from
/// <c>%ProgramData%\TcpRedirector\config.json</c> on first launch after
/// upgrade.
///
/// Thread-safe in the sense that every call re-reads from disk (config is
/// small, &lt;1KB). Creates the file automatically on first access.
/// Maps between backend JSON schema and .NET entity types.
///
/// WP5: v2 schema support added. On write, both the v2 shape
/// (<c>config_version</c>, <c>capture_mode</c>, <c>wintun{…}</c>, <c>apps[]</c>)
/// and the legacy <c>rules[]</c> mirror are emitted so pre-WP3 service
/// binaries can still parse the file. JSON field names use snake_case to
/// match the C++ side (WP3, <see cref="ConfigManager.cpp"/>).
/// </summary>
public sealed class JsonConfigRepository : IConfigRepository
{
    private const int kLegacyRulesCap = 128;

    private readonly string _configPath;

    /// <inheritdoc/>
    public string ConfigPath => _configPath;

    public JsonConfigRepository()
    {
        // One-shot migration from %ProgramData%. Never throws.
        AppPaths.EnsureConfigMigrated();
        _configPath = AppPaths.GetConfigPath();
    }

    /// <summary>
    /// Test / advanced-scenario overload — bypasses path resolution and
    /// migration. Callers are responsible for pointing at a valid location.
    /// </summary>
    public JsonConfigRepository(string configPath)
    {
        if (string.IsNullOrWhiteSpace(configPath))
            throw new ArgumentException("configPath must be a non-empty path", nameof(configPath));
        _configPath = configPath;
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

    // ── WP5 v2 read paths ────────────────────────────

    public CaptureMode ReadCaptureMode()
    {
        try
        {
            var j = Load();
            var s = j["capture_mode"]?.GetValue<string>();
            return CaptureModeFromString(s);
        }
        catch
        {
            // Дефолт после установки — Wintun (embedded).
            return CaptureMode.Wintun;
        }
    }

    public WintunSettings ReadWintunSettings()
    {
        var w = new WintunSettings();
        try
        {
            var j = Load();
            var jw = j["wintun"]?.AsObject();
            if (jw is null) return w;

            w.AdapterName    = jw["adapter_name"]?.GetValue<string>() ?? w.AdapterName;
            w.AdapterGuid    = jw["adapter_guid"]?.GetValue<string>() ?? w.AdapterGuid;
            w.TunnelIpv4Cidr = jw["tunnel_ipv4_cidr"]?.GetValue<string>() ?? w.TunnelIpv4Cidr;
            w.TunnelIpv6Cidr = jw["tunnel_ipv6_cidr"]?.GetValue<string>() ?? w.TunnelIpv6Cidr;

            if (jw["mtu"] is JsonNode mtuNode && mtuNode is JsonValue mtuVal && mtuVal.TryGetValue<int>(out var mtu))
                w.Mtu = mtu;

            w.Engine = WintunEngineFromString(jw["engine"]?.GetValue<string>());

            // process_filter_enabled (embedded-engine per-process filtering).
            // Missing → default true, matching the C++ service default.
            if (jw["process_filter_enabled"] is JsonValue pfVal && pfVal.TryGetValue<bool>(out var pf))
                w.ProcessFilterEnabled = pf;

            // route_ladder_prefix — split-tunnel route ladder depth. Missing →
            // default 5. Out-of-range values are clamped to 1..8, matching the
            // C++ service (ConfigManager.cpp).
            if (jw["route_ladder_prefix"] is JsonValue rlpVal && rlpVal.TryGetValue<int>(out var rlp))
                w.RouteLadderPrefix = Math.Clamp(rlp, 1, 8);

            // block_ipv6 — neutralize IPv6 in Wintun mode. Missing → default
            // true, matching the C++ service default.
            if (jw["block_ipv6"] is JsonValue biVal && biVal.TryGetValue<bool>(out var bi))
                w.BlockIpv6 = bi;

            var je = jw["external_engine"]?.AsObject();
            if (je is not null)
            {
                w.ExternalEngine.Executable   = je["executable"]?.GetValue<string>()   ?? w.ExternalEngine.Executable;
                w.ExternalEngine.Socks5Listen = je["socks5_listen"]?.GetValue<string>() ?? w.ExternalEngine.Socks5Listen;

                if (je["restart_on_crash"] is JsonValue rocVal && rocVal.TryGetValue<bool>(out var roc))
                    w.ExternalEngine.RestartOnCrash = roc;

                if (je["restart_backoff_ms"] is JsonValue rbVal && rbVal.TryGetValue<int>(out var rb))
                    w.ExternalEngine.RestartBackoffMs = rb;

                var extra = je["extra_args"]?.AsArray();
                if (extra is not null)
                {
                    w.ExternalEngine.ExtraArgs.Clear();
                    foreach (var a in extra)
                    {
                        var s = a?.GetValue<string>();
                        if (!string.IsNullOrEmpty(s))
                            w.ExternalEngine.ExtraArgs.Add(s);
                    }
                }
            }
        }
        catch
        {
            // Any parse failure → return defaults (already partially populated).
        }
        return w;
    }

    public List<AppRule> ReadApps()
    {
        var apps = new List<AppRule>();
        try
        {
            var j = Load();
            var arr = j["apps"]?.AsArray();
            if (arr is not null)
            {
                foreach (var item in arr)
                {
                    var obj = item?.AsObject();
                    if (obj is null) continue;
                    var a = ParseAppRule(obj);
                    if (a is not null) apps.Add(a);
                }
                return apps;
            }

            // v1 → v2 in-memory upgrade: synthesise apps[] from legacy rules[].
            // Mirrors ConfigManager::UpgradeLegacyRulesToApps (C++ WP3).
            var rules = j["rules"]?.AsArray();
            if (rules is null) return apps;
            foreach (var item in rules)
            {
                var obj = item?.AsObject();
                if (obj is null) continue;

                // v1 rules had either "exe" or "pattern"; prefer "exe" (WP3 spec).
                var exeName = obj["exe"]?.GetValue<string>();
                if (string.IsNullOrEmpty(exeName))
                    exeName = obj["pattern"]?.GetValue<string>();
                if (string.IsNullOrEmpty(exeName)) continue;

                // proxyId (legacy) or proxy_id (neutral).
                var proxyId = obj["proxyId"]?.GetValue<string>();
                if (string.IsNullOrEmpty(proxyId))
                    proxyId = obj["proxy_id"]?.GetValue<string>();
                if (string.IsNullOrEmpty(proxyId)) proxyId = "default";

                var app = new AppRule
                {
                    Pattern         = exeName,
                    ProxyId         = proxyId,
                    RouteAllTraffic = false
                };

                // v1 held one port under "port". 0 or absent → ports[]=[].
                if (obj["port"] is JsonValue pv && pv.TryGetValue<int>(out var p) && p >= 1 && p <= 65535)
                    app.Ports.Add(p);

                apps.Add(app);
            }
        }
        catch
        {
            // Parse failure — return whatever we accumulated.
        }
        return apps;
    }

    // ── Write ────────────────────────────────────────

    public bool WriteFull(ProxyConfig config, string exePath, List<Rule> rules)
    {
        try
        {
            // Preserve existing log level from current config
            var logLevel = ReadInt("log", "level", 2);

            var j = new JsonObject
            {
                ["app"] = new JsonObject { ["exePath"] = exePath },
                ["proxy"] = new JsonObject
                {
                    ["host"] = config.Host,
                    ["port"] = config.Port,
                    ["enabled"] = true
                },
                ["auth"] = new JsonObject
                {
                    ["enabled"] = config.AuthRequired,
                    ["username"] = config.Login,
                    ["kerberos"] = config.KerberosEnabled
                },
                ["log"] = new JsonObject
                {
                    ["level"] = logLevel,
                    ["fileEnabled"] = true,
                    ["maxSizeMB"] = 50
                },
                ["stats"] = new JsonObject
                {
                    ["updateIntervalMs"] = 2000
                }
            };

            // Preserve DPAPI-encrypted password from existing file
            var encPwd = ReadString("auth", "encryptedPassword");
            if (!string.IsNullOrEmpty(encPwd))
                j["auth"]!["encryptedPassword"] = encPwd;

            // Rules — written as strings (backend JsonToRules expects this)
            var rulesArr = new JsonArray();
            foreach (var r in rules)
            {
                rulesArr.Add(new JsonObject
                {
                    ["id"] = r.Id,
                    ["pattern"] = r.Pattern,
                    ["description"] = r.Description,
                    ["priority"] = r.Priority,
                    ["enabled"] = r.Enabled,
                    ["type"] = r.Type switch
                    {
                        RuleType.ProcessPath => "process_path",
                        RuleType.Global => "global",
                        _ => "process_name"
                    },
                    ["action"] = r.Action switch
                    {
                        RuleAction.Direct => "direct",
                        RuleAction.Block => "block",
                        _ => "proxy"
                    }
                });
            }
            j["rules"] = rulesArr;

            Save(j);
            return true;
        }
        catch
        {
            return false;
        }
    }

    public bool WriteFullV2(
        ProxyConfig config,
        CaptureMode captureMode,
        WintunSettings wintun,
        List<AppRule> apps)
    {
        try
        {
            // B6 (QA audit): MERGE onto the existing config object instead of
            // rebuilding it from scratch. Rebuilding dropped every field the GUI
            // does not model — stats.updateIntervalMs (clobbered to 2000 even
            // though it is read with a 1000 default elsewhere), stats.graphWindowSec,
            // log.fileEnabled, log.maxSizeMB, proxy.enabled, and any unknown key a
            // future service adds. Merging preserves them; the GUI only sets the
            // fields it actually owns. auth.encryptedPassword is preserved
            // automatically because we start from the on-disk object.
            JsonObject j;
            try { j = Load(); }
            catch { j = BuildDefaultSkeleton(); } // corrupt/unreadable file -> fresh skeleton

            j["config_version"] = 2;
            j["capture_mode"]   = CaptureModeToString(captureMode);
            j["wintun"]         = WintunToJson(wintun);   // fully GUI-owned section

            // app.exePath — keep existing; derive from apps[] only if empty.
            var existingExePath = j["app"]?["exePath"]?.GetValue<string>() ?? "";
            var exePath = string.IsNullOrEmpty(existingExePath)
                ? apps.FirstOrDefault(a => !string.IsNullOrEmpty(a.ExePath))?.ExePath
                  ?? apps.FirstOrDefault(a => !string.IsNullOrEmpty(a.Pattern))?.Pattern
                  ?? ""
                : existingExePath;
            GetOrCreateObject(j, "app")["exePath"] = exePath;

            var proxy = GetOrCreateObject(j, "proxy");
            proxy["host"] = config.Host;
            proxy["port"] = config.Port;
            if (proxy["enabled"] is null) proxy["enabled"] = true; // preserve if already set

            var auth = GetOrCreateObject(j, "auth");
            auth["enabled"]  = config.AuthRequired;
            auth["kerberos"] = config.KerberosEnabled;
            // (Задача №2) Режим хранения пароля.
            auth["encryptPassword"] = config.EncryptPassword;

            // ── Per-user Kerberos auth helper (Phase 9) ──────────────────
            // GUI owns these four keys; merge them onto the existing "auth"
            // object so unknown siblings are preserved. fallback_policy is kept
            // verbatim on the wire ("drop"/"system"/"error"); an unexpected
            // value is normalised to the safe default "drop".
            auth["per_user_enabled"] = config.PerUserEnabled;
            auth["spn"]              = config.Spn ?? "";
            auth["fallback_policy"]  = NormalizeFallbackPolicy(config.FallbackPolicy);
            auth["helper_timeout_ms"] = config.HelperTimeoutMs;

            // (Задача №2) Нормализация auth-полей ВСЕГДА, независимо от того,
            // ввёл ли пользователь новый пароль. Ранее очистка неактивных полей
            // выполнялась только внутри `if (пароль введён)`, поэтому при смене
            // encryptPassword / переключении Basic↔Kerberos / отключении auth
            // без повторного ввода пароля «мусорные» поля оставались в файле.
            //
            // Правила (какие поля актуальны):
            //   • auth выключен            → username/encryptedPassword/password = "" ;
            //   • Kerberos                 → username/encryptedPassword/password = ""
            //                                 (SSPI: креды не хранятся);
            //   • Basic + encryptPassword  → password = "" (plaintext не нужен),
            //                                 encryptedPassword — новый или прежний;
            //   • Basic + !encryptPassword → encryptedPassword = "" (DPAPI не нужен),
            //                                 password — новый или прежний.
            var existingEnc   = auth["encryptedPassword"]?.GetValue<string>() ?? "";
            var existingPlain = auth["password"]?.GetValue<string>() ?? "";

            if (!config.AuthRequired || config.KerberosEnabled)
            {
                // Неактивная авторизация или Kerberos — креды не хранятся
                // (Kerberos использует SSPI-контекст текущей учётной записи).
                auth["username"]          = "";
                auth["encryptedPassword"] = "";
                auth["password"]          = "";
            }
            else if (config.EncryptPassword)
            {
                // Basic + шифрование: plaintext-поле всегда пустое.
                auth["username"] = config.Login;
                auth["password"] = "";
                if (!string.IsNullOrEmpty(config.Password))
                {
                    var enc = EncryptPasswordDpapi(config.Password);
                    // Не затираем прежний секрет, если новое шифрование не удалось.
                    auth["encryptedPassword"] = string.IsNullOrEmpty(enc) ? existingEnc : enc;
                }
                else
                {
                    // Новый пароль не введён — сохраняем прежний зашифрованный.
                    auth["encryptedPassword"] = existingEnc;
                }
            }
            else
            {
                // Basic + plaintext: encrypted-поле всегда пустое.
                auth["username"]          = config.Login;
                auth["encryptedPassword"] = "";
                // Новый пароль не введён — сохраняем прежний plaintext.
                auth["password"] = string.IsNullOrEmpty(config.Password) ? existingPlain : config.Password;
            }
            // (Задача №1) IPC-настройки — GUI владеет флагом auth_enabled.
            var ipc = GetOrCreateObject(j, "ipc");
            ipc["auth_enabled"] = config.IpcAuthEnabled;

            // log.* — GUI owns "level" (written elsewhere on the service scale via
            // WriteInt); only seed the others if absent so we never reset them.
            var log = GetOrCreateObject(j, "log");
            if (log["level"] is null)       log["level"] = 2;
            if (log["fileEnabled"] is null) log["fileEnabled"] = true;
            if (log["maxSizeMB"] is null)   log["maxSizeMB"] = 50;

            // stats.* — never clobber; only seed a default interval if missing.
            var stats = GetOrCreateObject(j, "stats");
            if (stats["updateIntervalMs"] is null) stats["updateIntervalMs"] = 2000;

            j["apps"]  = AppsToJson(apps);
            j["rules"] = BuildLegacyRulesMirror(apps);

            Save(j);
            return true;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// Encrypts <paramref name="plaintext"/> with DPAPI in
    /// <see cref="DataProtectionScope.LocalMachine"/> scope and returns the
    /// standard Base64 of the blob. Machine scope is mandatory: the GUI runs as
    /// an admin user while the service runs as LocalSystem, and only a
    /// machine-scoped blob is decryptable by both. The plaintext is encoded as
    /// UTF-16LE (<see cref="Encoding.Unicode"/>) so the C++ service's
    /// <c>ConfigManager::DecryptPassword</c>, which reads the blob back as a
    /// <c>wchar_t*</c>, gets a matching byte layout. Returns "" on failure.
    /// </summary>
    private static string EncryptPasswordDpapi(string plaintext)
    {
        try
        {
            var bytes = Encoding.Unicode.GetBytes(plaintext);
            var blob = ProtectedData.Protect(bytes, null, DataProtectionScope.LocalMachine);
            return Convert.ToBase64String(blob);
        }
        catch
        {
            return "";
        }
    }

    /// <summary>
    /// Normalises an <c>auth.fallback_policy</c> value to exactly one of the
    /// three wire tokens. Any unrecognised/empty value collapses to the safe
    /// default <c>"drop"</c> (never silently use the machine account).
    /// </summary>
    private static string NormalizeFallbackPolicy(string? policy) =>
        (policy?.Trim().ToLowerInvariant()) switch
        {
            "system" => "system",
            "error"  => "error",
            _        => "drop"
        };

    /// <summary>Returns the child object at <paramref name="key"/>, creating an
    /// empty one (and attaching it) if it is absent or not an object. Used by the
    /// merge-based writers so unknown sibling keys are preserved (B6).</summary>
    private static JsonObject GetOrCreateObject(JsonObject parent, string key)
    {
        if (parent[key] is JsonObject existing) return existing;
        var created = new JsonObject();
        parent[key] = created;
        return created;
    }

    public void WriteInt(string section, string key, int value)
    {
        try
        {
            var j = Load();
            if (j[section] is null)
                j[section] = new JsonObject();
            j[section]![key] = value;
            Save(j);
        }
        catch
        {
            // File I/O errors are non-fatal
        }
    }

    // (Задача №1) Строковая запись с сохранением остальных полей (merge).
    public void WriteString(string section, string key, string value)
    {
        try
        {
            var j = Load();
            if (j[section] is null)
                j[section] = new JsonObject();
            j[section]![key] = value;
            Save(j);
        }
        catch
        {
            // File I/O errors are non-fatal
        }
    }

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
        {
            // Seed a default schema-preserving skeleton so the file appears
            // next to the install with a valid structure that mirrors what
            // WriteFullV2 produces.
            var seed = BuildDefaultSkeleton();
            Save(seed);
        }
    }

    private static JsonObject BuildDefaultSkeleton()
    {
        var defaults = new ProxyConfig();
        var wintun = new WintunSettings();
        return new JsonObject
        {
            ["config_version"] = 2,
            // Дефолт после установки/создания скелета — Wintun (embedded).
            ["capture_mode"]   = CaptureModeToString(CaptureMode.Wintun),
            ["wintun"]         = WintunToJson(wintun),
            ["app"] = new JsonObject { ["exePath"] = defaults.ExePath },
            ["proxy"] = new JsonObject
            {
                ["host"] = defaults.Host,
                ["port"] = defaults.Port,
                ["enabled"] = true
            },
            ["auth"] = new JsonObject
            {
                ["enabled"] = defaults.AuthRequired,
                ["username"] = defaults.Login,
                ["kerberos"] = defaults.KerberosEnabled,
                // Per-user Kerberos auth helper (Phase 9) — feature OFF defaults.
                ["per_user_enabled"]  = defaults.PerUserEnabled,
                ["spn"]               = defaults.Spn,
                ["fallback_policy"]   = defaults.FallbackPolicy,
                ["helper_timeout_ms"] = defaults.HelperTimeoutMs
            },
            ["log"] = new JsonObject
            {
                ["level"] = 2,
                ["fileEnabled"] = true,
                ["maxSizeMB"] = 50
            },
            ["stats"] = new JsonObject
            {
                ["updateIntervalMs"] = 2000
            },
            ["apps"]  = new JsonArray(),
            ["rules"] = new JsonArray()
        };
    }

    private JsonObject ResetToEmpty()
    {
        var root = new JsonObject();
        Save(root);
        return root;
    }

    // ---- v2 JSON helpers (snake_case, mirroring C++ WP3) ------------------

    private static string CaptureModeToString(CaptureMode m) => m switch
    {
        CaptureMode.Wintun => "wintun",
        _ => "windivert"
    };

    private static CaptureMode CaptureModeFromString(string? s)
    {
        // Пустое/отсутствующее значение → дефолт Wintun (embedded).
        if (string.IsNullOrEmpty(s)) return CaptureMode.Wintun;
        return s.Trim().ToLowerInvariant() switch
        {
            "wintun" => CaptureMode.Wintun,
            "windivert" => CaptureMode.WinDivert,
            _ => CaptureMode.Wintun
        };
    }

    private static string WintunEngineToString(WintunEngineKind e) => e switch
    {
        WintunEngineKind.External => "external",
        _ => "embedded"
    };

    private static WintunEngineKind WintunEngineFromString(string? s)
    {
        if (string.IsNullOrEmpty(s)) return WintunEngineKind.Embedded;
        return s.Trim().ToLowerInvariant() switch
        {
            "external" => WintunEngineKind.External,
            _ => WintunEngineKind.Embedded
        };
    }

    private static JsonObject WintunToJson(WintunSettings w)
    {
        var extraArgs = new JsonArray();
        foreach (var a in w.ExternalEngine.ExtraArgs)
            extraArgs.Add(a);

        return new JsonObject
        {
            ["adapter_name"]     = w.AdapterName,
            ["adapter_guid"]     = w.AdapterGuid,
            ["tunnel_ipv4_cidr"] = w.TunnelIpv4Cidr,
            ["tunnel_ipv6_cidr"] = w.TunnelIpv6Cidr,
            ["mtu"]                    = w.Mtu,
            ["engine"]                 = WintunEngineToString(w.Engine),
            ["process_filter_enabled"] = w.ProcessFilterEnabled,
            ["route_ladder_prefix"]    = w.RouteLadderPrefix,
            ["block_ipv6"]             = w.BlockIpv6,
            ["external_engine"]  = new JsonObject
            {
                ["executable"]          = w.ExternalEngine.Executable,
                ["extra_args"]          = extraArgs,
                ["socks5_listen"]       = w.ExternalEngine.Socks5Listen,
                ["restart_on_crash"]    = w.ExternalEngine.RestartOnCrash,
                ["restart_backoff_ms"]  = w.ExternalEngine.RestartBackoffMs
            }
        };
    }

    private static JsonArray AppsToJson(List<AppRule> apps)
    {
        var arr = new JsonArray();
        foreach (var a in apps)
        {
            var ports = new JsonArray();
            foreach (var p in a.Ports) ports.Add(p);

            var ranges = new JsonArray();
            foreach (var r in a.PortRanges)
                ranges.Add(new JsonObject { ["from"] = r.From, ["to"] = r.To });

            arr.Add(new JsonObject
            {
                ["exe_path"]          = a.ExePath,
                ["pattern"]           = a.Pattern,
                ["proxy_id"]          = string.IsNullOrEmpty(a.ProxyId) ? "default" : a.ProxyId,
                ["route_all_traffic"] = a.RouteAllTraffic,
                ["ports"]             = ports,
                ["port_ranges"]       = ranges
            });
        }
        return arr;
    }

    /// <summary>
    /// Emit a legacy v1 <c>rules[]</c> mirror from v2 <c>apps[]</c>. Matches
    /// C++ <see cref="ConfigManager.BuildLegacyRulesMirror"/> semantics:
    /// <c>route_all_traffic=true</c> apps are skipped (v1 cannot express
    /// any-port); discrete ports are always emitted; ranges are expanded
    /// only if the total flat-entry count stays ≤ 128. Truncation is silent.
    /// </summary>
    private static JsonArray BuildLegacyRulesMirror(List<AppRule> apps)
    {
        var arr = new JsonArray();
        var emitted = 0;
        var truncated = false;

        void Emit(string pattern, int port, string proxyId)
        {
            if (emitted >= kLegacyRulesCap) { truncated = true; return; }
            arr.Add(new JsonObject
            {
                // Write both keys: "exe" for legacy v1 readers and "pattern" for
                // the schema-v2 service reader (ConfigManager::JsonToRules). The
                // service now tolerates either, but emitting both keeps old and
                // new readers happy and avoids depending on the fallback path.
                ["exe"]     = pattern,
                ["pattern"] = pattern,
                ["port"]    = port,
                ["proxyId"] = string.IsNullOrEmpty(proxyId) ? "default" : proxyId
            });
            emitted++;
        }

        foreach (var a in apps)
        {
            if (truncated) break;
            if (string.IsNullOrEmpty(a.Pattern)) continue;
            if (a.RouteAllTraffic) continue;

            foreach (var p in a.Ports)
            {
                Emit(a.Pattern, p, a.ProxyId);
                if (truncated) break;
            }
            if (truncated) break;

            foreach (var r in a.PortRanges)
            {
                var span = r.To >= r.From ? (r.To - r.From + 1) : 0;
                if (span <= 0) continue;
                if (emitted + span > kLegacyRulesCap) { truncated = true; break; }
                for (int p = r.From; p <= r.To; p++)
                    Emit(a.Pattern, p, a.ProxyId);
            }
        }

        return arr;
    }

    private static AppRule? ParseAppRule(JsonObject obj)
    {
        var pattern = obj["pattern"]?.GetValue<string>();
        if (string.IsNullOrEmpty(pattern)) return null;

        var a = new AppRule
        {
            Pattern         = pattern,
            ExePath         = obj["exe_path"]?.GetValue<string>() ?? "",
            ProxyId         = obj["proxy_id"]?.GetValue<string>() ?? "default",
            RouteAllTraffic = obj["route_all_traffic"] is JsonValue rv && rv.TryGetValue<bool>(out var rb) && rb
        };

        var ports = obj["ports"]?.AsArray();
        if (ports is not null)
        {
            var seen = new HashSet<int>();
            foreach (var pn in ports)
            {
                if (pn is JsonValue pv && pv.TryGetValue<int>(out var p) && p >= 1 && p <= 65535)
                    if (seen.Add(p)) a.Ports.Add(p);
            }
        }

        var ranges = obj["port_ranges"]?.AsArray();
        if (ranges is not null)
        {
            foreach (var rn in ranges)
            {
                var ro = rn?.AsObject();
                if (ro is null) continue;
                if (ro["from"] is not JsonValue fv || !fv.TryGetValue<int>(out var from)) continue;
                if (ro["to"]   is not JsonValue tv || !tv.TryGetValue<int>(out var to))   continue;
                if (from < 1 || from > 65535 || to < 1 || to > 65535 || from > to) continue;
                a.PortRanges.Add(new PortRange { From = from, To = to });
            }
        }

        return a;
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
