using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;

namespace GBFRUltrawideSetup
{
    /// <summary>
    /// Minimal string-resource mechanism (no third-party dependencies).
    /// Contains two dictionaries, en and zh-TW, queried via <see cref="L(string)"/>.
    /// When a key is missing: fall back to en, then fall back to the key itself.
    /// The language choice is saved to installer_lang.txt next to the exe and reused on the next launch.
    /// </summary>
    internal static class Strings
    {
        public const string English = "en";
        public const string TradChinese = "zh-TW";

        private const string LangFileName = "installer_lang.txt";

        private static readonly Dictionary<string, Dictionary<string, string>> _all =
            new Dictionary<string, Dictionary<string, string>>();

        private static string _current;

        /// <summary>Raised when the language changes, so the UI can redraw.</summary>
        public static event EventHandler LanguageChanged;

        static Strings()
        {
            _all[English] = BuildEnglish();
            _all[TradChinese] = BuildTradChinese();
            _current = LoadSavedLanguage();
        }

        /// <summary>Current language code ("en" or "zh-TW").</summary>
        public static string Current
        {
            get { return _current; }
        }

        /// <summary>Switches the language and persists it; raises LanguageChanged only if it actually changed.</summary>
        public static void SetLanguage(string lang)
        {
            if (string.IsNullOrEmpty(lang) || !_all.ContainsKey(lang))
                lang = English;
            if (lang == _current)
                return;
            _current = lang;
            SaveLanguage(lang);
            EventHandler h = LanguageChanged;
            if (h != null) h(null, EventArgs.Empty);
        }

        /// <summary>Looks up a string resource. When missing, falls back to en, then to the key itself.</summary>
        public static string L(string key)
        {
            string value;
            Dictionary<string, string> cur;
            if (_all.TryGetValue(_current, out cur) && cur.TryGetValue(key, out value))
                return value;
            Dictionary<string, string> en;
            if (_all.TryGetValue(English, out en) && en.TryGetValue(key, out value))
                return value;
            return key;
        }

        /// <summary>Formatted variant: applies string.Format to L(key).</summary>
        public static string F(string key, params object[] args)
        {
            return string.Format(L(key), args);
        }

        // ==================================================================
        //  Language persistence
        // ==================================================================

        private static string LangFilePath
        {
            get { return Path.Combine(AppDomain.CurrentDomain.BaseDirectory, LangFileName); }
        }

        private static string LoadSavedLanguage()
        {
            try
            {
                string path = LangFilePath;
                if (File.Exists(path))
                {
                    string saved = File.ReadAllText(path).Trim();
                    if (_all.ContainsKey(saved))
                        return saved;
                }
            }
            catch { }

            // Choose the default from the system UI culture: starts with "zh" -> zh-TW, otherwise en.
            try
            {
                string name = CultureInfo.CurrentUICulture.Name;
                if (name != null && name.StartsWith("zh", StringComparison.OrdinalIgnoreCase))
                    return TradChinese;
            }
            catch { }
            return English;
        }

        private static void SaveLanguage(string lang)
        {
            try { File.WriteAllText(LangFilePath, lang); }
            catch { }
        }

        // ==================================================================
        //  String dictionaries
        // ==================================================================

