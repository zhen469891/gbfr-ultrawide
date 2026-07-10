using System;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace GBFRUltrawideSetup
{
    /// <summary>
    /// Detects the version of the GBFRUltrawide plugin CURRENTLY installed in the game
    /// directory (as opposed to UpdateChecker, which is about the newest published release).
    ///
    /// Detection order (first hit wins):
    ///   1) The installed scripts\GBFRUltrawide.asi bytes: the greppable
    ///      "GBFRUW-VERSION=&lt;version&gt;" marker embedded in the binary. This is the file
    ///      actually sitting in the folder, so it is the authoritative "what is installed"
    ///      even if the game has not been launched since (the log would still show the
    ///      previous build). Only builds that carry the marker are detectable this way.
    ///   2) The plugin's own log, scripts\GBFRUltrawide.log: the most recent
    ///      "GBFRUltrawide vX.Y.Z loaded." line. Fallback for older .asi files that predate
    ///      the marker, as long as the game has been launched once with the plugin.
    ///
    /// If neither yields a version, the caller treats the install as "version unknown"
    /// (possibly outdated) rather than hard-erroring.
    /// </summary>
    internal static class InstalledVersion
    {
        private const string AsiRelPath = "scripts\\GBFRUltrawide.asi";
        private const string LogRelPath = "scripts\\GBFRUltrawide.log";

        /// <summary>Marker literal embedded in the plugin binary (see src\dllmain.cpp).</summary>
        private const string VersionTag = "GBFRUW-VERSION=";

        // "GBFRUltrawide v0.2.1 loaded." (a trailing " (dev)" may follow the period).
        // Capture the dotted version plus any -/+ suffix (e.g. 0.3.0-rc1).
        private static readonly Regex LogLineRegex = new Regex(
            @"GBFRUltrawide\s+v([0-9][0-9A-Za-z.+\-]*)\s+loaded\.",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

        // Same version shape as the log regex, used to bound the marker read.
        private static readonly Regex TagRegex = new Regex(
            @"^([0-9][0-9A-Za-z.+\-]*)",
            RegexOptions.CultureInvariant);

        /// <summary>
        /// Returns the detected installed version (no leading 'v'), or null if it could
        /// not be determined. gameDir is the game root; the plugin files live under scripts\.
        /// </summary>
        public static string Detect(string gameDir)
        {
            if (string.IsNullOrEmpty(gameDir))
                return null;

            string fromAsi = FromAsi(Path.Combine(gameDir, AsiRelPath));
            if (!string.IsNullOrEmpty(fromAsi))
                return fromAsi;

            string fromLog = FromLog(Path.Combine(gameDir, LogRelPath));
            if (!string.IsNullOrEmpty(fromLog))
                return fromLog;

            return null;
        }

        /// <summary>Most recent "GBFRUltrawide vX loaded." version in the log, or null.</summary>
        private static string FromLog(string logPath)
        {
            try
            {
                if (!File.Exists(logPath))
                    return null;

                // The log is truncated on every launch, so it holds a single session; the
                // last matching line is the current build. Read shared so an open handle
                // (game running) does not block us.
                string last = null;
                using (var fs = new FileStream(logPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                using (var reader = new StreamReader(fs))
                {
                    string line;
                    while ((line = reader.ReadLine()) != null)
                    {
                        Match m = LogLineRegex.Match(line);
                        if (m.Success)
                            last = m.Groups[1].Value;
                    }
                }
                return last;
            }
            catch
            {
                return null;
            }
        }

        /// <summary>Version from the "GBFRUW-VERSION=" marker in the installed .asi, or null.</summary>
        private static string FromAsi(string asiPath)
        {
            try
            {
                if (!File.Exists(asiPath))
                    return null;

                byte[] bytes = File.ReadAllBytes(asiPath);
                // The marker is plain ASCII; decode Latin1 so every byte maps 1:1 and the
                // marker's byte offset equals its char index.
                string text = Encoding.GetEncoding("ISO-8859-1").GetString(bytes);

                int idx = text.IndexOf(VersionTag, StringComparison.Ordinal);
                if (idx < 0)
                    return null;

                int start = idx + VersionTag.Length;
                // The version literal is NUL-terminated in the binary; stop at the first
                // NUL (or any non-version byte) and validate the captured token.
                int end = text.IndexOf('\0', start);
                if (end < 0) end = Math.Min(text.Length, start + 64);
                string candidate = text.Substring(start, end - start);

                Match m = TagRegex.Match(candidate);
                return m.Success ? m.Groups[1].Value : null;
            }
            catch
            {
                return null;
            }
        }
    }
}
