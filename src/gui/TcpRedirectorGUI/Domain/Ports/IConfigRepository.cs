using TcpRedirectorGUI.Domain.Entities;

namespace TcpRedirectorGUI.Domain.Ports;

/// <summary>
/// Interface for reading/writing the TcpRedirector configuration file.
/// This is the PRIMARY config source; IPC is secondary for live sync.
/// </summary>
public interface IConfigRepository
{
    /// <summary>
    /// Absolute path of the <c>config.json</c> this repository reads/writes.
    /// Exposed so the GUI can show the user exactly where settings were saved
    /// and perform an explicit read-back verification against the same file.
    /// </summary>
    string ConfigPath { get; }

    // ── Generic getters (unchanged from WP2) ─────────
    string ReadString(string section, string key, string defaultValue = "");
    bool ReadBool(string section, string key, bool defaultValue = false);
    int ReadPort(string key, int defaultValue = 3128);
    int ReadInt(string section, string key, int defaultValue = 0);

    // ── Legacy v1 API ────────────────────────────────
    List<Rule> ReadRules();
    bool WriteFull(ProxyConfig config, string exePath, List<Rule> rules);
    void WriteInt(string section, string key, int value);

    /// <summary>
    /// (Задача №1) Записывает строковое значение в <c>section.key</c>, сохраняя
    /// остальные поля (merge). Используется для GUI-настроек, напр.
    /// <c>gui.language</c>.
    /// </summary>
    void WriteString(string section, string key, string value);

    // ── WP5 v2 API ───────────────────────────────────

    /// <summary>Reads <c>capture_mode</c> from the top-level object. Defaults to <see cref="CaptureMode.WinDivert"/>.</summary>
    CaptureMode ReadCaptureMode();

    /// <summary>Reads the <c>wintun{…}</c> block. Missing fields fall back to defaults from <see cref="WintunSettings"/>.</summary>
    WintunSettings ReadWintunSettings();

    /// <summary>
    /// Reads <c>apps[]</c>. If <c>apps[]</c> is missing but legacy <c>rules[]</c>
    /// is present, synthesises <see cref="AppRule"/> entries (v1 → v2 upgrade
    /// mirroring the C++ side).
    /// </summary>
    List<AppRule> ReadApps();

    /// <summary>
    /// Writes the full v2 payload (<c>config_version</c>, <c>capture_mode</c>,
    /// <c>wintun{…}</c>, <c>apps[]</c>) alongside the classic proxy/auth/log/stats
    /// sections and a mirrored legacy <c>rules[]</c> array for backward-compat.
    /// Preserves the existing atomic-write pattern.
    /// </summary>
    bool WriteFullV2(
        ProxyConfig config,
        CaptureMode captureMode,
        WintunSettings wintun,
        List<AppRule> apps);
}