        private static Dictionary<string, string> BuildEnglish()
        {
            var d = new Dictionary<string, string>(StringComparer.Ordinal);

            // Window / tabs / language
            d["App.Title"] = "GBFR Ultrawide Installer & Configurator";
            d["Tab.Install"] = "Install";
            d["Tab.Config"] = "Settings";
            d["Lang.Label"] = "Language:";
            d["Lang.English"] = "English";
            d["Lang.TradChinese"] = "繁體中文";

            // Update check
            d["Update.Available"] = "⚠ New version {0} available (current: {1}) — click here to download";
            d["Update.CheckFailed"] = "Update check failed: {0}";
            d["Update.OpenFailed"] = "Could not open the download page: {0}";

            // Install tab - game path
            d["Install.PathGroup"] = "Game folder (Granblue Fantasy Relink install directory)";
            d["Install.Browse"] = "Browse…";
            d["Install.Detect"] = "Auto-detect";
            d["Install.PathPrompt"] = "Enter or auto-detect the game install folder.";
            d["Install.PathValid"] = "Found {0}. Path is valid.";
            d["Install.PathInvalid"] = "{0} was not found in this folder.";

            // Install tab - status
            d["Install.StateGroup"] = "Installation status";
            d["Install.LoaderLabel"] = "ASI Loader (winmm.dll):";
            d["Install.AsiLabel"] = "Ultrawide plugin (scripts\\GBFRUltrawide.asi):";
            d["Install.IniLabel"] = "Config file (scripts\\GBFRUltrawide.ini):";
            d["State.NotChecked"] = "Not checked";
            d["State.Installed"] = "Installed";
            d["State.NotInstalled"] = "Not installed";
            d["State.NA"] = "—";

            // Install tab - action buttons
            d["Install.Install"] = "Install";
            d["Install.Uninstall"] = "Uninstall";
            d["Install.OpenDir"] = "Open game folder";
            d["Install.OpenLog"] = "Open log";

            // Install tab - message area
            d["Install.MessagesGroup"] = "Messages";

            // Detection
            d["Detect.Found"] = "Auto-detected game path: {0}";
            d["Detect.NotFound"] = "Could not auto-detect the game path. Please browse to it manually.";
            d["Detect.DialogTitle"] = "Auto-detect";
            d["Detect.DialogBody"] = "Could not locate Granblue Fantasy Relink. Please use \"Browse…\" to select the game folder manually.";

            // Browse
            d["Browse.Description"] = "Select the Granblue Fantasy Relink install folder (which contains {0}).";

            // Install flow
            d["Install.Title"] = "Install";
            d["Install.Uninstall.Title"] = "Uninstall";
            d["Install.PathInvalidBody"] = "Invalid game path: the folder must contain {0}.";
            d["Install.PayloadMissingTitle"] = "Install";
            d["Install.PayloadMissingBody"] = "Install source files (payload) are missing:\n\n{0}\n\nPlease make sure the payload folder next to this tool is complete ({1}).";
            d["Install.LegacyTitle"] = "Legacy files detected";
            d["Install.LegacyBody"] = "Detected legacy GBFRelinkFix / old ASI Loader files:\n\n{0}\n\nBack up and remove them? (Removal is recommended to avoid loading two plugins at once.)";
            d["Install.BackupDone"] = "Backed up {0} file(s) to: {1}";
            d["Install.LegacyRemoved"] = "Removed legacy file: {0}";
            d["Install.DeployedLoader"] = "Deployed winmm.dll (Ultimate ASI Loader) to the game root.";
            d["Install.DeployedAsi"] = "Deployed GBFRUltrawide.asi to scripts\\.";
            d["Install.IniKept"] = "scripts\\GBFRUltrawide.ini already exists; your current settings were kept (not overwritten).";
            d["Install.DeployedIni"] = "Deployed GBFRUltrawide.ini to scripts\\.";
            d["Install.Done"] = "Installation complete.";
            d["Install.DoneMsg"] = "Installation complete!";
            d["Install.DoneWithBackup"] = "Installation complete!\n\nExisting files were backed up to:\n{0}";
            d["Install.FailedPermTitle"] = "Installation failed";
            d["Install.FailedPermBody"] = "Writing to the game folder was denied (insufficient permission).\n\nIf the game is installed under a protected location such as Program Files, close this tool and re-open it via right-click → \"Run as administrator\", then try again.\n\nDetails: {0}";
            d["Install.FailedPermLog"] = "Installation failed (permission denied): {0}";
            d["Install.FailedTitle"] = "Installation failed";
            d["Install.FailedBody"] = "Installation failed:\n{0}";
            d["Install.FailedLog"] = "Installation failed: {0}";

            // Uninstall flow
            d["Uninstall.ConfirmBody"] = "The following files will be removed:\n\n  winmm.dll\n  {0}\n\nAlso delete the config file {1}?\n\n[Yes] Delete the config file too\n[No] Keep the config file\n[Cancel] Do not uninstall";
            d["Uninstall.Deleted"] = "Deleted: {0}";
            d["Uninstall.NothingToRemove"] = "No removable files were found.";
            d["Uninstall.Done"] = "Uninstall complete ({0} file(s) removed).";
            d["Uninstall.BackupsFound"] = "Existing backups detected; you can restore the original files manually:";
            d["Uninstall.BackupHint"] = "(Copy the files inside the backup folder back into the game folder to restore.)";
            d["Uninstall.FailedPermTitle"] = "Uninstall failed";
            d["Uninstall.FailedPermBody"] = "Deleting files was denied (insufficient permission). Please re-run this tool as administrator.\n\nDetails: {0}";
            d["Uninstall.FailedPermLog"] = "Uninstall failed (permission denied): {0}";
            d["Uninstall.FailedTitle"] = "Uninstall failed";
            d["Uninstall.FailedBody"] = "Uninstall failed:\n{0}";
            d["Uninstall.FailedLog"] = "Uninstall failed: {0}";

            // Open directory / log
            d["OpenDir.Failed"] = "Could not open the game folder: {0}";
            d["OpenLog.Title"] = "Open log";
            d["OpenLog.NotFound"] = "Log file not found:\n{0}\n\nLaunch the game once so the plugin can create the log.";
            d["OpenLog.Failed"] = "Could not open the log: {0}";

            // Settings tab - groups
            d["Config.HintNotLoaded"] = "Config file not loaded yet.";
            d["Config.HintNoIni"] = "scripts\\GBFRUltrawide.ini not found. Please select the game path on the Install tab and install first.";
            d["Config.HintLoadFailed"] = "Failed to read the config file: {0}";
            d["Config.HintLoaded"] = "Config file: {0}";
            d["Config.Reloaded"] = "Reloaded settings: {0}";

            d["Config.Group.General"] = "General";
            d["Config.Group.Resolution"] = "Custom resolution";
            d["Config.Group.Fixes"] = "Ultrawide / non-16:9 fixes";
            d["Config.Group.Span"] = "Span HUD";
            d["Config.Group.Camera"] = "Camera";
            d["Config.Group.Graphics"] = "Graphics tweaks";
            d["Config.Group.Experimental"] = "Experimental";

            // Settings tab - buttons
            d["Config.Save"] = "Save settings";
            d["Config.Reload"] = "Reload";
            d["Config.Defaults"] = "Restore defaults";

            // Settings tab - general
            d["Config.Delay"] = "Injection delay (InjectionDelay):";
            d["Config.DelayHint"] = "milliseconds (default 1000)";

            // Settings tab - resolution
            d["Config.ResEnabled"] = "Enable custom resolution";
            d["Config.Width"] = "Width:";
            d["Config.Height"] = "Height:";
            d["Config.ResHint"] = "When width/height is 0, the desktop resolution is used.";

            // Settings tab - fixes
            d["Config.FixHud"] = "Fix HUD (resize HUD to 16:9 and span backgrounds, fades, etc.)";
            d["Config.FixAspect"] = "Fix aspect ratio (correct the image when narrower than 16:9)";
            d["Config.FixNameplates"] = "Fix nameplates (world-anchored nameplate position/scale when wider than 16:9)";
            d["Config.FixFov"] = "Fix FOV (correct the field of view when narrower than 16:9)";

            // Settings tab - Span HUD
            d["Config.SpanEnabled"] = "Enable Span HUD (stretch the gameplay HUD to a given aspect ratio)";
            d["Config.SpanRatio"] = "HUD aspect ratio:";
            d["Config.SpanRatio.Auto"] = "Auto (match screen resolution)";
            d["Config.SpanRatio.169"] = "16:9 (1.7778)";
            d["Config.SpanRatio.219"] = "21:9 (2.3889)";
            d["Config.SpanRatio.329"] = "32:9 (3.5556)";
            d["Config.SpanRatio.Custom"] = "Custom value…";
            d["Config.SpanCustomHint"] = "(the number on the right is used only when \"Custom value\" is selected)";
            d["Config.SpanAllHud"] = "Span all HUD (menus and story scenes are automatically kept at 16:9 by a built-in blocklist; safe to leave on)";
            d["Config.SpanAllBg"] = "Span all background images (main-menu background, etc.; may cause visual issues)";

            // Settings tab - camera
            d["Config.Fov"] = "Gameplay FOV multiplier:";
            d["Config.FovHint"] = "1.0 = original FOV, 1.2 = 20% higher";
            d["Config.Cam"] = "Camera distance multiplier:";
            d["Config.CamHint"] = "1.0 = original distance, 1.2 = 20% further back";

            // Settings tab - graphics
            d["Config.ShadowEnabled"] = "Override shadow quality";
            d["Config.ShadowHint"] = "Game presets: ultra = 2048, high = 1024, standard = 256";
            d["Config.Lod"] = "Level of Detail multiplier:";
            d["Config.LodHint"] = "Higher values reduce object pop-in (0.1–10). Experimental; effect may vary.";
            d["Config.DisableTaa"] = "Disable TAA (temporal anti-aliasing)";

            // Settings tab - experimental
            d["Config.FpsCap"] = "Raise the framerate cap to 240fps (use at your own risk; may cause physics glitches)";

            // Settings tab - save / restore
            d["Config.Saved"] = "Settings saved: {0}";
            d["Config.SavedTitle"] = "Save settings";
            d["Config.SavedBody"] = "Settings saved.";
            d["Config.SaveFailedPermTitle"] = "Save failed";
            d["Config.SaveFailedPermBody"] = "Writing the config file was denied (insufficient permission). Please re-run this tool as administrator.\n\nDetails: {0}";
            d["Config.SaveFailedTitle"] = "Save failed";
            d["Config.SaveFailedBody"] = "Failed to save settings:\n{0}";
            d["Config.DefaultsApplied"] = "Defaults applied (not written to file yet; click \"Save settings\").";

            return d;
        }

