using System;
using System.Drawing;
using System.IO;
using System.Text.RegularExpressions;
using System.Windows.Forms;
using Microsoft.Win32;

namespace BalatroInGTA
{
    public class InstallerForm : Form
    {
        private TextBox txtGtaPath;
        private TextBox txtBalatroExe;
        private Label lblStatus;
        private Label lblGtaStatus;
        private Label lblBalatroStatus;
        private Button btnInstall;
        private Button btnUninstall;
        private ProgressBar progressBar;

        // Colors
        private readonly Color bgDark = Color.FromArgb(24, 26, 32);
        private readonly Color cardBg = Color.FromArgb(32, 35, 44);
        private readonly Color cardBorder = Color.FromArgb(48, 52, 66);
        private readonly Color accentColor = Color.FromArgb(220, 53, 69); // Balatro Red
        private readonly Color accentBlue = Color.FromArgb(13, 110, 253);
        private readonly Color accentGreen = Color.FromArgb(25, 135, 84);
        private readonly Color textWhite = Color.FromArgb(245, 245, 245);
        private readonly Color textMuted = Color.FromArgb(160, 165, 175);

        public InstallerForm()
        {
            InitializeComponent();
            AutoDetectPaths();
        }

        private void InitializeComponent()
        {
            this.Text = "Balatro in GTA V - Setup Installer";
            this.Size = new Size(680, 580);
            this.StartPosition = FormStartPosition.CenterScreen;
            this.FormBorderStyle = FormBorderStyle.FixedSingle;
            this.MaximizeBox = false;
            this.BackColor = bgDark;
            this.ForeColor = textWhite;
            this.Font = new Font("Segoe UI", 9.5f, FontStyle.Regular);

            // Header Banner
            Panel headerPanel = new Panel
            {
                Dock = DockStyle.Top,
                Height = 85,
                BackColor = Color.FromArgb(18, 20, 24)
            };

            Label titleLabel = new Label
            {
                Text = "🃏 BALATRO IN GTA V",
                Font = new Font("Segoe UI", 16f, FontStyle.Bold),
                ForeColor = Color.FromArgb(255, 75, 75),
                Location = new Point(24, 16),
                AutoSize = true
            };
            headerPanel.Controls.Add(titleLabel);

            Label subTitleLabel = new Label
            {
                Text = "Play full Balatro directly inside your in-game cell phone or fullscreen",
                Font = new Font("Segoe UI", 9.5f, FontStyle.Regular),
                ForeColor = textMuted,
                Location = new Point(26, 48),
                AutoSize = true
            };
            headerPanel.Controls.Add(subTitleLabel);

            this.Controls.Add(headerPanel);

            // Main Content Container
            Panel contentPanel = new Panel
            {
                Location = new Point(24, 100),
                Size = new Size(616, 360),
                BackColor = Color.Transparent
            };

            // GTA V Card
            Panel gtaCard = CreateCard(0, 0, 616, 115, "1. Grand Theft Auto V Directory");
            Label lblGtaDesc = new Label
            {
                Text = "Select your GTA V folder containing GTA5.exe or GTA5_Enhanced.exe:",
                ForeColor = textMuted,
                Location = new Point(16, 32),
                Size = new Size(580, 20)
            };
            gtaCard.Controls.Add(lblGtaDesc);

            txtGtaPath = new TextBox
            {
                Location = new Point(16, 56),
                Size = new Size(480, 26),
                BackColor = Color.FromArgb(20, 22, 28),
                ForeColor = textWhite,
                BorderStyle = BorderStyle.FixedSingle
            };
            txtGtaPath.TextChanged += (s, e) => ValidatePaths();
            gtaCard.Controls.Add(txtGtaPath);

            Button btnBrowseGta = CreateButton("Browse...", 504, 55, 95, 28, cardBg, textWhite, cardBorder);
            btnBrowseGta.Click += BtnBrowseGta_Click;
            gtaCard.Controls.Add(btnBrowseGta);

            lblGtaStatus = new Label
            {
                Location = new Point(16, 88),
                Size = new Size(580, 20),
                Font = new Font("Segoe UI", 8.5f, FontStyle.Regular),
                ForeColor = textMuted
            };
            gtaCard.Controls.Add(lblGtaStatus);
            contentPanel.Controls.Add(gtaCard);

            // Balatro Card
            Panel balatroCard = CreateCard(0, 130, 616, 115, "2. Balatro Executable (Balatro.exe)");
            Label lblBalatroDesc = new Label
            {
                Text = "Select your Balatro.exe (auto-launched when opening the phone in GTA V):",
                ForeColor = textMuted,
                Location = new Point(16, 32),
                Size = new Size(580, 20)
            };
            balatroCard.Controls.Add(lblBalatroDesc);

            txtBalatroExe = new TextBox
            {
                Location = new Point(16, 56),
                Size = new Size(480, 26),
                BackColor = Color.FromArgb(20, 22, 28),
                ForeColor = textWhite,
                BorderStyle = BorderStyle.FixedSingle
            };
            txtBalatroExe.TextChanged += (s, e) => ValidatePaths();
            balatroCard.Controls.Add(txtBalatroExe);

            Button btnBrowseBalatro = CreateButton("Browse...", 504, 55, 95, 28, cardBg, textWhite, cardBorder);
            btnBrowseBalatro.Click += BtnBrowseBalatro_Click;
            balatroCard.Controls.Add(btnBrowseBalatro);

            lblBalatroStatus = new Label
            {
                Location = new Point(16, 88),
                Size = new Size(580, 20),
                Font = new Font("Segoe UI", 8.5f, FontStyle.Regular),
                ForeColor = textMuted
            };
            balatroCard.Controls.Add(lblBalatroStatus);
            contentPanel.Controls.Add(balatroCard);

            // Status Bar & Progress
            progressBar = new ProgressBar
            {
                Location = new Point(0, 260),
                Size = new Size(616, 10),
                Style = ProgressBarStyle.Continuous,
                Visible = false
            };
            contentPanel.Controls.Add(progressBar);

            lblStatus = new Label
            {
                Location = new Point(0, 278),
                Size = new Size(616, 75),
                Font = new Font("Segoe UI", 9.5f, FontStyle.Regular),
                ForeColor = textMuted,
                TextAlign = ContentAlignment.TopCenter
            };
            contentPanel.Controls.Add(lblStatus);

            this.Controls.Add(contentPanel);

            // Bottom Actions Panel
            Panel bottomPanel = new Panel
            {
                Dock = DockStyle.Bottom,
                Height = 70,
                BackColor = Color.FromArgb(18, 20, 24)
            };

            btnUninstall = CreateButton("Uninstall", 24, 18, 120, 36, Color.FromArgb(40, 44, 54), textMuted, cardBorder);
            btnUninstall.Click += BtnUninstall_Click;
            bottomPanel.Controls.Add(btnUninstall);

            btnInstall = CreateButton("Install Mod", 470, 16, 170, 38, Color.FromArgb(220, 53, 69), Color.White, Color.Transparent);
            btnInstall.Font = new Font("Segoe UI", 10.5f, FontStyle.Bold);
            btnInstall.Click += BtnInstall_Click;
            bottomPanel.Controls.Add(btnInstall);

            this.Controls.Add(bottomPanel);
        }

