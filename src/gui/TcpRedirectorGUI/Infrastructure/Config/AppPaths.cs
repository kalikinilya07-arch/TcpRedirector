using System;
using System.Diagnostics;
using System.IO;

namespace TcpRedirectorGUI.Infrastructure.Config;

/// <summary>
/// Resolves on-disk paths for the GUI. The GUI is installed under a
/// nested <c>gui\</c> subfolder next to the service executable so
/// <c>config.json</c> lives one directory above <see cref="AppContext.BaseDirectory"/>.
///
/// The service (<c>TcpRedirectorService.exe</c>) always reads/writes
/// <c>config.json</c> from <b>its own EXE directory</b> (resolved via
/// <c>GetModuleFileNameW</c>). To guarantee the GUI and the service use the
/// <b>same</b> file, <see cref="GetConfigPath"/> is anchored to the service
/// EXE directory whenever the service executable can be located.
///
/// Resolution order for <see cref="GetConfigPath"/>:
///   1. <c>&lt;serviceDir&gt;\config.json</c> — authoritative when
///      <c>TcpRedirectorService.exe</c> is found next to / above the GUI
///      (installed layout) or in a dev <c>build\</c> folder.
///   2. <c>&lt;BaseDir&gt;\config.json</c> — GUI + service co-located and no
///      distinct service dir resolved (portable/dev).
///   3. <c>&lt;BaseDir&gt;\..\config.json</c> — installed layout fallback when
///      the service EXE cannot be found but the parent dir is writable.
///
/// A stray <c>config.json</c> inside the nested <c>gui\</c> folder is NOT
/// preferred over the service's file — that would silently desync the two
/// processes. Never falls back to <c>%ProgramData%</c>, <c>%AppData%</c>, or
/// <see cref="Environment.CurrentDirectory"/> for the primary path.
/// </summary>
internal static class AppPaths
{
    private const string ConfigFileName = "config.json";
    private const string LegacyFolderName = "TcpRedirector";
    private const string ServiceExeName = "TcpRedirectorService.exe";

    /// <summary>
    /// Returns <see cref="AppContext.BaseDirectory"/>. Do NOT use
    /// <c>Assembly.GetExecutingAssembly().Location</c> — it is empty under
    /// single-file publish.
    /// </summary>
    public static string GetBaseDirectory() => AppContext.BaseDirectory;

    /// <summary>
    /// Parent directory of <see cref="GetBaseDirectory"/>. This is the
    /// installed layout's install root (containing the service EXE and the
    /// nested <c>gui\</c> folder). Falls back to the base directory if the
    /// parent cannot be determined (e.g. base is a drive root).
    /// </summary>
    public static string GetInstallDirectory()
    {
        var baseDir = GetBaseDirectory();
        // Trim any trailing separator so GetDirectoryName returns the parent
        // rather than the same directory.
        var trimmed = baseDir.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var parent = Path.GetDirectoryName(trimmed);
        return string.IsNullOrEmpty(parent) ? baseDir : parent;
    }

    /// <summary>
    /// Full path of <c>TcpRedirectorService.exe</c>, or <c>null</c> if it
    /// cannot be located. This is the SINGLE source of truth for locating the
    /// service: both <see cref="GetServiceDirectory"/> (config anchor) and the
    /// GUI launcher (<c>ShellViewModel.FindBackendExe</c>) resolve through here,
    /// so the launched service and the config file can never point at different
    /// directories.
    ///
    /// Candidate order (relative to <see cref="GetBaseDirectory"/>) — must match
    /// the layouts the launcher supports, build output first:
    ///   1. <c>..\..\build\</c>  (dev build output)
    ///   2. co-located           (portable / deploy\gui with service alongside)
    ///   3. <c>..\</c>            (installed: GUI in nested gui\ folder)
    ///   4. <c>..\..\</c>         (project-root dev layout)
    /// </summary>
    public static string? GetServiceExePath()
    {
        var baseDir = GetBaseDirectory();

        var candidates = new[]
        {
            Path.Combine(baseDir, "..", "..", "build", ServiceExeName),  // dev build\ output (launcher priority)
            Path.Combine(baseDir, ServiceExeName),                        // co-located
            Path.Combine(baseDir, "..", ServiceExeName),                  // installed: gui\ under install root
            Path.Combine(baseDir, "..", "..", ServiceExeName),            // project-root dev layout
        };

        foreach (var c in candidates)
        {
            try
            {
                var full = Path.GetFullPath(c);
                if (File.Exists(full))
                    return full;
            }
            catch
            {
                // Ignore malformed candidate paths.
            }
        }

        return null;
    }

