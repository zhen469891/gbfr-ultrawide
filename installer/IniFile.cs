using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;

namespace GBFRUltrawideSetup
{
    /// <summary>
    /// Simple line-based INI reader/writer.
    /// When updating an existing key, only that line is changed; all other lines
    /// (comments, blank lines, unknown keys) are preserved verbatim.
    /// </summary>
    internal sealed class IniFile
    {
        private List<string> _lines = new List<string>();

        public string FilePath { get; private set; }

        public IniFile(string path)
        {
            FilePath = path;
            Reload();
        }

        public void Reload()
        {
            if (File.Exists(FilePath))
                _lines = new List<string>(File.ReadAllLines(FilePath));
            else
                _lines = new List<string>();
        }

        public void Save()
        {
            // File.WriteAllLines defaults to UTF-8 (no BOM), which is compatible with the original ASCII content.
            File.WriteAllLines(FilePath, _lines.ToArray());
        }

        public bool HasSection(string section)
        {
            return FindSection(section) >= 0;
        }

        public string Get(string section, string key, string defaultValue)
        {
            int keyIndex, eqIndex;
            if (FindKey(section, key, out keyIndex, out eqIndex))
            {
                string line = _lines[keyIndex];
                string value = line.Substring(eqIndex + 1).Trim();
                return value;
            }
            return defaultValue;
        }

        public void Set(string section, string key, string value)
        {
            int keyIndex, eqIndex;
            if (FindKey(section, key, out keyIndex, out eqIndex))
            {
                // Keep the original key formatting; only replace the value.
                string line = _lines[keyIndex];
                string keyPart = line.Substring(0, eqIndex).TrimEnd();
                _lines[keyIndex] = keyPart + " = " + value;
                return;
            }

            int sectionIndex = FindSection(section);
            if (sectionIndex < 0)
            {
                if (_lines.Count > 0 && _lines[_lines.Count - 1].Trim().Length != 0)
                    _lines.Add(string.Empty);
                _lines.Add("[" + section + "]");
                _lines.Add(key + " = " + value);
                return;
            }

            // Section exists but the key does not: insert after the last non-blank line within the section.
            int insertAt = sectionIndex + 1;
            for (int i = sectionIndex + 1; i < _lines.Count; i++)
            {
                string name;
                if (IsSectionLine(_lines[i], out name))
                    break;
                if (_lines[i].Trim().Length != 0)
                    insertAt = i + 1;
            }
            _lines.Insert(insertAt, key + " = " + value);
        }

        // ---------- Typed accessors ----------

        public bool GetBool(string section, string key, bool defaultValue)
        {
            string v = Get(section, key, null);
            if (v == null) return defaultValue;
            v = v.Trim().ToLowerInvariant();
            if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
            if (v == "false" || v == "0" || v == "no" || v == "off") return false;
            return defaultValue;
        }

        public int GetInt(string section, string key, int defaultValue)
        {
            string v = Get(section, key, null);
            int result;
            if (v != null && int.TryParse(v.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out result))
                return result;
            return defaultValue;
        }

        public double GetDouble(string section, string key, double defaultValue)
        {
            string v = Get(section, key, null);
            double result;
            if (v != null && double.TryParse(v.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out result))
                return result;
            return defaultValue;
        }

        public void SetBool(string section, string key, bool value)
        {
            Set(section, key, value ? "true" : "false");
        }

        public void SetInt(string section, string key, int value)
        {
            Set(section, key, value.ToString(CultureInfo.InvariantCulture));
        }

        public void SetDouble(string section, string key, double value)
        {
            Set(section, key, value.ToString("0.####", CultureInfo.InvariantCulture));
        }

        // ---------- Internals ----------

        private static bool IsSectionLine(string line, out string name)
        {
            name = null;
            string t = line.Trim();
            if (t.Length >= 2 && t[0] == '[')
            {
                int close = t.IndexOf(']');
                if (close > 0)
                {
                    name = t.Substring(1, close - 1).Trim();
                    return true;
                }
            }
            return false;
        }

        private static bool IsCommentOrBlank(string line)
        {
            string t = line.Trim();
            return t.Length == 0 || t[0] == ';' || t[0] == '#';
        }

        private int FindSection(string section)
        {
            for (int i = 0; i < _lines.Count; i++)
            {
                string name;
                if (IsSectionLine(_lines[i], out name) &&
                    string.Equals(name, section, StringComparison.OrdinalIgnoreCase))
                    return i;
            }
            return -1;
        }

        /// <summary>Locates the line of the given key within the section; eqIndex is the position of '=' on that line.</summary>
        private bool FindKey(string section, string key, out int keyIndex, out int eqIndex)
        {
            keyIndex = -1;
            eqIndex = -1;
            int sectionIndex = FindSection(section);
            if (sectionIndex < 0) return false;

            for (int i = sectionIndex + 1; i < _lines.Count; i++)
            {
                string line = _lines[i];
                string name;
                if (IsSectionLine(line, out name))
                    break;
                if (IsCommentOrBlank(line))
                    continue;
                int eq = line.IndexOf('=');
                if (eq <= 0)
                    continue;
                string k = line.Substring(0, eq).Trim();
                if (string.Equals(k, key, StringComparison.OrdinalIgnoreCase))
                {
                    keyIndex = i;
                    eqIndex = eq;
                    return true;
                }
            }
            return false;
        }
    }
}