        private Panel CreateCard(int x, int y, int w, int h, string title)
        {
            Panel p = new Panel
            {
                Location = new Point(x, y),
                Size = new Size(w, h),
                BackColor = cardBg
            };

            Label lbl = new Label
            {
                Text = title,
                Font = new Font("Segoe UI", 10.5f, FontStyle.Bold),
                ForeColor = Color.FromArgb(240, 240, 245),
                Location = new Point(14, 8),
                AutoSize = true
            };
            p.Controls.Add(lbl);

            p.Paint += (s, e) =>
            {
                ControlPaint.DrawBorder(e.Graphics, p.ClientRectangle,
                    cardBorder, 1, ButtonBorderStyle.Solid,
                    cardBorder, 1, ButtonBorderStyle.Solid,
                    cardBorder, 1, ButtonBorderStyle.Solid,
                    cardBorder, 1, ButtonBorderStyle.Solid);
            };

            return p;
        }

        private Button CreateButton(string text, int x, int y, int w, int h, Color bg, Color fg, Color border)
        {
            Button b = new Button
            {
                Text = text,
                Location = new Point(x, y),
                Size = new Size(w, h),
                BackColor = bg,
                ForeColor = fg,
                FlatStyle = FlatStyle.Flat,
                Cursor = Cursors.Hand
            };
            if (border == Color.Transparent || border.A == 0)
            {
                b.FlatAppearance.BorderSize = 0;
            }
            else
            {
                b.FlatAppearance.BorderColor = border;
                b.FlatAppearance.BorderSize = 1;
            }
            return b;
        }