    /// <summary>
    /// Directory that contains <c>TcpRedirectorService.exe</c>, or <c>null</c>
    /// if the service executable cannot be located. This is the authoritative
    /// anchor for <c>config.json</c> because the service reads/writes the file
    /// from its own EXE directory.
    /// </summary>
    public static string? GetServiceDirectory()
    {
        var exe = GetServiceExePath();
        return string.IsNullOrEmpty(exe) ? null : Path.GetDirectoryName(exe);
    }

    /// <summary>
    /// Resolves the <c>config.json</c> path per the class-level rule. Anchored
    /// to the service EXE directory when it can be found, so the GUI and the
    /// service always operate on the same file.
    /// </summary>
    public static string GetConfigPath()
    {
        var baseDir = GetBaseDirectory();
        var installDir = GetInstallDirectory();

        // 1. Authoritative: next to the service EXE (what the service actually uses).
        var serviceDir = GetServiceDirectory();
        if (!string.IsNullOrEmpty(serviceDir))
            return Path.Combine(serviceDir, ConfigFileName);

        // 2. Service EXE not found — fall back to existing files WITHOUT
        //    preferring a stray gui\config.json over the install-root file.
        var atParent = Path.Combine(installDir, ConfigFileName);
        var atBase = Path.Combine(baseDir, ConfigFileName);

        var haveDistinctParent =
            !string.Equals(installDir, baseDir, StringComparison.OrdinalIgnoreCase);

        if (haveDistinctParent && File.Exists(atParent))
            return atParent;
        if (File.Exists(atBase))
            return atBase;

        // 3. Nothing exists — pick the default target for a fresh save.
        //    Prefer the install (parent) directory when writable.
        if (haveDistinctParent
            && Directory.Exists(installDir)
            && IsDirectoryWritable(installDir))
        {
            return atParent;
        }

        return atBase;
    }

    /// <summary>
    /// Legacy <c>%ProgramData%\TcpRedirector\config.json</c> path used for
    /// the one-shot migration only.
    /// </summary>
    public static string GetLegacyProgramDataConfigPath() =>
        Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
            LegacyFolderName,
            ConfigFileName);

    /// <summary>
    /// One-shot migration from <c>%ProgramData%\TcpRedirector\config.json</c>
    /// to the resolved config path. No-ops if the target already exists or
    /// the legacy file is absent. Never throws — any failure is logged to
    /// <see cref="Debug"/> and swallowed so the GUI cannot be prevented from
    /// starting by a bad migration.
    /// </summary>
    public static void EnsureConfigMigrated()
    {
        try
        {
            var newPath = GetConfigPath();
            if (File.Exists(newPath))
                return;

            var legacyPath = GetLegacyProgramDataConfigPath();
            if (!File.Exists(legacyPath))
                return;

            var parent = Path.GetDirectoryName(newPath);
            if (!string.IsNullOrEmpty(parent) && !Directory.Exists(parent))
                Directory.CreateDirectory(parent);

            // overwrite:false — we already checked !File.Exists(newPath), but
            // this makes the intent explicit and avoids clobbering a file
            // created concurrently by the service.
            File.Copy(legacyPath, newPath, overwrite: false);

            Debug.WriteLine($"[Config] Migrated legacy config from %ProgramData% to {newPath}");
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"[Config] Migration from %ProgramData% failed: {ex.Message}");
            // Swallow — GUI must never crash on config migration failure.
        }
    }

    private static bool IsDirectoryWritable(string path)
    {
        try
        {
            var probe = Path.Combine(path, $".write_probe_{Guid.NewGuid():N}.tmp");
            using (var fs = new FileStream(probe, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1, FileOptions.DeleteOnClose))
            {
                // File is deleted on close via DeleteOnClose.
            }
            return true;
        }
        catch
        {
            return false;
        }
    }
}
