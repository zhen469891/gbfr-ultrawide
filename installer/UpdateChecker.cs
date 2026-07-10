using System;
using System.IO;
using System.Net;
using System.Text.RegularExpressions;
using System.Threading;

namespace GBFRUltrawideSetup
{
    /// <summary>
    /// Background "is a newer release available?" check against GitHub.
    /// Identity is the release tag (e.g. "v0.1.0"). The /releases LIST endpoint is used
    /// (not /releases/latest) because this project may mark 0.x tags as pre-releases,
    /// which /releases/latest skips. Only two JSON fields are needed (tag_name and the
    /// release html_url), extracted with regexes to avoid a JSON-library dependency.
    /// </summary>
    internal static class UpdateChecker
    {
        /// <summary>
        /// Version of this bundle, stamped at build time by build_installer.ps1 into the
        /// generated VersionInfo.g.cs (from -Version, i.e. the release tag minus the
        /// leading 'v'). Local/dev builds get "0.0.0-dev", which CompareVersions ranks
        /// below every real release, so the update notice simply points at the newest
        /// release - no special-casing needed.
        /// </summary>
        public const string CurrentVersion = VersionInfo.Version;

        private const string ReleasesApiUrl =
            "https://api.github.com/repos/zhen469891/gbfr-ultrawide/releases?per_page=10";

        /// <summary>Fallback link when a release-specific page is unavailable.</summary>
        public const string ReleasesPageUrl =
            "https://github.com/zhen469891/gbfr-ultrawide/releases";

        internal sealed class Result
        {
            public bool UpdateAvailable;
            public string LatestVersion; // normalized ("0.2.0", no leading 'v')
            public string HtmlUrl;       // release page of the newest release
        }

        /// <summary>
        /// Runs the check on a thread-pool thread. onDone is invoked on that worker
        /// thread with (result, null) on success or (null, error) on failure - the
        /// caller is responsible for marshalling back onto the UI thread.
        /// </summary>
        public static void CheckAsync(Action<Result, Exception> onDone)
        {
            ThreadPool.QueueUserWorkItem(delegate
            {
                try
                {
                    Result r = Check();
                    onDone(r, null);
                }
                catch (Exception ex)
                {
                    onDone(null, ex);
                }
            });
        }

        private static Result Check()
        {
            // GitHub requires TLS 1.2+ and a User-Agent header.
            try { ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12; }
            catch (NotSupportedException) { }

            HttpWebRequest req = (HttpWebRequest)WebRequest.Create(ReleasesApiUrl);
            req.Method = "GET";
            req.UserAgent = "GBFRUltrawideSetup/" + CurrentVersion;
            req.Accept = "application/vnd.github+json";
            req.Timeout = 10000;
            req.ReadWriteTimeout = 10000;

            string json;
            using (WebResponse resp = req.GetResponse())
            using (StreamReader reader = new StreamReader(resp.GetResponseStream()))
                json = reader.ReadToEnd();

            // Pick the highest-versioned release; do not rely on the array order.
            string bestTag = null;
            foreach (Match m in Regex.Matches(json, "\"tag_name\"\\s*:\\s*\"([^\"]+)\""))
            {
                string tag = m.Groups[1].Value;
                if (bestTag == null || CompareVersions(tag, bestTag) > 0)
                    bestTag = tag;
            }

            Result result = new Result();
            if (bestTag == null)
            {
                // No releases published yet -> nothing newer.
                result.UpdateAvailable = false;
                return result;
            }

            result.LatestVersion = Normalize(bestTag);
            result.UpdateAvailable = CompareVersions(bestTag, CurrentVersion) > 0;
            result.HtmlUrl = FindReleaseUrl(json, bestTag);
            return result;
        }

        /// <summary>Finds the release page URL for the given tag in the raw /releases JSON.</summary>
        private static string FindReleaseUrl(string json, string tag)
        {
            foreach (Match m in Regex.Matches(json, "\"html_url\"\\s*:\\s*\"([^\"]+/releases/tag/[^\"]+)\""))
            {
                string url = m.Groups[1].Value;
                if (url.EndsWith("/releases/tag/" + tag, StringComparison.OrdinalIgnoreCase) ||
                    url.EndsWith("/releases/tag/" + Uri.EscapeDataString(tag), StringComparison.OrdinalIgnoreCase))
                    return url;
            }
            return ReleasesPageUrl;
        }

        /// <summary>Strips a leading 'v'/'V' and whitespace from a tag.</summary>
        private static string Normalize(string tag)
        {
            if (string.IsNullOrEmpty(tag)) return "";
            return tag.Trim().TrimStart('v', 'V');
        }

        /// <summary>
        /// Semver-ish comparison of two tags/versions: dotted numeric parts compared
        /// numerically; when they are equal, a version WITHOUT a pre-release suffix ranks
        /// higher ("0.2.0" &gt; "0.2.0-rc1"); two suffixes fall back to ordinal comparison.
        /// </summary>
        public static int CompareVersions(string a, string b)
        {
            string na = Normalize(a), nb = Normalize(b);

            string coreA = na, sufA = "";
            int cutA = na.IndexOfAny(new char[] { '-', '+' });
            if (cutA >= 0) { coreA = na.Substring(0, cutA); sufA = na.Substring(cutA + 1); }

            string coreB = nb, sufB = "";
            int cutB = nb.IndexOfAny(new char[] { '-', '+' });
            if (cutB >= 0) { coreB = nb.Substring(0, cutB); sufB = nb.Substring(cutB + 1); }

            string[] pa = coreA.Split('.');
            string[] pb = coreB.Split('.');
            int len = Math.Max(pa.Length, pb.Length);
            for (int i = 0; i < len; i++)
            {
                int va = 0, vb = 0;
                if (i < pa.Length) int.TryParse(pa[i], out va);
                if (i < pb.Length) int.TryParse(pb[i], out vb);
                if (va != vb) return va.CompareTo(vb);
            }

            bool hasSufA = sufA.Length > 0, hasSufB = sufB.Length > 0;
            if (hasSufA != hasSufB) return hasSufA ? -1 : 1; // release > pre-release
            return string.CompareOrdinal(sufA, sufB);
        }
    }
}