        private static bool HasGtaExe(string dir)
        {
            if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir))
                return false;
            return File.Exists(Path.Combine(dir, "GTA5.exe")) ||
                   File.Exists(Path.Combine(dir, "GTA5_Enhanced.exe"));
        }

        private static string GetGtaExeName(string dir)
        {
            if (!string.IsNullOrEmpty(dir) && Directory.Exists(dir))
            {
                if (File.Exists(Path.Combine(dir, "GTA5_Enhanced.exe")))
                    return "GTA5_Enhanced.exe";
                if (File.Exists(Path.Combine(dir, "GTA5.exe")))
                    return "GTA5.exe";
            }
            return "GTA5.exe";
        }

        private void AutoDetectPaths()
        {
            // 1. Detect GTA V
            string detectedGta = DetectGtaPath();
            if (!string.IsNullOrEmpty(detectedGta))
                txtGtaPath.Text = detectedGta;

            // 2. Detect Balatro
            string detectedBalatro = DetectBalatroPath();
            if (!string.IsNullOrEmpty(detectedBalatro))
                txtBalatroExe.Text = detectedBalatro;

            ValidatePaths();
        }

        private string DetectGtaPath()
        {
            // Check current working directory or adjacent folders first
            string currentDir = AppDomain.CurrentDomain.BaseDirectory;
            if (HasGtaExe(currentDir))
                return currentDir;
            if (Directory.GetParent(currentDir) != null && HasGtaExe(Directory.GetParent(currentDir).FullName))
                return Directory.GetParent(currentDir).FullName;

            // Common Steam and installation paths (Enhanced edition prioritized, then Standard)
            string[] searchPaths = new string[]
            {
                @"D:\SteamLibrary\steamapps\common\Grand Theft Auto V Enhanced",
                @"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto V Enhanced",
                @"E:\SteamLibrary\steamapps\common\Grand Theft Auto V Enhanced",
                @"F:\SteamLibrary\steamapps\common\Grand Theft Auto V Enhanced",
                @"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto V",
                @"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto V Balatro test",
                @"D:\SteamLibrary\steamapps\common\Grand Theft Auto V",
                @"E:\SteamLibrary\steamapps\common\Grand Theft Auto V",
                @"F:\SteamLibrary\steamapps\common\Grand Theft Auto V",
                @"D:\Games\Grand Theft Auto V",
                @"C:\Program Files\Epic Games\GTAV",
                @"C:\Program Files\Rockstar Games\Grand Theft Auto V"
            };

            foreach (var p in searchPaths)
            {
                if (HasGtaExe(p))
                    return p;
            }

            // Steam libraryfolders.vdf dynamic detection
            try
            {
                string steamPath = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string;
                if (string.IsNullOrEmpty(steamPath))
                    steamPath = Registry.GetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\Valve\Steam", "InstallPath", null) as string;

                if (!string.IsNullOrEmpty(steamPath))
                {
                    string vdfPath = Path.Combine(steamPath, @"steamapps\libraryfolders.vdf");
                    if (File.Exists(vdfPath))
                    {
                        string vdfText = File.ReadAllText(vdfPath);
                        MatchCollection matches = Regex.Matches(vdfText, "\"path\"\\s+\"([^\"]+)\"");
                        foreach (Match m in matches)
                        {
                            string libPath = m.Groups[1].Value.Replace(@"\\", @"\");
                            string enh = Path.Combine(libPath, @"steamapps\common\Grand Theft Auto V Enhanced");
                            if (HasGtaExe(enh)) return enh;
                            string std = Path.Combine(libPath, @"steamapps\common\Grand Theft Auto V");
                            if (HasGtaExe(std)) return std;
                        }
                    }
                }
            }
            catch { }

            // Registry checks
            try
            {
                using (RegistryKey key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\WOW6432Node\Rockstar Games\Grand Theft Auto V"))
                {
                    if (key != null)
                    {
                        string val = key.GetValue("InstallFolder") as string;
                        if (!string.IsNullOrEmpty(val) && HasGtaExe(val))
                            return val;
                    }
                }
            }
            catch { }

            return "";
        }

        private string DetectBalatroPath()
        {
            string[] searchPaths = new string[]
            {
                @"C:\Program Files (x86)\Steam\steamapps\common\Balatro\Balatro.exe",
                @"D:\SteamLibrary\steamapps\common\Balatro\Balatro.exe",
                @"E:\SteamLibrary\steamapps\common\Balatro\Balatro.exe",
                @"F:\SteamLibrary\steamapps\common\Balatro\Balatro.exe"
            };

            foreach (var p in searchPaths)
            {
                if (File.Exists(p))
                    return p;
            }

            try
            {
                string steamPath = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string;
                if (string.IsNullOrEmpty(steamPath))
                    steamPath = Registry.GetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\Valve\Steam", "InstallPath", null) as string;

                if (!string.IsNullOrEmpty(steamPath))
                {
                    string vdfPath = Path.Combine(steamPath, @"steamapps\libraryfolders.vdf");
                    if (File.Exists(vdfPath))
                    {
                        string vdfText = File.ReadAllText(vdfPath);
                        MatchCollection matches = Regex.Matches(vdfText, "\"path\"\\s+\"([^\"]+)\"");
                        foreach (Match m in matches)
                        {
                            string libPath = m.Groups[1].Value.Replace(@"\\", @"\");
                            string bal = Path.Combine(libPath, @"steamapps\common\Balatro\Balatro.exe");
                            if (File.Exists(bal)) return bal;
                        }
                    }
                }
            }
            catch { }

            return "";
        }

        private void ValidatePaths()
        {
            bool gtaValid = false;
            bool balatroValid = false;

            string gtaDir = txtGtaPath.Text.Trim();
            if (HasGtaExe(gtaDir))
            {
                gtaValid = true;
                string exeName = GetGtaExeName(gtaDir);
                bool isEnhanced = exeName.Equals("GTA5_Enhanced.exe", StringComparison.OrdinalIgnoreCase);
                string editionLabel = isEnhanced ? "GTA V Enhanced" : "GTA V";

                bool hasShv = File.Exists(Path.Combine(gtaDir, "ScriptHookV.dll"));
                bool hasReShade = File.Exists(Path.Combine(gtaDir, "dxgi.dll")) ||
                                  File.Exists(Path.Combine(gtaDir, "d3d11.dll")) ||
                                  File.Exists(Path.Combine(gtaDir, "ReShade64.asi")) ||
                                  File.Exists(Path.Combine(gtaDir, "ReShade64.dll"));

                if (hasShv && hasReShade)
                {
                    lblGtaStatus.Text = string.Format("✓ {0} found ({1}). ScriptHookV & ReShade detected.", editionLabel, exeName);
                    lblGtaStatus.ForeColor = Color.FromArgb(75, 210, 120);
                }
                else if (!hasShv && !hasReShade)
                {
                    lblGtaStatus.Text = string.Format("⚠ {0} found, but ScriptHookV and ReShade are missing!", editionLabel);
                    lblGtaStatus.ForeColor = Color.FromArgb(255, 193, 7);
                }
                else if (!hasShv)
                {
                    lblGtaStatus.Text = string.Format("⚠ {0} found, but ScriptHookV.dll is missing! (Required for ASI plugins)", exeName);
                    lblGtaStatus.ForeColor = Color.FromArgb(255, 193, 7);
                }
                else
                {
                    lblGtaStatus.Text = string.Format("⚠ {0} found with ScriptHookV, but ReShade is missing! (Required for video passthrough)", editionLabel);
                    lblGtaStatus.ForeColor = Color.FromArgb(255, 193, 7);
                }
            }
            else
            {
                lblGtaStatus.Text = string.IsNullOrEmpty(gtaDir) ? "Please select your GTA V folder." : "✗ GTA5.exe or GTA5_Enhanced.exe not found in this folder.";
                lblGtaStatus.ForeColor = string.IsNullOrEmpty(gtaDir) ? textMuted : Color.FromArgb(255, 100, 100);
            }

            string balatroExe = txtBalatroExe.Text.Trim();
            if (File.Exists(balatroExe) && Path.GetFileName(balatroExe).Equals("Balatro.exe", StringComparison.OrdinalIgnoreCase))
            {
                balatroValid = true;
                string balatroDir = Path.GetDirectoryName(balatroExe);
                bool hasLovely = File.Exists(Path.Combine(balatroDir, "version.dll"));

                if (hasLovely)
                {
                    lblBalatroStatus.Text = "✓ Balatro.exe found. Lovely mod loader detected.";
                    lblBalatroStatus.ForeColor = Color.FromArgb(75, 210, 120);
                }
                else
                {
                    lblBalatroStatus.Text = "✓ Balatro.exe found. (Note: Make sure Lovely mod loader is installed)";
                    lblBalatroStatus.ForeColor = Color.FromArgb(255, 193, 7);
                }
            }
            else
            {
                lblBalatroStatus.Text = string.IsNullOrEmpty(balatroExe) ? "Please select your Balatro.exe file." : "✗ File is not a valid Balatro.exe.";
                lblBalatroStatus.ForeColor = string.IsNullOrEmpty(balatroExe) ? textMuted : Color.FromArgb(255, 100, 100);
            }

            btnInstall.Enabled = gtaValid && balatroValid;
            btnInstall.BackColor = btnInstall.Enabled ? accentColor : Color.FromArgb(60, 64, 75);
            btnInstall.Cursor = btnInstall.Enabled ? Cursors.Hand : Cursors.Default;
        }

        private void BtnBrowseGta_Click(object sender, EventArgs e)
        {
            using (FolderBrowserDialog fbd = new FolderBrowserDialog())
            {
                fbd.Description = "Select your Grand Theft Auto V installation folder:";
                if (Directory.Exists(txtGtaPath.Text))
                    fbd.SelectedPath = txtGtaPath.Text;
                if (fbd.ShowDialog() == DialogResult.OK)
                {
                    txtGtaPath.Text = fbd.SelectedPath;
                    ValidatePaths();
                }
            }
        }

        private void BtnBrowseBalatro_Click(object sender, EventArgs e)
        {
            using (OpenFileDialog ofd = new OpenFileDialog())
            {
                ofd.Filter = "Balatro Executable (Balatro.exe)|Balatro.exe|All Executables (*.exe)|*.exe";
                ofd.Title = "Select Balatro.exe";
                if (!string.IsNullOrEmpty(txtBalatroExe.Text) && Directory.Exists(Path.GetDirectoryName(txtBalatroExe.Text)))
                    ofd.InitialDirectory = Path.GetDirectoryName(txtBalatroExe.Text);
                if (ofd.ShowDialog() == DialogResult.OK)
                {
                    txtBalatroExe.Text = ofd.FileName;
                    ValidatePaths();
                }
            }
        }

        private void BtnInstall_Click(object sender, EventArgs e)
        {
            string gtaDir = txtGtaPath.Text.Trim();
            string balatroExe = txtBalatroExe.Text.Trim();

            if (!HasGtaExe(gtaDir))
            {
                MessageBox.Show("Please select a valid GTA V installation directory (containing GTA5.exe or GTA5_Enhanced.exe).", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            if (!File.Exists(balatroExe))
            {
                MessageBox.Show("Please select a valid Balatro.exe path.", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            string baseDir = AppDomain.CurrentDomain.BaseDirectory;
            string modFilesDir = Path.Combine(baseDir, "mod_files");

            if (!Directory.Exists(modFilesDir))
            {
                if (Directory.Exists(Path.Combine(baseDir, "bin")))
                    modFilesDir = Path.Combine(baseDir, "bin");
                else if (Directory.Exists(Path.Combine(baseDir, @"..\bin")))
                    modFilesDir = Path.GetFullPath(Path.Combine(baseDir, @"..\bin"));
                else
                {
                    MessageBox.Show("Missing 'bin' or 'mod_files' folder! Please keep Installer.exe inside the repository or unpacked release folder.", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }
            }

            try
            {
                progressBar.Visible = true;
                progressBar.Value = 10;
                lblStatus.Text = "Installing GTA V mod files...";
                lblStatus.ForeColor = textWhite;
                Application.DoEvents();

                // 1. Copy GTA V files
                string srcAsi = Path.Combine(modFilesDir, @"gta\BalatroPassthrough.asi");
                string srcIni = Path.Combine(modFilesDir, @"gta\BalatroPassthrough.ini");
                string srcFx = Path.Combine(modFilesDir, @"gta\reshade-shaders\Shaders\BalatroPhone.fx");

                File.Copy(srcAsi, Path.Combine(gtaDir, "BalatroPassthrough.asi"), true);
                progressBar.Value = 35;

                // Configure BalatroPassthrough.ini with exact path to Balatro.exe
                string iniContent = File.ReadAllText(srcIni);
                iniContent = Regex.Replace(iniContent, @"ExecutablePath=.*", "ExecutablePath=" + balatroExe);
                File.WriteAllText(Path.Combine(gtaDir, "BalatroPassthrough.ini"), iniContent);
                progressBar.Value = 50;

                // Copy ReShade shader
                string targetShaderDir = Path.Combine(gtaDir, @"reshade-shaders\Shaders");
                if (!Directory.Exists(targetShaderDir))
                    Directory.CreateDirectory(targetShaderDir);
                File.Copy(srcFx, Path.Combine(targetShaderDir, "BalatroPhone.fx"), true);

                // Ensure ReShadePreset.ini includes BalatroPhone technique
                string presetPath = Path.Combine(gtaDir, "ReShadePreset.ini");
                try
                {
                    if (File.Exists(presetPath))
                    {
                        string presetText = File.ReadAllText(presetPath);
                        if (!presetText.Contains("BalatroPhone@BalatroPhone.fx"))
                        {
                            if (presetText.Contains("Techniques="))
                                presetText = Regex.Replace(presetText, @"Techniques=(.*)", "Techniques=BalatroPhone@BalatroPhone.fx,$1");
                            else
                                presetText += "\r\nTechniques=BalatroPhone@BalatroPhone.fx\r\n";

                            if (presetText.Contains("TechniqueSorting="))
                                presetText = Regex.Replace(presetText, @"TechniqueSorting=(.*)", "TechniqueSorting=BalatroPhone@BalatroPhone.fx,$1");
                            else
                                presetText += "TechniqueSorting=BalatroPhone@BalatroPhone.fx\r\n";

                            File.WriteAllText(presetPath, presetText.Trim() + "\r\n");
                        }
                    }
                    else
                    {
                        File.WriteAllText(presetPath, "Techniques=BalatroPhone@BalatroPhone.fx\r\nTechniqueSorting=BalatroPhone@BalatroPhone.fx\r\n");
                    }
                }
                catch { }

                progressBar.Value = 65;

                // 2. Copy Balatro Mod files
                lblStatus.Text = "Installing Balatro bridge files into %APPDATA%...";
                Application.DoEvents();

                string appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
                string balatroModDir = Path.Combine(appData, @"Balatro\Mods\BalatroPassthrough");
                if (!Directory.Exists(balatroModDir))
                    Directory.CreateDirectory(balatroModDir);

                string srcLua = Path.Combine(modFilesDir, @"balatro\BalatroPassthrough\balatro_bridge.lua");
                string srcLovely = Path.Combine(modFilesDir, @"balatro\BalatroPassthrough\lovely.toml");

                File.Copy(srcLua, Path.Combine(balatroModDir, "balatro_bridge.lua"), true);
                File.Copy(srcLovely, Path.Combine(balatroModDir, "lovely.toml"), true);
                progressBar.Value = 100;

                lblStatus.Text = "🎉 Installation Complete!\nPress F9 inside GTA V to launch and open the phone.\nPress F10 to cycle Small Phone, Big Phone, and Fullscreen modes.";
                lblStatus.ForeColor = Color.FromArgb(75, 230, 130);

                MessageBox.Show(
                    "Balatro in GTA V has been successfully installed!\n\n" +
                    "HOW TO PLAY:\n" +
                    "1. Launch Grand Theft Auto V.\n" +
                    "2. Press F9 to pull up the in-game phone (Balatro launches automatically).\n" +
                    "3. Press F10 to toggle between Small Phone, Big Phone, and Fullscreen modes.\n" +
                    "4. Use your mouse to click and drag cards.\n" +
                    "5. Press F9 again to close the phone at any time.",
                    "Installation Successful", MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            catch (Exception ex)
            {
                lblStatus.Text = "Installation failed: " + ex.Message;
                lblStatus.ForeColor = Color.FromArgb(255, 80, 80);
                MessageBox.Show("An error occurred during installation:\n" + ex.Message, "Installation Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private void BtnUninstall_Click(object sender, EventArgs e)
        {
            string gtaDir = txtGtaPath.Text.Trim();
            if (string.IsNullOrEmpty(gtaDir) || !Directory.Exists(gtaDir))
            {
                MessageBox.Show("Please select your GTA V installation folder first.", "Uninstall", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            if (MessageBox.Show("Are you sure you want to uninstall Balatro in GTA V?", "Confirm Uninstall", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
                return;

            try
            {
                // Delete GTA V mod files
                string asiPath = Path.Combine(gtaDir, "BalatroPassthrough.asi");
                string iniPath = Path.Combine(gtaDir, "BalatroPassthrough.ini");
                string fxPath = Path.Combine(gtaDir, @"reshade-shaders\Shaders\BalatroPhone.fx");

                if (File.Exists(asiPath)) File.Delete(asiPath);
                if (File.Exists(iniPath)) File.Delete(iniPath);
                if (File.Exists(fxPath)) File.Delete(fxPath);

                // Delete Balatro mod files
                string appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
                string balatroModDir = Path.Combine(appData, @"Balatro\Mods\BalatroPassthrough");
                if (Directory.Exists(balatroModDir))
                    Directory.Delete(balatroModDir, true);

                lblStatus.Text = "Mod uninstalled successfully.";
                lblStatus.ForeColor = textWhite;
                MessageBox.Show("Balatro in GTA V has been removed.", "Uninstalled", MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            catch (Exception ex)
            {
                MessageBox.Show("Error during uninstall: " + ex.Message, "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        [STAThread]
        public static void Main()
        {
            try
            {
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.Run(new InstallerForm());
            }
            catch (Exception ex)
            {
                MessageBox.Show("Installer encountered an error:\n\n" + ex.ToString(),
                                "Balatro in GTA V Setup Error",
                                MessageBoxButtons.OK,
                                MessageBoxIcon.Error);
            }
        }
    }
}
