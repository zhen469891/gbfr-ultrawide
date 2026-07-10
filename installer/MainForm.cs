using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Text.RegularExpressions;
using System.Windows.Forms;
using Microsoft.Win32;

namespace GBFRUltrawideSetup
{
    public sealed class MainForm : Form
    {
        private const string GameExeName = "granblue_fantasy_relink.exe";
        private const string SteamAppId = "881020";
        private const string GameFolderName = "Granblue Fantasy Relink";
        private const string BackupRootName = "GBFRUltrawide_backup";
        private const string AsiRelPath = "scripts\\GBFRUltrawide.asi";
        private const string IniRelPath = "scripts\\GBFRUltrawide.ini";
        private const string LogRelPath = "scripts\\GBFRUltrawide.log";

        // ---------- Language bar ----------
        private ComboBox _cboLang;
        private Label _lblLangCaption;
        private LinkLabel _lnkUpdate;
        private UpdateChecker.Result _updateInfo;

        // ---------- Tabs ----------
        private TabControl _tabs;
        private TabPage _tabInstall, _tabConfig;

        // ---------- Install tab controls ----------
        private TextBox _txtGamePath;
        private Label _lblPathState;
        private Label _lblLoaderState, _lblAsiState, _lblIniState, _lblVersionState;
        private Button _btnBrowse, _btnDetect;
        private Button _btnInstall, _btnUninstall, _btnOpenDir, _btnOpenLog;
        private TextBox _txtLog;
        // Static install-tab text that must be redrawn when the language changes
        private GroupBox _grpPath, _grpState, _grpMessages;
        private Label _lblLoaderCaption, _lblAsiCaption, _lblIniCaption, _lblVersionCaption;

        // ---------- Settings tab controls ----------
        private Label _lblConfigHint;
        private Panel _configScroll;
        private Button _btnSaveCfg, _btnReloadCfg, _btnDefaultsCfg;
        private NumericUpDown _numDelay, _numWidth, _numHeight, _numSpanCustom, _numFov, _numCam, _numLod;
        private CheckBox _chkResEnabled, _chkFixHud, _chkFixAspect, _chkFixNameplates, _chkFixFov;
        private CheckBox _chkSpanEnabled, _chkSpanAllHud, _chkSpanAllBg;
        private ComboBox _cboSpanRatio, _cboShadow;
        private CheckBox _chkShadowEnabled, _chkDisableTaa, _chkFpsCap;

        // Static settings-tab text (label/hint/group) that must be redrawn when the language changes
        private readonly List<KeyValuePair<Control, string>> _localizedControls =
            new List<KeyValuePair<Control, string>>();

        private IniFile _ini;

        public MainForm()
        {
            Font = new Font("Microsoft JhengHei UI", 9F);
            AutoScaleMode = AutoScaleMode.Font;
            StartPosition = FormStartPosition.CenterScreen;
            MinimumSize = new Size(720, 660);
            ClientSize = new Size(760, 680);

            // --- Language bar (docked to top) ---
            var langBar = new FlowLayoutPanel();
            langBar.Dock = DockStyle.Top;
            langBar.FlowDirection = FlowDirection.RightToLeft;
            langBar.AutoSize = true;
            langBar.Padding = new Padding(6, 4, 6, 2);

            _cboLang = new ComboBox();
            _cboLang.DropDownStyle = ComboBoxStyle.DropDownList;
            _cboLang.Width = 130;
            _cboLang.Items.Add(new LangItem(Strings.English, "Lang.English"));
            _cboLang.Items.Add(new LangItem(Strings.TradChinese, "Lang.TradChinese"));
            SelectLangItem(Strings.Current);
            _cboLang.SelectedIndexChanged += OnLangChanged;

            _lblLangCaption = new Label();
            _lblLangCaption.AutoSize = true;
            _lblLangCaption.Margin = new Padding(3, 7, 3, 3);

            // Update notice: hidden until the background check finds a newer release.
            // The bar flows right-to-left, so adding it after the language caption puts
            // it at the far left of the same row, styled as a warning.
            _lnkUpdate = new LinkLabel();
            _lnkUpdate.AutoSize = true;
            _lnkUpdate.Visible = false;
            _lnkUpdate.Margin = new Padding(3, 7, 12, 3);
            _lnkUpdate.Font = new Font(Font, FontStyle.Bold);
            _lnkUpdate.LinkColor = Color.Firebrick;
            _lnkUpdate.ActiveLinkColor = Color.Red;
            _lnkUpdate.VisitedLinkColor = Color.Firebrick;
            _lnkUpdate.LinkClicked += OnUpdateLinkClicked;

            langBar.Controls.Add(_cboLang);
            langBar.Controls.Add(_lblLangCaption);
            langBar.Controls.Add(_lnkUpdate);

            // --- Tabs ---
            _tabs = new TabControl();
            _tabs.Dock = DockStyle.Fill;

            _tabInstall = new TabPage();
            _tabInstall.Controls.Add(BuildInstallTab());

            _tabConfig = new TabPage();
            _tabConfig.Controls.Add(BuildConfigTab());

            _tabs.TabPages.Add(_tabInstall);
            _tabs.TabPages.Add(_tabConfig);
            _tabs.SelectedIndexChanged += OnTabChanged;

            Controls.Add(_tabs);
            Controls.Add(langBar);

            Strings.LanguageChanged += delegate { ApplyLanguage(); };
            ApplyLanguage();

            Load += OnFormLoad;
        }

        // ==================================================================
        //  UI construction
        // ==================================================================