        private static Dictionary<string, string> BuildTradChinese()
        {
            var d = new Dictionary<string, string>(StringComparer.Ordinal);

            // Window / tabs / language
            d["App.Title"] = "GBFR Ultrawide 安裝設定工具";
            d["Tab.Install"] = "安裝";
            d["Tab.Config"] = "設定";
            d["Lang.Label"] = "語言：";
            d["Lang.English"] = "English";
            d["Lang.TradChinese"] = "繁體中文";

            // Update check
            d["Update.Available"] = "⚠ 偵測到新版本 {0}（目前 {1}），點此前往下載頁";
            d["Update.CheckFailed"] = "版本檢查失敗：{0}";
            d["Update.OpenFailed"] = "無法開啟下載頁：{0}";

            // Install tab - game path
            d["Install.PathGroup"] = "遊戲路徑（Granblue Fantasy Relink 安裝資料夾）";
            d["Install.Browse"] = "瀏覽…";
            d["Install.Detect"] = "自動偵測";
            d["Install.PathPrompt"] = "請輸入或偵測遊戲安裝資料夾。";
            d["Install.PathValid"] = "已找到 {0}，路徑有效。";
            d["Install.PathInvalid"] = "此資料夾內找不到 {0}。";

            // Install tab - status
            d["Install.StateGroup"] = "安裝狀態";
            d["Install.LoaderLabel"] = "ASI Loader（winmm.dll）：";
            d["Install.AsiLabel"] = "Ultrawide 外掛（scripts\\GBFRUltrawide.asi）：";
            d["Install.IniLabel"] = "設定檔（scripts\\GBFRUltrawide.ini）：";
            d["State.NotChecked"] = "未偵測";
            d["State.Installed"] = "已安裝";
            d["State.NotInstalled"] = "未安裝";
            d["State.NA"] = "—";

            // Install tab - action buttons
            d["Install.Install"] = "安裝";
            d["Install.Uninstall"] = "解除安裝";
            d["Install.OpenDir"] = "開啟遊戲目錄";
            d["Install.OpenLog"] = "開啟 log";

            // Install tab - message area
            d["Install.MessagesGroup"] = "訊息";

            // Detection
            d["Detect.Found"] = "自動偵測到遊戲路徑：{0}";
            d["Detect.NotFound"] = "無法自動偵測遊戲路徑，請手動瀏覽選擇。";
            d["Detect.DialogTitle"] = "自動偵測";
            d["Detect.DialogBody"] = "找不到 Granblue Fantasy Relink 的安裝位置，請以「瀏覽…」手動選擇遊戲資料夾。";

            // Browse
            d["Browse.Description"] = "請選擇 Granblue Fantasy Relink 的安裝資料夾（內含 {0}）。";

            // Install flow
            d["Install.Title"] = "安裝";
            d["Install.Uninstall.Title"] = "解除安裝";
            d["Install.PathInvalidBody"] = "遊戲路徑無效：資料夾內必須有 {0}。";
            d["Install.PayloadMissingTitle"] = "安裝";
            d["Install.PayloadMissingBody"] = "找不到安裝來源檔案（payload）：\n\n{0}\n\n請確認本工具旁的 payload 資料夾完整（{1}）。";
            d["Install.LegacyTitle"] = "偵測到舊版檔案";
            d["Install.LegacyBody"] = "偵測到舊版 GBFRelinkFix／舊 ASI Loader 檔案：\n\n{0}\n\n是否備份後移除？（建議移除，避免與本插件重複載入）";
            d["Install.BackupDone"] = "已備份 {0} 個檔案到：{1}";
            d["Install.LegacyRemoved"] = "已移除舊版檔案：{0}";
            d["Install.DeployedLoader"] = "已部署 winmm.dll（Ultimate ASI Loader）→ 遊戲根目錄";
            d["Install.DeployedAsi"] = "已部署 GBFRUltrawide.asi → scripts\\";
            d["Install.IniKept"] = "scripts\\GBFRUltrawide.ini 已存在，保留您現有的設定（未覆蓋）。";
            d["Install.DeployedIni"] = "已部署 GBFRUltrawide.ini → scripts\\";
            d["Install.Done"] = "安裝完成。";
            d["Install.DoneMsg"] = "安裝完成！";
            d["Install.DoneWithBackup"] = "安裝完成！\n\n原有檔案已備份到：\n{0}";
            d["Install.FailedPermTitle"] = "安裝失敗";
            d["Install.FailedPermBody"] = "寫入遊戲目錄被拒（權限不足）。\n\n若遊戲安裝在 Program Files 等受保護位置，請關閉本工具後，以滑鼠右鍵→「以系統管理員身分執行」重新開啟再試一次。\n\n詳細訊息：{0}";
            d["Install.FailedPermLog"] = "安裝失敗（權限不足）：{0}";
            d["Install.FailedTitle"] = "安裝失敗";
            d["Install.FailedBody"] = "安裝失敗：\n{0}";
            d["Install.FailedLog"] = "安裝失敗：{0}";

            // Uninstall flow
            d["Uninstall.ConfirmBody"] = "將移除下列檔案：\n\n  winmm.dll\n  {0}\n\n是否連設定檔 {1} 一併刪除？\n\n【是】連設定檔一起刪除\n【否】保留設定檔\n【取消】不進行解除安裝";
            d["Uninstall.Deleted"] = "已刪除：{0}";
            d["Uninstall.NothingToRemove"] = "沒有找到可移除的檔案。";
            d["Uninstall.Done"] = "解除安裝完成（共移除 {0} 個檔案）。";
            d["Uninstall.BackupsFound"] = "偵測到既有備份，可手動還原原始檔案：";
            d["Uninstall.BackupHint"] = "（將備份資料夾內的檔案複製回遊戲目錄即可還原。）";
            d["Uninstall.FailedPermTitle"] = "解除安裝失敗";
            d["Uninstall.FailedPermBody"] = "刪除檔案被拒（權限不足）。請以系統管理員身分重新執行本工具。\n\n詳細訊息：{0}";
            d["Uninstall.FailedPermLog"] = "解除安裝失敗（權限不足）：{0}";
            d["Uninstall.FailedTitle"] = "解除安裝失敗";
            d["Uninstall.FailedBody"] = "解除安裝失敗：\n{0}";
            d["Uninstall.FailedLog"] = "解除安裝失敗：{0}";

            // Open directory / log
            d["OpenDir.Failed"] = "無法開啟遊戲目錄：{0}";
            d["OpenLog.Title"] = "開啟 log";
            d["OpenLog.NotFound"] = "找不到 log 檔：\n{0}\n\n請先啟動一次遊戲，插件載入後才會產生 log。";
            d["OpenLog.Failed"] = "無法開啟 log：{0}";

            // Settings tab - groups
            d["Config.HintNotLoaded"] = "尚未載入設定檔。";
            d["Config.HintNoIni"] = "找不到 scripts\\GBFRUltrawide.ini，請先在「安裝」頁選擇遊戲路徑並完成安裝。";
            d["Config.HintLoadFailed"] = "讀取設定檔失敗：{0}";
            d["Config.HintLoaded"] = "設定檔：{0}";
            d["Config.Reloaded"] = "已重新載入設定：{0}";

            d["Config.Group.General"] = "一般";
            d["Config.Group.Resolution"] = "自訂解析度";
            d["Config.Group.Fixes"] = "超寬螢幕／非 16:9 修正";
            d["Config.Group.Span"] = "HUD 延伸（Span HUD）";
            d["Config.Group.Camera"] = "視角";
            d["Config.Group.Graphics"] = "畫質調整";
            d["Config.Group.Experimental"] = "實驗性";

            // Settings tab - buttons
            d["Config.Save"] = "儲存設定";
            d["Config.Reload"] = "重新載入";
            d["Config.Defaults"] = "還原預設值";

            // Settings tab - general
            d["Config.Delay"] = "注入延遲 InjectionDelay：";
            d["Config.DelayHint"] = "毫秒（預設 1000）";

            // Settings tab - resolution
            d["Config.ResEnabled"] = "啟用自訂解析度";
            d["Config.Width"] = "寬：";
            d["Config.Height"] = "高：";
            d["Config.ResHint"] = "寬／高設為 0 時，會直接使用桌面解析度。";

            // Settings tab - fixes
            d["Config.FixHud"] = "修正 HUD（將 HUD 調整為 16:9 並延伸背景漸暗等效果）";
            d["Config.FixAspect"] = "修正比例（窄於 16:9 時修正畫面比例）";
            d["Config.FixNameplates"] = "修正名牌（寬於 16:9 時修正世界定位名牌的位置與縮放）";
            d["Config.FixFov"] = "修正 FOV（窄於 16:9 時修正視野）";

            // Settings tab - Span HUD
            d["Config.SpanEnabled"] = "啟用 HUD 延伸（將戰鬥 HUD 延伸至指定比例）";
            d["Config.SpanRatio"] = "HUD 比例：";
            d["Config.SpanRatio.Auto"] = "自動（依螢幕解析度）";
            d["Config.SpanRatio.169"] = "16:9（1.7778）";
            d["Config.SpanRatio.219"] = "21:9（2.3889）";
            d["Config.SpanRatio.329"] = "32:9（3.5556）";
            d["Config.SpanRatio.Custom"] = "自訂數值…";
            d["Config.SpanCustomHint"] = "（選「自訂數值」時才使用右側數字）";
            d["Config.SpanAllHud"] = "延伸所有 HUD（內建排除清單會讓選單、劇情畫面自動維持 16:9，建議保持開啟）";
            d["Config.SpanAllBg"] = "延伸所有背景圖（主選單背景等，可能出現視覺問題）";

            // Settings tab - camera
            d["Config.Fov"] = "遊玩 FOV 倍率：";
            d["Config.FovHint"] = "1.0 = 原始視野，1.2 = 提高 20%";
            d["Config.Cam"] = "相機距離倍率：";
            d["Config.CamHint"] = "1.0 = 原始距離，1.2 = 拉遠 20%";

            // Settings tab - graphics
            d["Config.ShadowEnabled"] = "覆寫陰影品質";
            d["Config.ShadowHint"] = "遊戲內建：ultra=2048、high=1024、standard=256";
            d["Config.Lod"] = "LOD 倍率：";
            d["Config.LodHint"] = "調高可減少物件突然浮現（0.1–10）。實驗性功能，效果可能因場景而異。";
            d["Config.DisableTaa"] = "關閉 TAA（時間性反鋸齒）";

            // Settings tab - experimental
            d["Config.FpsCap"] = "將幀率上限提高到 240fps（風險自負，可能出現物理異常）";

            // Settings tab - save / restore
            d["Config.Saved"] = "設定已儲存：{0}";
            d["Config.SavedTitle"] = "儲存設定";
            d["Config.SavedBody"] = "設定已儲存。";
            d["Config.SaveFailedPermTitle"] = "儲存失敗";
            d["Config.SaveFailedPermBody"] = "寫入設定檔被拒（權限不足）。請以系統管理員身分重新執行本工具。\n\n詳細訊息：{0}";
            d["Config.SaveFailedTitle"] = "儲存失敗";
            d["Config.SaveFailedBody"] = "儲存設定失敗：\n{0}";
            d["Config.DefaultsApplied"] = "已套用預設值（尚未寫入檔案，請按「儲存設定」）。";

            return d;
        }
    }
}
