namespace TcpRedirectorGUI.Domain.Entities;

// ===========================================================================
// WP5 — Schema-v2 domain entities.
// These types mirror the C++ service structures introduced in WP3
// (see plans/WINTUN_INTEGRATION_PLAN.md §3, §5, §11-WP5).
//
// Kept in a sibling file (not merged into ProxyConfig.cs) to keep the
// original proxy/auth container focused. JSON binding lives in
// Infrastructure/Config/JsonConfigRepository.cs — these entities are
// pure POCOs with no attribute noise so they stay easy to unit-test.
// ===========================================================================

/// <summary>
/// Active capture engine. Mirrors C++ <c>infrastructure::CaptureMode</c>.
/// Serialized to JSON as lowercase strings (<c>"windivert"</c> / <c>"wintun"</c>).
/// </summary>
public enum CaptureMode
{
    /// <summary>Classic WinDivert-based capture (default / backward-compat).</summary>
    WinDivert = 0,

    /// <summary>Wintun + tun2socks user-mode engine (v2 addition).</summary>
    Wintun = 1
}

/// <summary>
/// Kind of tun2socks-engine driving Wintun. Only consulted when
/// <see cref="CaptureMode"/> is <see cref="CaptureMode.Wintun"/>.
/// Serialized to JSON as lowercase strings (<c>"embedded"</c> / <c>"external"</c>).
/// </summary>
public enum WintunEngineKind
{
    /// <summary>Embedded (in-process) lwIP engine — default.</summary>
    Embedded = 0,

    /// <summary>External <c>tun2socks.exe</c> child process.</summary>
    External = 1
}

/// <summary>
/// Inclusive TCP port range <c>[From; To]</c>. Both endpoints must be in 1..65535
/// and <c>From ≤ To</c>. Enforced by
/// <see cref="Infrastructure.Config.PortSpecParser"/> on the way in.
/// </summary>
public sealed class PortRange
{
    public int From { get; set; }
    public int To { get; set; }

    /// <summary>Human-readable form, e.g. <c>"8000-8100"</c> or <c>"80"</c> if degenerate.</summary>
    public override string ToString() => From == To ? $"{From}" : $"{From}-{To}";
}

/// <summary>
/// External tun2socks child-process settings. Mirrors C++
/// <c>infrastructure::ExternalEngineSettings</c>.
/// </summary>
public sealed class ExternalEngineSettings
{
    public string Executable { get; set; } = ".bin/tun2socks/tun2socks.exe";
    public List<string> ExtraArgs { get; set; } = new();
    public string Socks5Listen { get; set; } = "127.0.0.1:1080";
    public bool RestartOnCrash { get; set; } = true;
    public int RestartBackoffMs { get; set; } = 2000;
}

/// <summary>
/// Wintun adapter + engine settings. Mirrors C++
/// <c>infrastructure::WintunSettings</c>.
/// </summary>
public sealed class WintunSettings
{
    public string AdapterName { get; set; } = "TcpRedirector";
    public string AdapterGuid { get; set; } = "";
    public string TunnelIpv4Cidr { get; set; } = "10.6.7.1/24";
    public string TunnelIpv6Cidr { get; set; } = "";
    public int Mtu { get; set; } = 1500;
    public WintunEngineKind Engine { get; set; } = WintunEngineKind.Embedded;

    /// <summary>
    /// Enables per-process filtering in the Wintun embedded engine. Mirrors
    /// C++ <c>wintun.process_filter_enabled</c> (default <c>true</c>). Must be
    /// round-tripped so it is not lost when the GUI rewrites the config.
    /// </summary>
    public bool ProcessFilterEnabled { get; set; } = true;

    /// <summary>
    /// Depth of the split-tunnel route ladder (number of /N prefixes installed).
    /// Mirrors C++ <c>wintun.route_ladder_prefix</c> (default <c>5</c>, valid
    /// range <c>1..8</c>). Must be round-tripped so the GUI does not silently
    /// drop it when it rewrites the whole <c>wintun</c> section on save.
    /// </summary>
    public int RouteLadderPrefix { get; set; } = 5;

    /// <summary>
    /// Neutralize IPv6 while Wintun capture is active (embedded is IPv4-only).
    /// Mirrors C++ <c>wintun.block_ipv6</c> (default <c>true</c>). When enabled,
    /// the service routes all IPv6 into the TUN and the embedded engine answers
    /// TCP RST to IPv6 SYNs, forcing an immediate IPv4 fallback. Must be
    /// round-tripped so the GUI does not silently drop it when it rewrites the
    /// whole <c>wintun</c> section on save.
    /// </summary>
    public bool BlockIpv6 { get; set; } = true;

    public ExternalEngineSettings ExternalEngine { get; set; } = new();
}

/// <summary>
/// One per-app rule from <c>apps[]</c>. Mirrors C++
/// <c>infrastructure::AppRule</c>. A rule matches when the process pattern
/// matches AND (<see cref="RouteAllTraffic"/> OR the destination port is in
/// <see cref="Ports"/> OR <see cref="PortRanges"/>).
/// </summary>
public sealed class AppRule
{
    /// <summary>Full path of the target EXE (optional, informational).</summary>
    public string ExePath { get; set; } = "";

    /// <summary>Process-name / process-path pattern (wildcards allowed by C++ matcher).</summary>
    public string Pattern { get; set; } = "";

    /// <summary>Proxy identifier — currently always <c>"default"</c> (see WP3).</summary>
    public string ProxyId { get; set; } = "default";

    /// <summary>When true, <see cref="Ports"/> and <see cref="PortRanges"/> are ignored.</summary>
    public bool RouteAllTraffic { get; set; }

    /// <summary>Individual TCP ports (1..65535, deduplicated).</summary>
    public List<int> Ports { get; set; } = new();

    /// <summary>Inclusive TCP port ranges.</summary>
    public List<PortRange> PortRanges { get; set; } = new();
}

/// <summary>
/// Aggregate v2 configuration surface. The GUI hands this whole object to
/// <see cref="Infrastructure.Config.JsonConfigRepository"/> when saving; the
/// repository is responsible for merging with existing on-disk fields
/// (proxy, auth, log, stats, DPAPI blobs, …) — those are unchanged in v2.
///
/// <see cref="ProxyConfig"/> is intentionally left focused on
/// host/port/auth. This container adds the v2-only fields side-by-side.
/// </summary>
public sealed class AppConfig
{
    public int ConfigVersion { get; set; } = 2;
    public CaptureMode CaptureMode { get; set; } = CaptureMode.WinDivert;
    public WintunSettings Wintun { get; set; } = new();
    public List<AppRule> Apps { get; set; } = new();
}