        private Control BuildInstallTab()
        {
            var root = new TableLayoutPanel();
            root.Dock = DockStyle.Fill;
            root.ColumnCount = 1;
            root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100F));
            root.Padding = new Padding(8);
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100F));

            // --- Game path ---
            var pathGrid = MakeGrid(3);
            pathGrid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100F));
            pathGrid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            pathGrid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));

            _txtGamePath = new TextBox();
            _txtGamePath.Anchor = AnchorStyles.Left | AnchorStyles.Right;
            _txtGamePath.TextChanged += delegate { RefreshStatus(); };

            _btnBrowse = MakeButton(OnBrowse);
            _btnDetect = MakeButton(delegate(object s, EventArgs e) { AutoDetect(true); });

            pathGrid.Controls.Add(_txtGamePath, 0, 0);
            pathGrid.Controls.Add(_btnBrowse, 1, 0);
            pathGrid.Controls.Add(_btnDetect, 2, 0);

            _lblPathState = MakeHint(" ");
            pathGrid.Controls.Add(_lblPathState, 0, 1);
            pathGrid.SetColumnSpan(_lblPathState, 3);

            _grpPath = MakeGroup("Install.PathGroup", pathGrid);
            root.Controls.Add(_grpPath, 0, 0);

            // --- Installation status ---
            var stateGrid = MakeGrid(2);
            stateGrid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            stateGrid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));

            _lblLoaderState = MakeStateLabel();
            _lblAsiState = MakeStateLabel();
            _lblIniState = MakeStateLabel();
            _lblVersionState = MakeStateLabel();
            // The outdated warning can be long; let it wrap instead of stretching the form.
            _lblVersionState.MaximumSize = new Size(560, 0);

            _lblLoaderCaption = MakeLabel("Install.LoaderLabel");
            _lblAsiCaption = MakeLabel("Install.AsiLabel");
            _lblIniCaption = MakeLabel("Install.IniLabel");
            _lblVersionCaption = MakeLabel("Install.VersionLabel");

            stateGrid.Controls.Add(_lblLoaderCaption, 0, 0);
            stateGrid.Controls.Add(_lblLoaderState, 1, 0);
            stateGrid.Controls.Add(_lblAsiCaption, 0, 1);
            stateGrid.Controls.Add(_lblAsiState, 1, 1);
            stateGrid.Controls.Add(_lblIniCaption, 0, 2);
            stateGrid.Controls.Add(_lblIniState, 1, 2);
            stateGrid.Controls.Add(_lblVersionCaption, 0, 3);
            stateGrid.Controls.Add(_lblVersionState, 1, 3);

            _grpState = MakeGroup("Install.StateGroup", stateGrid);
            root.Controls.Add(_grpState, 0, 1);

            // --- Action buttons ---
            var buttons = new FlowLayoutPanel();
            buttons.AutoSize = true;
            buttons.AutoSizeMode = AutoSizeMode.GrowAndShrink;
            buttons.Anchor = AnchorStyles.Left | AnchorStyles.Top;
            buttons.Margin = new Padding(0, 6, 0, 6);

            _btnInstall = MakeButton(delegate(object s, EventArgs e) { DoInstall(); });
            _btnUninstall = MakeButton(delegate(object s, EventArgs e) { DoUninstall(); });
            _btnOpenDir = MakeButton(delegate(object s, EventArgs e) { OpenGameDir(); });
            _btnOpenLog = MakeButton(delegate(object s, EventArgs e) { OpenLogFile(); });

            buttons.Controls.Add(_btnInstall);
            buttons.Controls.Add(_btnUninstall);
            buttons.Controls.Add(_btnOpenDir);
            buttons.Controls.Add(_btnOpenLog);
            root.Controls.Add(buttons, 0, 2);

            // --- Messages ---
            _grpMessages = new GroupBox();
            _grpMessages.Dock = DockStyle.Fill;
            _grpMessages.Padding = new Padding(8);

            _txtLog = new TextBox();
            _txtLog.Multiline = true;
            _txtLog.ReadOnly = true;
            _txtLog.ScrollBars = ScrollBars.Vertical;
            _txtLog.Dock = DockStyle.Fill;
            _txtLog.BackColor = SystemColors.Window;
            _grpMessages.Controls.Add(_txtLog);

            root.Controls.Add(_grpMessages, 0, 3);
            return root;
        }

        private Control BuildConfigTab()
        {
            var root = new TableLayoutPanel();
            root.Dock = DockStyle.Fill;
            root.ColumnCount = 1;
            root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100F));
            root.Padding = new Padding(8);
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100F));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));

            _lblConfigHint = new Label();
            _lblConfigHint.AutoSize = true;
            _lblConfigHint.ForeColor = Color.Firebrick;
            _lblConfigHint.Margin = new Padding(3, 3, 3, 6);
            root.Controls.Add(_lblConfigHint, 0, 0);

            _configScroll = new Panel();
            _configScroll.Dock = DockStyle.Fill;
            _configScroll.AutoScroll = true;

            var content = new TableLayoutPanel();
            content.ColumnCount = 1;
            content.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100F));
            content.AutoSize = true;
            content.AutoSizeMode = AutoSizeMode.GrowAndShrink;
            content.Dock = DockStyle.Top;
            content.Padding = new Padding(0, 0, 6, 0);

            content.Controls.Add(MakeGroup("Config.Group.General", BuildGeneralGrid()));
            content.Controls.Add(MakeGroup("Config.Group.Resolution", BuildResolutionGrid()));
            content.Controls.Add(MakeGroup("Config.Group.Fixes", BuildFixesGrid()));
            content.Controls.Add(MakeGroup("Config.Group.Span", BuildSpanGrid()));
            content.Controls.Add(MakeGroup("Config.Group.Camera", BuildCameraGrid()));
            content.Controls.Add(MakeGroup("Config.Group.Graphics", BuildGraphicsGrid()));
            content.Controls.Add(MakeGroup("Config.Group.Experimental", BuildExperimentalGrid()));

            _configScroll.Controls.Add(content);
            root.Controls.Add(_configScroll, 0, 1);

            var buttons = new FlowLayoutPanel();
            buttons.AutoSize = true;
            buttons.AutoSizeMode = AutoSizeMode.GrowAndShrink;
            buttons.Anchor = AnchorStyles.Left | AnchorStyles.Top;
            buttons.Margin = new Padding(0, 6, 0, 0);

            _btnSaveCfg = MakeButton(delegate(object s, EventArgs e) { SaveConfig(); });
            _btnReloadCfg = MakeButton(delegate(object s, EventArgs e) { LoadConfig(true); });
            _btnDefaultsCfg = MakeButton(delegate(object s, EventArgs e) { ApplyDefaults(); });
            RegisterLocalized(_btnSaveCfg, "Config.Save");
            RegisterLocalized(_btnReloadCfg, "Config.Reload");
            RegisterLocalized(_btnDefaultsCfg, "Config.Defaults");

            buttons.Controls.Add(_btnSaveCfg);
            buttons.Controls.Add(_btnReloadCfg);
            buttons.Controls.Add(_btnDefaultsCfg);
            root.Controls.Add(buttons, 0, 2);

            return root;
        }

        private TableLayoutPanel BuildGeneralGrid()
        {
            var g = MakeGrid(3);
            _numDelay = MakeNum(0m, 60000m, 100m, 0);
            _numDelay.Value = 1000m;
            g.Controls.Add(MakeLabel("Config.Delay"), 0, 0);
            g.Controls.Add(_numDelay, 1, 0);
            g.Controls.Add(MakeHint("Config.DelayHint"), 2, 0);
            return g;
        }

        private TableLayoutPanel BuildResolutionGrid()
        {
            var g = MakeGrid(4);
            _chkResEnabled = MakeCheck("Config.ResEnabled");
            g.Controls.Add(_chkResEnabled, 0, 0);
            g.SetColumnSpan(_chkResEnabled, 4);

            _numWidth = MakeNum(0m, 16384m, 10m, 0);
            _numHeight = MakeNum(0m, 16384m, 10m, 0);
            g.Controls.Add(MakeLabel("Config.Width"), 0, 1);
            g.Controls.Add(_numWidth, 1, 1);
            g.Controls.Add(MakeLabel("Config.Height"), 2, 1);
            g.Controls.Add(_numHeight, 3, 1);

            var hint = MakeHint("Config.ResHint");
            g.Controls.Add(hint, 0, 2);
            g.SetColumnSpan(hint, 4);
            return g;
        }

        private TableLayoutPanel BuildFixesGrid()
        {
            var g = MakeGrid(1);
            _chkFixHud = MakeCheck("Config.FixHud");
            _chkFixAspect = MakeCheck("Config.FixAspect");
            _chkFixNameplates = MakeCheck("Config.FixNameplates");
            _chkFixFov = MakeCheck("Config.FixFov");
            g.Controls.Add(_chkFixHud, 0, 0);
            g.Controls.Add(_chkFixAspect, 0, 1);
            g.Controls.Add(_chkFixNameplates, 0, 2);
            g.Controls.Add(_chkFixFov, 0, 3);
            return g;
        }

        private TableLayoutPanel BuildSpanGrid()
        {
            var g = MakeGrid(4);
            _chkSpanEnabled = MakeCheck("Config.SpanEnabled");
            g.Controls.Add(_chkSpanEnabled, 0, 0);
            g.SetColumnSpan(_chkSpanEnabled, 4);

            _cboSpanRatio = new ComboBox();
            _cboSpanRatio.DropDownStyle = ComboBoxStyle.DropDownList;
            _cboSpanRatio.Width = 210;
            _cboSpanRatio.Items.Add(new ComboItem("Config.SpanRatio.Auto", 0.0));
            _cboSpanRatio.Items.Add(new ComboItem("Config.SpanRatio.169", 1.7778));
            _cboSpanRatio.Items.Add(new ComboItem("Config.SpanRatio.219", 2.3889));
            _cboSpanRatio.Items.Add(new ComboItem("Config.SpanRatio.329", 3.5556));
            _cboSpanRatio.Items.Add(new ComboItem("Config.SpanRatio.Custom", double.NaN));
            _cboSpanRatio.SelectedIndex = 0;
            _cboSpanRatio.SelectedIndexChanged += delegate { UpdateSpanCustomEnabled(); };

            _numSpanCustom = MakeNum(0.1m, 8m, 0.01m, 4);
            _numSpanCustom.Value = 2.3889m;
            _numSpanCustom.Enabled = false;

            g.Controls.Add(MakeLabel("Config.SpanRatio"), 0, 1);
            g.Controls.Add(_cboSpanRatio, 1, 1);
            g.Controls.Add(_numSpanCustom, 2, 1);
            g.Controls.Add(MakeHint("Config.SpanCustomHint"), 3, 1);

            _chkSpanAllHud = MakeCheck("Config.SpanAllHud");
            _chkSpanAllBg = MakeCheck("Config.SpanAllBg");
            g.Controls.Add(_chkSpanAllHud, 0, 2);
            g.SetColumnSpan(_chkSpanAllHud, 4);
            g.Controls.Add(_chkSpanAllBg, 0, 3);
            g.SetColumnSpan(_chkSpanAllBg, 4);
            return g;
        }

        private TableLayoutPanel BuildCameraGrid()
        {
            var g = MakeGrid(3);
            _numFov = MakeNum(0.1m, 2.5m, 0.05m, 2);
            _numFov.Value = 1m;
            _numCam = MakeNum(0.1m, 2.5m, 0.05m, 2);
            _numCam.Value = 1m;

            g.Controls.Add(MakeLabel("Config.Fov"), 0, 0);
            g.Controls.Add(_numFov, 1, 0);
            g.Controls.Add(MakeHint("Config.FovHint"), 2, 0);

            g.Controls.Add(MakeLabel("Config.Cam"), 0, 1);
            g.Controls.Add(_numCam, 1, 1);
            g.Controls.Add(MakeHint("Config.CamHint"), 2, 1);
            return g;
        }

        private TableLayoutPanel BuildGraphicsGrid()
        {
            var g = MakeGrid(3);
            _chkShadowEnabled = MakeCheck("Config.ShadowEnabled");
            _cboShadow = new ComboBox();
            _cboShadow.DropDownStyle = ComboBoxStyle.DropDownList;
            _cboShadow.Width = 100;
            _cboShadow.Items.AddRange(new object[] { "512", "1024", "2048", "4096", "8192", "16384" });
            _cboShadow.SelectedItem = "4096";

            g.Controls.Add(_chkShadowEnabled, 0, 0);
            g.Controls.Add(_cboShadow, 1, 0);
            g.Controls.Add(MakeHint("Config.ShadowHint"), 2, 0);

            _numLod = MakeNum(0.1m, 10m, 0.1m, 2);
            _numLod.Value = 1m;
            g.Controls.Add(MakeLabel("Config.Lod"), 0, 1);
            g.Controls.Add(_numLod, 1, 1);
            var lodHint = MakeHint("Config.LodHint");
            lodHint.MaximumSize = new Size(560, 0);
            g.Controls.Add(lodHint, 2, 1);

            _chkDisableTaa = MakeCheck("Config.DisableTaa");
            g.Controls.Add(_chkDisableTaa, 0, 2);
            g.SetColumnSpan(_chkDisableTaa, 3);
            return g;
        }

        private TableLayoutPanel BuildExperimentalGrid()
        {
            var g = MakeGrid(1);
            _chkFpsCap = MakeCheck("Config.FpsCap");
            g.Controls.Add(_chkFpsCap, 0, 0);
            return g;
        }

        // ==================================================================
        //  i18n: localized redraw
        // ==================================================================

        /// <summary>Registers a control with its string key so its Text is reset when the language changes.</summary>
        private void RegisterLocalized(Control c, string key)
        {
            _localizedControls.Add(new KeyValuePair<Control, string>(c, key));
        }

        private void SelectLangItem(string lang)
        {
            for (int i = 0; i < _cboLang.Items.Count; i++)
            {
                var it = _cboLang.Items[i] as LangItem;
                if (it != null && it.Code == lang)
                {
                    _cboLang.SelectedIndex = i;
                    return;
                }
            }
            if (_cboLang.Items.Count > 0) _cboLang.SelectedIndex = 0;
        }

        private void OnLangChanged(object sender, EventArgs e)
        {
            var it = _cboLang.SelectedItem as LangItem;
            if (it != null)
                Strings.SetLanguage(it.Code); // triggers Strings.LanguageChanged -> ApplyLanguage
        }

        /// <summary>Resets all visible UI text according to the current language.</summary>
        private void ApplyLanguage()
        {
            SuspendLayout();

            Text = Strings.L("App.Title");
            _lblLangCaption.Text = Strings.L("Lang.Label");
            _tabInstall.Text = Strings.L("Tab.Install");
            _tabConfig.Text = Strings.L("Tab.Config");

            // Static install-tab text
            _grpPath.Text = Strings.L("Install.PathGroup");
            _btnBrowse.Text = Strings.L("Install.Browse");
            _btnDetect.Text = Strings.L("Install.Detect");
            _grpState.Text = Strings.L("Install.StateGroup");
            _lblLoaderCaption.Text = Strings.L("Install.LoaderLabel");
            _lblAsiCaption.Text = Strings.L("Install.AsiLabel");
            _lblIniCaption.Text = Strings.L("Install.IniLabel");
            _lblVersionCaption.Text = Strings.L("Install.VersionLabel");
            _btnInstall.Text = Strings.L("Install.Install");
            _btnUninstall.Text = Strings.L("Install.Uninstall");
            _btnOpenDir.Text = Strings.L("Install.OpenDir");
            _btnOpenLog.Text = Strings.L("Install.OpenLog");
            _grpMessages.Text = Strings.L("Install.MessagesGroup");

            // Registered static settings-tab text
            foreach (KeyValuePair<Control, string> pair in _localizedControls)
                pair.Key.Text = Strings.L(pair.Value);

            // Combo box items (display text comes from ToString(), so they must be redrawn)
            RefreshComboDisplay(_cboLang);
            RefreshComboDisplay(_cboSpanRatio);

            // Redraw dynamic status text for the current language
            RefreshStatus();
            RefreshConfigHint();
            RefreshUpdateNotice();

            ResumeLayout(true);
        }

        private static void RefreshComboDisplay(ComboBox cbo)
        {
            if (cbo == null || cbo.Items.Count == 0) return;
            // A DropDownList shows each item's ToString(), so it must be redrawn after a language switch.
            // Cache the items and the selection, clear, then re-add the same objects to force ToString() to be re-read.
            int sel = cbo.SelectedIndex;
            var items = new object[cbo.Items.Count];
            cbo.Items.CopyTo(items, 0);
            cbo.BeginUpdate();
            cbo.Items.Clear();
            cbo.Items.AddRange(items);
            if (sel >= 0 && sel < cbo.Items.Count)
                cbo.SelectedIndex = sel;
            cbo.EndUpdate();
        }

        // ==================================================================
        //  UI helpers (all built from string keys; the actual text is set in ApplyLanguage)
        // ==================================================================

        private static TableLayoutPanel MakeGrid(int cols)
        {
            var t = new TableLayoutPanel();
            t.ColumnCount = cols;
            t.AutoSize = true;
            t.AutoSizeMode = AutoSizeMode.GrowAndShrink;
            t.Dock = DockStyle.Top;
            return t;
        }

        private GroupBox MakeGroup(string titleKey, Control content)
        {
            var g = new GroupBox();
            g.AutoSize = true;
            g.AutoSizeMode = AutoSizeMode.GrowAndShrink;
            g.Anchor = AnchorStyles.Left | AnchorStyles.Right | AnchorStyles.Top;
            g.Padding = new Padding(10, 4, 10, 8);
            g.Margin = new Padding(3, 3, 3, 8);
            content.Dock = DockStyle.Top;
            g.Controls.Add(content);
            RegisterLocalized(g, titleKey);
            return g;
        }

        private Label MakeLabel(string key)
        {
            var l = new Label();
            l.AutoSize = true;
            l.Anchor = AnchorStyles.Left;
            l.Margin = new Padding(3, 7, 3, 3);
            RegisterLocalized(l, key);
            return l;
        }

        private Label MakeHint(string key)
        {
            var l = MakeLabel(key);
            l.ForeColor = SystemColors.GrayText;
            return l;
        }

        private static Label MakeStateLabel()
        {
            var l = new Label();
            l.AutoSize = true;
            l.Anchor = AnchorStyles.Left;
            l.Margin = new Padding(3, 7, 3, 3);
            l.ForeColor = SystemColors.GrayText;
            return l;
        }

        private CheckBox MakeCheck(string key)
        {
            var c = new CheckBox();
            c.AutoSize = true;
            c.Anchor = AnchorStyles.Left;
            c.Margin = new Padding(3, 4, 3, 4);
            RegisterLocalized(c, key);
            return c;
        }

        private static NumericUpDown MakeNum(decimal min, decimal max, decimal step, int decimals)
        {
            var n = new NumericUpDown();
            n.Minimum = min;
            n.Maximum = max;
            n.Increment = step;
            n.DecimalPlaces = decimals;
            n.Width = 90;
            n.Anchor = AnchorStyles.Left;
            n.Margin = new Padding(3, 4, 3, 4);
            return n;
        }

        private static Button MakeButton(EventHandler onClick)
        {
            var b = new Button();
            b.AutoSize = true;
            b.AutoSizeMode = AutoSizeMode.GrowAndShrink;
            b.MinimumSize = new Size(96, 30);
            b.Padding = new Padding(6, 2, 6, 2);
            b.Margin = new Padding(3, 3, 6, 3);
            if (onClick != null) b.Click += onClick;
            return b;
        }

        /// <summary>Combo item: display text comes from a string key; Value is the corresponding ratio (NaN = custom).</summary>
        private sealed class ComboItem
        {
            public readonly string Key;
            public readonly double Value; // NaN = custom
            public ComboItem(string key, double value) { Key = key; Value = value; }
            public override string ToString() { return Strings.L(Key); }
        }

        /// <summary>Language combo item.</summary>
        private sealed class LangItem
        {
            public readonly string Code;
            private readonly string _key;
            public LangItem(string code, string key) { Code = code; _key = key; }
            public override string ToString() { return Strings.L(_key); }
        }

        // ==================================================================
        //  Shared state
        // ==================================================================

        private string GamePath
        {
            get { return _txtGamePath.Text.Trim(); }
        }

        private string IniPath
        {
            get { return Path.Combine(GamePath, IniRelPath); }
        }

        private static string AppDir
        {
            get { return AppDomain.CurrentDomain.BaseDirectory; }
        }

        private static bool IsValidGamePath(string path)
        {
            try
            {
                return !string.IsNullOrEmpty(path) &&
                       Directory.Exists(path) &&
                       File.Exists(Path.Combine(path, GameExeName));
            }
            catch { return false; }
        }

        private void Log(string message)
        {
            _txtLog.AppendText("[" + DateTime.Now.ToString("HH:mm:ss") + "] " + message + Environment.NewLine);
        }

        // ==================================================================
        //  Startup and status
        // ==================================================================

        private void OnFormLoad(object sender, EventArgs e)
        {
            AutoDetect(false);
            RefreshStatus();
            StartUpdateCheck();
        }

        // ==================================================================
        //  Update check (GitHub releases)
        // ==================================================================

        private void StartUpdateCheck()
        {
            UpdateChecker.CheckAsync(delegate(UpdateChecker.Result result, Exception error)
            {
                // Runs on a thread-pool thread; marshal back onto the UI thread.
                try
                {
                    if (IsDisposed || !IsHandleCreated) return;
                    BeginInvoke((MethodInvoker)delegate
                    {
                        if (error != null)
                        {
                            Log(Strings.F("Update.CheckFailed", error.Message));
                            return;
                        }
                        _updateInfo = result;
                        RefreshUpdateNotice();
                        // The installed-vs-latest comparison depends on the GitHub result,
                        // so re-evaluate the install status now that it has arrived.
                        RefreshStatus();
                    });
                }
                catch (ObjectDisposedException) { }
                catch (InvalidOperationException) { }
            });
        }

        /// <summary>Shows/hides and (re)words the update notice; also called on language switch.</summary>
        private void RefreshUpdateNotice()
        {
            if (_lnkUpdate == null) return;
            if (_updateInfo != null && _updateInfo.UpdateAvailable)
            {
                _lnkUpdate.Text = Strings.F("Update.Available",
                    _updateInfo.LatestVersion, UpdateChecker.CurrentVersion);
                _lnkUpdate.Visible = true;
            }
            else
            {
                _lnkUpdate.Visible = false;
            }
        }

        private void OnUpdateLinkClicked(object sender, LinkLabelLinkClickedEventArgs e)
        {
            string url = (_updateInfo != null && !string.IsNullOrEmpty(_updateInfo.HtmlUrl))
                ? _updateInfo.HtmlUrl
                : UpdateChecker.ReleasesPageUrl;
            try { Process.Start(url); }
            catch (Exception ex) { Log(Strings.F("Update.OpenFailed", ex.Message)); }
        }

        private void OnTabChanged(object sender, EventArgs e)
        {
            if (_tabs.SelectedTab == _tabConfig)
                LoadConfig(false);
        }

        private void RefreshStatus()
        {
            string game = GamePath;
            bool valid = IsValidGamePath(game);

            if (string.IsNullOrEmpty(game))
            {
                _lblPathState.Text = Strings.L("Install.PathPrompt");
                _lblPathState.ForeColor = SystemColors.GrayText;
            }
            else if (valid)
            {
                _lblPathState.Text = Strings.F("Install.PathValid", GameExeName);
                _lblPathState.ForeColor = Color.Green;
            }
            else
            {
                _lblPathState.Text = Strings.F("Install.PathInvalid", GameExeName);
                _lblPathState.ForeColor = Color.Firebrick;
            }

            bool asiInstalled = valid && File.Exists(Path.Combine(game, AsiRelPath));
            SetState(_lblLoaderState, valid && File.Exists(Path.Combine(game, "winmm.dll")), valid);
            SetState(_lblAsiState, asiInstalled, valid);
            SetState(_lblIniState, valid && File.Exists(Path.Combine(game, IniRelPath)), valid);
            RefreshInstalledVersion(game, valid, asiInstalled);

            _btnInstall.Enabled = valid;
            _btnUninstall.Enabled = valid;
            _btnOpenDir.Enabled = valid;
            _btnOpenLog.Enabled = valid;
        }

        private static void SetState(Label label, bool installed, bool pathValid)
        {
            if (!pathValid)
            {
                label.Text = Strings.L("State.NA");
                label.ForeColor = SystemColors.GrayText;
            }
            else if (installed)
            {
                label.Text = Strings.L("State.Installed");
                label.ForeColor = Color.Green;
            }
            else
            {
                label.Text = Strings.L("State.NotInstalled");
                label.ForeColor = Color.Firebrick;
            }
        }

        /// <summary>
        /// Detects and displays the plugin version installed in the game folder, and warns
        /// when it is older than the latest known version. "Latest" is the higher of the
        /// installer's own shipping version and the newest GitHub release (when the online
        /// check has already returned). Uses UpdateChecker.CompareVersions throughout.
        /// </summary>
        private void RefreshInstalledVersion(string game, bool pathValid, bool asiInstalled)
        {
            if (_lblVersionState == null) return;

            // Path not valid, or the plugin isn't installed at all: nothing to report here
            // (the ASI row already shows "Not installed"), keep the version row neutral.
            if (!pathValid || !asiInstalled)
            {
                _lblVersionState.Text = Strings.L("State.NA");
                _lblVersionState.ForeColor = SystemColors.GrayText;
                return;
            }

            string installed = InstalledVersion.Detect(game);
            string latest = LatestKnownVersion();

            if (string.IsNullOrEmpty(installed))
            {
                // Installed but version undetectable (old .asi with no marker and no log yet).
                // Soft prompt: possibly outdated, never a hard error.
                _lblVersionState.Text = Strings.F("Version.UnknownOutdated", latest);
                _lblVersionState.ForeColor = Color.Firebrick;
                return;
            }

            int cmp = UpdateChecker.CompareVersions(installed, latest);
            if (cmp < 0)
            {
                _lblVersionState.Text = Strings.F("Version.Outdated", installed, latest);
                _lblVersionState.ForeColor = Color.Firebrick;
            }
            else if (cmp == 0)
            {
                _lblVersionState.Text = Strings.F("Version.UpToDate", installed);
                _lblVersionState.ForeColor = Color.Green;
            }
            else
            {
                // Installed is newer than anything we know about (dev/local build) - just
                // show it, no warning.
                _lblVersionState.Text = Strings.F("Version.Value", installed);
                _lblVersionState.ForeColor = SystemColors.ControlText;
            }
        }

        /// <summary>
        /// The newest version to compare an installed plugin against: the higher of this
        /// installer's shipping version and the latest GitHub release (if the check ran).
        /// </summary>
        private string LatestKnownVersion()
        {
            string latest = UpdateChecker.CurrentVersion;
            if (_updateInfo != null && !string.IsNullOrEmpty(_updateInfo.LatestVersion) &&
                UpdateChecker.CompareVersions(_updateInfo.LatestVersion, latest) > 0)
                latest = _updateInfo.LatestVersion;
            return latest;
        }

        // ==================================================================
        //  Path detection
        // ==================================================================

        private void OnBrowse(object sender, EventArgs e)
        {
            using (var dlg = new FolderBrowserDialog())
            {
                dlg.Description = Strings.F("Browse.Description", GameExeName);
                if (IsValidGamePath(GamePath))
                    dlg.SelectedPath = GamePath;
                if (dlg.ShowDialog(this) == DialogResult.OK)
                    _txtGamePath.Text = dlg.SelectedPath;
            }
        }

        private void AutoDetect(bool interactive)
        {
            foreach (string candidate in EnumerateCandidatePaths())
            {
                if (IsValidGamePath(candidate))
                {
                    _txtGamePath.Text = candidate;
                    Log(Strings.F("Detect.Found", candidate));
                    return;
                }
            }

            Log(Strings.L("Detect.NotFound"));
            if (interactive)
                MessageBox.Show(this, Strings.L("Detect.DialogBody"),
                    Strings.L("Detect.DialogTitle"), MessageBoxButtons.OK, MessageBoxIcon.Information);
        }

        private static IEnumerable<string> EnumerateCandidatePaths()
        {
            var results = new List<string>();
            string steamRoot = GetSteamInstallPath();

            // 1) Parse libraryfolders.vdf to find the library that holds app 881020
            if (!string.IsNullOrEmpty(steamRoot))
            {
                string vdf = Path.Combine(steamRoot, "steamapps", "libraryfolders.vdf");
                try
                {
                    if (File.Exists(vdf))
                    {
                        foreach (string lib in ParseLibrariesForApp(vdf, SteamAppId))
                            results.Add(Path.Combine(lib, "steamapps", "common", GameFolderName));
                    }
                }
                catch { }

                // 2) Steam's own library
                results.Add(Path.Combine(steamRoot, "steamapps", "common", GameFolderName));
            }

            // 3) Common-path fallback
            DriveInfo[] drives;
            try { drives = DriveInfo.GetDrives(); }
            catch { drives = new DriveInfo[0]; }

            foreach (DriveInfo d in drives)
            {
                if (d.DriveType != DriveType.Fixed) continue;
                string r = d.RootDirectory.FullName;
                results.Add(Path.Combine(r, "SteamLibrary", "steamapps", "common", GameFolderName));
                results.Add(Path.Combine(r, "Steam", "steamapps", "common", GameFolderName));
                results.Add(Path.Combine(r, "Games", "Steam", "steamapps", "common", GameFolderName));
                results.Add(Path.Combine(r, "Program Files (x86)", "Steam", "steamapps", "common", GameFolderName));
                results.Add(Path.Combine(r, "Program Files", "Steam", "steamapps", "common", GameFolderName));
            }
            return results;
        }

        private static string GetSteamInstallPath()
        {
            string[] keys = new string[]
            {
                "SOFTWARE\\WOW6432Node\\Valve\\Steam",
                "SOFTWARE\\Valve\\Steam"
            };
            foreach (string k in keys)
            {
                try
                {
                    using (RegistryKey key = Registry.LocalMachine.OpenSubKey(k))
                    {
                        if (key != null)
                        {
                            object v = key.GetValue("InstallPath");
                            if (v != null && Directory.Exists(v.ToString()))
                                return v.ToString();
                        }
                    }
                }
                catch { }
            }
            try
            {
                using (RegistryKey key = Registry.CurrentUser.OpenSubKey("SOFTWARE\\Valve\\Steam"))
                {
                    if (key != null)
                    {
                        object v = key.GetValue("SteamPath");
                        if (v != null)
                        {
                            string p = v.ToString().Replace('/', '\\');
                            if (Directory.Exists(p)) return p;
                        }
                    }
                }
            }
            catch { }
            return null;
        }

        /// <summary>Finds library paths from libraryfolders.vdf that contain the given appid.</summary>
        private static List<string> ParseLibrariesForApp(string vdfPath, string appId)
        {
            var libs = new List<string>();
            string text = File.ReadAllText(vdfPath);
            var pathRegex = new Regex("\"path\"\\s*\"((?:[^\"\\\\]|\\\\.)*)\"", RegexOptions.IgnoreCase);
            var appRegex = new Regex("\"" + Regex.Escape(appId) + "\"\\s*\"");

            MatchCollection matches = pathRegex.Matches(text);
            for (int i = 0; i < matches.Count; i++)
            {
                int start = matches[i].Index;
                int end = (i + 1 < matches.Count) ? matches[i + 1].Index : text.Length;
                string block = text.Substring(start, end - start);
                if (appRegex.IsMatch(block))
                {
                    string p = matches[i].Groups[1].Value.Replace("\\\\", "\\");
                    if (!libs.Contains(p)) libs.Add(p);
                }
            }
            return libs;
        }

        // ==================================================================
        //  Install / uninstall
        // ==================================================================

        private void DoInstall()
        {
            string game = GamePath;
            if (!IsValidGamePath(game))
            {
                MessageBox.Show(this, Strings.F("Install.PathInvalidBody", GameExeName),
                    Strings.L("Install.Title"), MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            // --- payload check ---
            // payload\ mirrors the game-directory layout (winmm.dll at the root, the
            // GBFRUltrawide.* files under scripts\), so the same folder works both as this
            // installer's source and as a "copy these into the game folder" manual install.
            string payloadDir = Path.Combine(AppDir, "payload");
            string[] payloadFiles = new string[] { "winmm.dll", AsiRelPath, IniRelPath };
            var missing = new List<string>();
            foreach (string f in payloadFiles)
            {
                if (!File.Exists(Path.Combine(payloadDir, f)))
                    missing.Add(f);
            }
            if (missing.Count > 0)
            {
                MessageBox.Show(this,
                    Strings.F("Install.PayloadMissingBody", string.Join("\n", missing.ToArray()), payloadDir),
                    Strings.L("Install.PayloadMissingTitle"), MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            try
            {
                string scriptsDir = Path.Combine(game, "scripts");

                // --- Detect legacy GBFRelinkFix files ---
                string[] legacyCandidates = new string[]
                {
                    "scripts\\GBFRelinkFix.asi",
                    "scripts\\GBFRelinkFix.ini",
                    "dinput8.dll"
                };
                var legacyFound = new List<string>();
                foreach (string rel in legacyCandidates)
                {
                    if (File.Exists(Path.Combine(game, rel)))
                        legacyFound.Add(rel);
                }

                bool removeLegacy = false;
                if (legacyFound.Count > 0)
                {
                    DialogResult r = MessageBox.Show(this,
                        Strings.F("Install.LegacyBody", string.Join("\n", legacyFound.ToArray())),
                        Strings.L("Install.LegacyTitle"), MessageBoxButtons.YesNo, MessageBoxIcon.Question);
                    removeLegacy = (r == DialogResult.Yes);
                }

                // --- Collect files to back up (those that will be overwritten or deleted) ---
                var toBackup = new List<string>();
                if (File.Exists(Path.Combine(game, "winmm.dll"))) toBackup.Add("winmm.dll");
                if (File.Exists(Path.Combine(game, AsiRelPath))) toBackup.Add(AsiRelPath);
                if (removeLegacy)
                {
                    foreach (string rel in legacyFound)
                    {
                        if (!toBackup.Contains(rel)) toBackup.Add(rel);
                    }
                }

                string backupDir = null;
                if (toBackup.Count > 0)
                {
                    backupDir = Path.Combine(game, BackupRootName, DateTime.Now.ToString("yyyyMMdd_HHmmss"));
                    foreach (string rel in toBackup)
                    {
                        string src = Path.Combine(game, rel);
                        string dst = Path.Combine(backupDir, rel);
                        Directory.CreateDirectory(Path.GetDirectoryName(dst));
                        File.Copy(src, dst, true);
                    }
                    Log(Strings.F("Install.BackupDone", toBackup.Count, backupDir));
                }

                // --- Remove legacy files ---
                if (removeLegacy)
                {
                    foreach (string rel in legacyFound)
                    {
                        File.Delete(Path.Combine(game, rel));
                        Log(Strings.F("Install.LegacyRemoved", rel));
                    }
                }

                // --- Deploy ---
                Directory.CreateDirectory(scriptsDir);
                File.Copy(Path.Combine(payloadDir, "winmm.dll"), Path.Combine(game, "winmm.dll"), true);
                Log(Strings.L("Install.DeployedLoader"));

                File.Copy(Path.Combine(payloadDir, AsiRelPath), Path.Combine(game, AsiRelPath), true);
                Log(Strings.L("Install.DeployedAsi"));

                string iniDst = Path.Combine(game, IniRelPath);
                if (File.Exists(iniDst))
                {
                    Log(Strings.L("Install.IniKept"));
                }
                else
                {
                    File.Copy(Path.Combine(payloadDir, IniRelPath), iniDst, false);
                    Log(Strings.L("Install.DeployedIni"));
                }

                Log(Strings.L("Install.Done"));
                string doneMsg = (backupDir != null)
                    ? Strings.F("Install.DoneWithBackup", backupDir)
                    : Strings.L("Install.DoneMsg");
                MessageBox.Show(this, doneMsg, Strings.L("Install.Title"), MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            catch (UnauthorizedAccessException ex)
            {
                Log(Strings.F("Install.FailedPermLog", ex.Message));
                MessageBox.Show(this, Strings.F("Install.FailedPermBody", ex.Message),
                    Strings.L("Install.FailedPermTitle"), MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            catch (Exception ex)
            {
                Log(Strings.F("Install.FailedLog", ex.Message));
                MessageBox.Show(this, Strings.F("Install.FailedBody", ex.Message),
                    Strings.L("Install.FailedTitle"), MessageBoxButtons.OK, MessageBoxIcon.Error);
            }

            RefreshStatus();
        }

        private void DoUninstall()
        {
            string game = GamePath;
            if (!IsValidGamePath(game))
            {
                MessageBox.Show(this, Strings.F("Install.PathInvalidBody", GameExeName),
                    Strings.L("Install.Uninstall.Title"), MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            DialogResult r = MessageBox.Show(this,
                Strings.F("Uninstall.ConfirmBody", AsiRelPath, IniRelPath),
                Strings.L("Install.Uninstall.Title"), MessageBoxButtons.YesNoCancel, MessageBoxIcon.Question);
            if (r == DialogResult.Cancel)
                return;
            bool deleteIni = (r == DialogResult.Yes);

            try
            {
                var targets = new List<string>();
                targets.Add("winmm.dll");
                targets.Add(AsiRelPath);
                if (deleteIni) targets.Add(IniRelPath);

                int removed = 0;
                foreach (string rel in targets)
                {
                    string p = Path.Combine(game, rel);
                    if (File.Exists(p))
                    {
                        File.Delete(p);
                        Log(Strings.F("Uninstall.Deleted", rel));
                        removed++;
                    }
                }
                if (removed == 0)
                    Log(Strings.L("Uninstall.NothingToRemove"));
                else
                    Log(Strings.F("Uninstall.Done", removed));

                // List existing backups
                string backupRoot = Path.Combine(game, BackupRootName);
                if (Directory.Exists(backupRoot))
                {
                    string[] backups = Directory.GetDirectories(backupRoot);
                    if (backups.Length > 0)
                    {
                        Log(Strings.L("Uninstall.BackupsFound"));
                        foreach (string b in backups)
                            Log("    " + b);
                        Log(Strings.L("Uninstall.BackupHint"));
                    }
                }
            }
            catch (UnauthorizedAccessException ex)
            {
                Log(Strings.F("Uninstall.FailedPermLog", ex.Message));
                MessageBox.Show(this, Strings.F("Uninstall.FailedPermBody", ex.Message),
                    Strings.L("Uninstall.FailedPermTitle"), MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            catch (Exception ex)
            {
                Log(Strings.F("Uninstall.FailedLog", ex.Message));
                MessageBox.Show(this, Strings.F("Uninstall.FailedBody", ex.Message),
                    Strings.L("Uninstall.FailedTitle"), MessageBoxButtons.OK, MessageBoxIcon.Error);
            }

            RefreshStatus();
        }

        private void OpenGameDir()
        {
            string game = GamePath;
            if (!Directory.Exists(game)) return;
            try { Process.Start("explorer.exe", "\"" + game + "\""); }
            catch (Exception ex) { Log(Strings.F("OpenDir.Failed", ex.Message)); }
        }

        private void OpenLogFile()
        {
            string logPath = Path.Combine(GamePath, LogRelPath);
            if (!File.Exists(logPath))
            {
                MessageBox.Show(this, Strings.F("OpenLog.NotFound", logPath),
                    Strings.L("OpenLog.Title"), MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            try { Process.Start("notepad.exe", "\"" + logPath + "\""); }
            catch (Exception ex) { Log(Strings.F("OpenLog.Failed", ex.Message)); }
        }

        // ==================================================================
        //  Settings tab: read/write ini
        // ==================================================================

        private string ParamsSection()
        {
            if (_ini != null)
            {
                if (_ini.HasSection("GBFRelinkFix Parameters")) return "GBFRelinkFix Parameters";
                if (_ini.HasSection("GBFRUltrawide Parameters")) return "GBFRUltrawide Parameters";
            }
            return "GBFRelinkFix Parameters";
        }

        private void SetConfigEnabled(bool enabled)
        {
            _configScroll.Enabled = enabled;
            _btnSaveCfg.Enabled = enabled;
            _btnDefaultsCfg.Enabled = enabled;
            _btnReloadCfg.Enabled = enabled;
        }

        // Tracks the current settings-tab state so the hint can be redrawn on a language switch.
        private enum ConfigHintState { NotLoaded, NoIni, LoadFailed, Loaded }
        private ConfigHintState _configHintState = ConfigHintState.NotLoaded;
        private string _configHintArg = "";

        private void RefreshConfigHint()
        {
            if (_lblConfigHint == null) return;
            switch (_configHintState)
            {
                case ConfigHintState.NoIni:
                    _lblConfigHint.ForeColor = Color.Firebrick;
                    _lblConfigHint.Text = Strings.L("Config.HintNoIni");
                    break;
                case ConfigHintState.LoadFailed:
                    _lblConfigHint.ForeColor = Color.Firebrick;
                    _lblConfigHint.Text = Strings.F("Config.HintLoadFailed", _configHintArg);
                    break;
                case ConfigHintState.Loaded:
                    _lblConfigHint.ForeColor = Color.Green;
                    _lblConfigHint.Text = Strings.F("Config.HintLoaded", _configHintArg);
                    break;
                default:
                    _lblConfigHint.ForeColor = Color.Firebrick;
                    _lblConfigHint.Text = Strings.L("Config.HintNotLoaded");
                    break;
            }
        }

        private void LoadConfig(bool interactive)
        {
            string iniPath = IniPath;
            if (!IsValidGamePath(GamePath) || !File.Exists(iniPath))
            {
                _ini = null;
                SetConfigEnabled(false);
                _configHintState = ConfigHintState.NoIni;
                RefreshConfigHint();
                return;
            }

            try
            {
                _ini = new IniFile(iniPath);
            }
            catch (Exception ex)
            {
                _ini = null;
                SetConfigEnabled(false);
                _configHintState = ConfigHintState.LoadFailed;
                _configHintArg = ex.Message;
                RefreshConfigHint();
                return;
            }

            SetConfigEnabled(true);
            _configHintState = ConfigHintState.Loaded;
            _configHintArg = iniPath;
            RefreshConfigHint();

            string ps = ParamsSection();
            SetNum(_numDelay, _ini.GetInt(ps, "InjectionDelay", 1000));

            _chkResEnabled.Checked = _ini.GetBool("Custom Resolution", "Enabled", true);
            SetNum(_numWidth, _ini.GetInt("Custom Resolution", "Width", 0));
            SetNum(_numHeight, _ini.GetInt("Custom Resolution", "Height", 0));

            _chkFixHud.Checked = _ini.GetBool("Fix HUD", "Enabled", true);
            _chkFixAspect.Checked = _ini.GetBool("Fix Aspect Ratio", "Enabled", true);
            _chkFixNameplates.Checked = _ini.GetBool("Fix Nameplates", "Enabled", true);
            _chkFixFov.Checked = _ini.GetBool("Fix FOV", "Enabled", true);

            _chkSpanEnabled.Checked = _ini.GetBool("Span HUD", "Enabled", true);
            double ratio = _ini.GetDouble("Span HUD", "AspectRatio", 0.0);
            SelectSpanRatio(ratio);
            _chkSpanAllHud.Checked = _ini.GetBool("Span HUD", "SpanAllHUD", true);
            _chkSpanAllBg.Checked = _ini.GetBool("Span HUD", "SpanAllBackgrounds", false);

            SetNum(_numFov, (decimal)_ini.GetDouble("Gameplay FOV", "Multiplier", 1.0));
            SetNum(_numCam, (decimal)_ini.GetDouble("Gameplay Camera Distance", "Multiplier", 1.0));

            _chkShadowEnabled.Checked = _ini.GetBool("Shadow Quality", "Enabled", false);
            SelectShadowValue(_ini.GetInt("Shadow Quality", "Value", 4096));

            SetNum(_numLod, (decimal)_ini.GetDouble("Level of Detail", "Multiplier", 1.0));
            _chkDisableTaa.Checked = _ini.GetBool("Disable TAA", "Enabled", false);
            _chkFpsCap.Checked = _ini.GetBool("Raise Framerate Cap", "Enabled", false);

            if (interactive)
                Log(Strings.F("Config.Reloaded", iniPath));
        }

        private void SaveConfig()
        {
            if (_ini == null) return;
            try
            {
                // Reload first to avoid clobbering external edits; update key by key, preserving comments and unknown keys.
                _ini.Reload();
                string ps = ParamsSection();

                _ini.SetInt(ps, "InjectionDelay", (int)_numDelay.Value);

                _ini.SetBool("Custom Resolution", "Enabled", _chkResEnabled.Checked);
                _ini.SetInt("Custom Resolution", "Width", (int)_numWidth.Value);
                _ini.SetInt("Custom Resolution", "Height", (int)_numHeight.Value);

                _ini.SetBool("Fix HUD", "Enabled", _chkFixHud.Checked);
                _ini.SetBool("Fix Aspect Ratio", "Enabled", _chkFixAspect.Checked);
                _ini.SetBool("Fix Nameplates", "Enabled", _chkFixNameplates.Checked);
                _ini.SetBool("Fix FOV", "Enabled", _chkFixFov.Checked);

                _ini.SetBool("Span HUD", "Enabled", _chkSpanEnabled.Checked);
                _ini.SetDouble("Span HUD", "AspectRatio", GetSpanRatioValue());
                _ini.SetBool("Span HUD", "SpanAllHUD", _chkSpanAllHud.Checked);
                _ini.SetBool("Span HUD", "SpanAllBackgrounds", _chkSpanAllBg.Checked);

                _ini.SetDouble("Gameplay FOV", "Multiplier", (double)_numFov.Value);
                _ini.SetDouble("Gameplay Camera Distance", "Multiplier", (double)_numCam.Value);

                _ini.SetBool("Shadow Quality", "Enabled", _chkShadowEnabled.Checked);
                _ini.SetInt("Shadow Quality", "Value", GetShadowValue());

                _ini.SetDouble("Level of Detail", "Multiplier", (double)_numLod.Value);
                _ini.SetBool("Disable TAA", "Enabled", _chkDisableTaa.Checked);
                _ini.SetBool("Raise Framerate Cap", "Enabled", _chkFpsCap.Checked);

                _ini.Save();
                Log(Strings.F("Config.Saved", _ini.FilePath));
                MessageBox.Show(this, Strings.L("Config.SavedBody"),
                    Strings.L("Config.SavedTitle"), MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            catch (UnauthorizedAccessException ex)
            {
                MessageBox.Show(this, Strings.F("Config.SaveFailedPermBody", ex.Message),
                    Strings.L("Config.SaveFailedPermTitle"), MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, Strings.F("Config.SaveFailedBody", ex.Message),
                    Strings.L("Config.SaveFailedTitle"), MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private void ApplyDefaults()
        {
            SetNum(_numDelay, 1000m);
            _chkResEnabled.Checked = true;
            SetNum(_numWidth, 0m);
            SetNum(_numHeight, 0m);
            _chkFixHud.Checked = true;
            _chkFixAspect.Checked = true;
            _chkFixNameplates.Checked = true;
            _chkFixFov.Checked = true;
            _chkSpanEnabled.Checked = true;
            SelectSpanRatio(0.0);
            _chkSpanAllHud.Checked = true;
            _chkSpanAllBg.Checked = false;
            SetNum(_numFov, 1m);
            SetNum(_numCam, 1m);
            _chkShadowEnabled.Checked = false;
            SelectShadowValue(4096);
            SetNum(_numLod, 1m);
            _chkDisableTaa.Checked = false;
            _chkFpsCap.Checked = false;
            Log(Strings.L("Config.DefaultsApplied"));
        }

        // ---------- Control and value conversion ----------

        private static void SetNum(NumericUpDown n, decimal value)
        {
            if (value < n.Minimum) value = n.Minimum;
            if (value > n.Maximum) value = n.Maximum;
            n.Value = value;
        }

        private void UpdateSpanCustomEnabled()
        {
            var item = _cboSpanRatio.SelectedItem as ComboItem;
            _numSpanCustom.Enabled = (item != null && double.IsNaN(item.Value));
        }

        private void SelectSpanRatio(double ratio)
        {
            const double tol = 0.02;
            int customIndex = _cboSpanRatio.Items.Count - 1;
            int selected = customIndex;
            for (int i = 0; i < _cboSpanRatio.Items.Count; i++)
            {
                var item = _cboSpanRatio.Items[i] as ComboItem;
                if (item != null && !double.IsNaN(item.Value) && Math.Abs(item.Value - ratio) < tol)
                {
                    selected = i;
                    break;
                }
            }
            _cboSpanRatio.SelectedIndex = selected;
            if (selected == customIndex)
                SetNum(_numSpanCustom, (decimal)ratio);
            UpdateSpanCustomEnabled();
        }

        private double GetSpanRatioValue()
        {
            var item = _cboSpanRatio.SelectedItem as ComboItem;
            if (item == null) return 0.0;
            if (double.IsNaN(item.Value)) return (double)_numSpanCustom.Value;
            return item.Value;
        }

        private void SelectShadowValue(int value)
        {
            string s = value.ToString(CultureInfo.InvariantCulture);
            if (!_cboShadow.Items.Contains(s))
                _cboShadow.Items.Add(s);
            _cboShadow.SelectedItem = s;
        }

        private int GetShadowValue()
        {
            int v;
            if (_cboShadow.SelectedItem != null &&
                int.TryParse(_cboShadow.SelectedItem.ToString(), NumberStyles.Integer, CultureInfo.InvariantCulture, out v))
                return v;
            return 4096;
        }
    }
}
