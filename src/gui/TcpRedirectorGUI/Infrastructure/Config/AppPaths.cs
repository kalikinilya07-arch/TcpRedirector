using System;
using System.Diagnostics;
using System.IO;

namespace TcpRedirectorGUI.Infrastructure.Config;

/// <summary>
/// Resolves on-disk paths for the GUI. The GUI is installed under a
/// nested <c>gui\</c> subfolder next to the service executable so
/// <c>config.json</c> lives one directory above <see cref="AppContext.BaseDirectory"/>.
///
/// Resolution order for <see cref="GetConfigPath"/>:
///   1. <c>&lt;BaseDir&gt;\config.json</c>       (dev / portable — GUI + service co-located)
///   2. <c>&lt;BaseDir&gt;\..\config.json</c>    (installed layout — GUI in nested gui\ folder)
///   3. Default target: <c>&lt;BaseDir&gt;\..\config.json</c> if the parent directory
///      exists and is writable; otherwise <c>&lt;BaseDir&gt;\config.json</c>.
///
/// Never falls back to <c>%ProgramData%</c>, <c>%AppData%</c>, or
/// <see cref="Environment.CurrentDirectory"/> for the primary path.
/// </summary>
internal static class AppPaths
{
    private const string ConfigFileName = "config.json";
    private const string LegacyFolderName = "TcpRedirector";

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
    /// Resolves the <c>config.json</c> path per the class-level rule.
    /// </summary>
    public static string GetConfigPath()
    {
        var baseDir = GetBaseDirectory();
        var installDir = GetInstallDirectory();

        var atBase = Path.Combine(baseDir, ConfigFileName);
        if (File.Exists(atBase))
            return atBase;

        var atParent = Path.Combine(installDir, ConfigFileName);
        if (File.Exists(atParent))
            return atParent;

        // Neither exists — pick the default target for a fresh save.
        // Prefer the install (parent) directory, but only if it exists and
        // appears writable; otherwise fall back to the base directory.
        if (!string.Equals(installDir, baseDir, StringComparison.OrdinalIgnoreCase)
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
