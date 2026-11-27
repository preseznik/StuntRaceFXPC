using System.Text.Json;

namespace StuntRaceFX.Launcher;

internal sealed partial class MainForm : Form
{
    private sealed record CarChoice(string Id, string Name) { public override string ToString() => Name; }
    private sealed record NumberChoice(int Value, string Label) { public override string ToString() => Label; }
    private static ComboBox Choices(string name, string label, string unit, string automatic, params int[] values)
    {
        var box = new ComboBox { Name = name, AccessibleName = label, DropDownStyle = ComboBoxStyle.DropDownList, Width = 210 };
        foreach (var number in values) box.Items.Add(new NumberChoice(number, number == 0 ? automatic : $"{number}{unit}"));
        box.SelectedIndex = 0;
        return box;
    }
    private static int SelectedNumber(ComboBox box) => ((NumberChoice)box.SelectedItem!).Value;
    private static void RestoreNumber(ComboBox box, int value) =>
        box.SelectedItem = box.Items.Cast<NumberChoice>().FirstOrDefault(choice => choice.Value == value) ?? box.Items[0];
    private readonly TextBox rom = new() { Dock = DockStyle.Fill, AccessibleName = "Game ROM" };
    private readonly TextBox output = new() { Dock = DockStyle.Fill, AccessibleName = "Build folder" };
    private readonly TextBox log = new() { Dock = DockStyle.Fill, Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Both, WordWrap = false, Font = new Font("Consolas", 9) };
    private readonly Button verify = new() { Text = "Verify ROM", AutoSize = true };
    private readonly Button build = new() { Text = "Build game", AutoSize = true };
    private readonly Button launch = new() { Name = "LaunchGame", Text = "Launch Game", AutoSize = true, Padding = new Padding(18, 5, 18, 5) };
    private readonly LaunchModePicker launchMode = new();
    private readonly ComboBox displayTarget = new() { Name = "DisplayTarget", DropDownStyle = ComboBoxStyle.DropDownList, Width = 116, AccessibleName = "Display", Anchor = AnchorStyles.Left };
    private readonly Label vrCourseHeading = Hint("Course", 2, 8);
    private readonly Label desktopCourseHeading = Hint("Course", 2);
    private readonly Label desktopCarHeading = Hint("Your car", 2, 8);
    private readonly ComboBox visualStyle = new() { Name = "VisualStyle", AccessibleName = "Visual style", DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Top };
    private RaceStyle preferredStyle, normalStyle;
    private readonly Label styleHeading = Hint("Visual style", 2, 8);
    private readonly Button inspect = new() { Text = "Inspect models", AutoSize = true };
    private readonly Button controller = new() { Name = "ConfigureController", Text = "Configure controller…", AutoSize = true };
    private readonly ComboBox car = new() { Name = "PlayerCar", DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Top, AccessibleName = "Player car" };
    private readonly ComboBox course = new() { Name = "Course", DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Top, AccessibleName = "Course" };
    private readonly Label vrCarHeading = Hint("Your car", 2, 8);
    private readonly ComboBox vrCar = new() { Name = "VrCar", DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Top, AccessibleName = "VR player car" };
    private readonly ComboBox vrCourse = new() { Name = "VrCourse", DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Top, AccessibleName = "VR course" };
    private readonly ComboBox resolution = new() { Name = "Resolution", DropDownStyle = ComboBoxStyle.DropDownList, Width = 210, AccessibleName = "Window resolution" };
    private readonly CheckBox customContent = new() { Name = "CustomContent", Text = "Custom content (Quick Race and multiplayer)", AutoSize = true };
    private readonly CheckBox enhancedPhysics = new() { Name = "EnhancedPhysics", Text = "Enhanced physics", AutoSize = true };
    private readonly Button practice = new() { Name = "StartPractice", Text = "Start practice (4WD Test Run)", AutoSize = true };
    private readonly CheckBox hideRaceHud = new() { Name = "HideRaceHud", Text = "Hide HUD during races (all styles and VR)", AutoSize = true };
    private readonly CheckBox classicSnes = new() { Name = "ClassicSnes", Text = "Render at SNES resolution (256 × 224, 4:3)", AutoSize = true };
    private readonly CheckBox n64LowRaster = new() { Name = "N64LowRaster", Text = "Render N64 mode at 320 × 240 (4:3)", AutoSize = true, Checked = true };
    private readonly ComboBox graphicsProfile = new() { Name = "GraphicsProfile", AccessibleName = "Graphics profile", DropDownStyle = ComboBoxStyle.DropDownList, Width = 210 };
    private readonly ComboBox timeOfDay = new() { Name = "TimeOfDay", AccessibleName = "Time of day", DropDownStyle = ComboBoxStyle.DropDownList, Width = 210 };
    private readonly ComboBox msaa = Choices("RemasteredMsaa", "Anti-aliasing", "× MSAA", "Off", 0, 2, 4, 8);
    private readonly ComboBox weather = new() { Name = "RemasteredWeather", AccessibleName = "Weather", DropDownStyle = ComboBoxStyle.DropDownList, Width = 210 };
    private readonly ComboBox camera = new() { Name = "RemasteredCamera", AccessibleName = "Starting camera", DropDownStyle = ComboBoxStyle.DropDownList, Width = 210 };
    private readonly CheckBox roughness = new() { Name = "MaterialRoughness", Text = "Material roughness", AutoSize = true };
    private readonly CheckBox volumetricLighting = new() { Name = "VolumetricLighting", Text = "Volumetric lighting", AutoSize = true };
    private readonly CheckBox wetTireSpray = new() { Name = "WetTireSpray", Text = "Wet-weather tire spray", AutoSize = true };
    private readonly CheckBox sceneryDetail = new() { Name = "HighDetailScenery", Text = "High-detail roadside scenery (King's Forest)", AutoSize = true };
    private readonly CheckBox roadDetail = new() { Name = "HighDetailRoad", Text = "High-detail road surfaces", AutoSize = true };
    private readonly CheckBox clearCoat = new() { Name = "CarClearCoat", Text = "Car clear coat", AutoSize = true };
    private readonly CheckBox indirect = new() { Name = "RayTracedIndirect", Text = "Ray-traced indirect lighting", AutoSize = true };
    private readonly CheckBox rayReflections = new() { Name = "RayTracedReflections", Text = "Ray-traced wet-road reflections", AutoSize = true };
    private readonly TrackBar shadowQuality = new() { Name = "ShadowQuality", AccessibleName = "Ray-traced shadow quality",
        Minimum = 0, Maximum = 2, Value = 2, LargeChange = 1, Width = 210 };
    private readonly Label shadowQualityLabel = Hint("Shadow quality: High (4 rays)");
    private readonly CheckBox rayShadows = new() { Name = "RayTracedShadows", Text = "Ray-traced scenery shadows", AutoSize = true };
    private readonly CheckBox cameraLook = new() { Name = "CameraLook", Text = "Mouse and right stick camera control", AutoSize = true };
    private readonly TrackBar cameraDistance = new() { Name = "CameraDistance", AccessibleName = "Maximum camera zoom distance in metres",
        Minimum = GameContent.MinCameraDistance, Maximum = GameContent.MaxCameraDistance, Value = GameContent.DefaultCameraDistance,
        LargeChange = 10, TickFrequency = 10, Width = 210 };
    private readonly Label lightingHint = Hint("Local track time stays fixed during the race.");
    private readonly Dictionary<string, DisplaySettings> profiles = new();
    private string? editingProfile;
    private bool n64RasterAvailable, classicRasterAvailable;
    private readonly ComboBox windowMode = new() { Name = "WindowMode", DropDownStyle = ComboBoxStyle.DropDownList, Width = 210, AccessibleName = "Window mode" };
    private readonly ComboBox desktopFps = Choices("DesktopFps", "Desktop driving frame limit", " FPS", "Display sync", 0, 30, 60, 90, 120, 144);
    private readonly ComboBox vrRefresh = Choices("VrRefresh", "Preferred headset refresh", " Hz", "Runtime default", 0, 72, 80, 90, 120, 144);
    private readonly CheckBox smoothVrMotion = new() { Name = "SmoothVrMotion", Text = "Smooth Quick Race vehicle motion", AutoSize = true, Checked = true };
    private readonly ComboBox vrRuntime = new() { Name = "VrRuntime", AccessibleName = "OpenXR runtime", DropDownStyle = ComboBoxStyle.DropDownList, Width = 260 };
    private readonly Label lastVrSession = Hint("");
    private readonly ComboBox vrScale = Choices("VrScale", "VR eye resolution", "%", "", 100, 85, 75, 60);
    private readonly Button cancel = new() { Text = "Cancel build", AutoSize = true, Enabled = false };
    private readonly Label status = new() { Text = "Select your ROM in Game files to begin.", AutoSize = true, Dock = DockStyle.Fill };
    private readonly Label session = new() { AutoSize = true, Dock = DockStyle.Top, Margin = new Padding(0, 16, 0, 0) };
    private readonly Label availability = Hint("");
    private string courseTitle = "Easy Ride";
    private readonly RaceBanner preview = new() { Hero = true };
    private readonly TabControl tabs = new LauncherTabs { Name = "LauncherTabs", Alignment = TabAlignment.Left, SizeMode = TabSizeMode.Fixed, ItemSize = new Size(48, 122), Dock = DockStyle.Fill, Padding = new Point(16, 8) };
    private readonly ProgressBar progress = new() { Dock = DockStyle.Fill, Height = 6, Style = ProgressBarStyle.Blocks };
    private readonly string settingsFile;
    private CancellationTokenSource? active;
    private bool closeAfterCancel;
    private bool classicDefault, switchingInterface;
    private string? previewPath;
    private string VrCarId => ((CarChoice)vrCar.SelectedItem!).Id;
    private string CarId => ((CarChoice)car.SelectedItem!).Id;
    private string CourseId => ((GameCourse)course.SelectedItem!).Id;
    private GameCourse SelectedVrCourse => (GameCourse)vrCourse.SelectedItem!;
    private bool NormalVr => VrSelected && !QuickRaceSelected;
    private bool VrSelected => displayTarget.SelectedIndex == 1;
    private string LaunchCourse => !QuickRaceSelected ? GameContent.DefaultCourse : VrSelected ? SelectedVrCourse.Id : CourseId;
    private RaceStyle vrStyle;
    private RaceStyle SelectedStyle => (RaceStyle)visualStyle.SelectedIndex;
    private bool N64ModeSelected => SelectedStyle == RaceStyle.N64;
    private bool RemasteredSelected => SelectedStyle == RaceStyle.Remastered;
    private string LaunchProfile => VrSelected ? "VR" : QuickRaceSelected || SelectedStyle != RaceStyle.Original ? visualStyle.Text : "Normal";
    private bool QuickRaceSelected => launchMode.SelectedMode == "Quick Race";

    public MainForm(string? settingsPath = null)
    {
        settingsFile = settingsPath ?? Path.Combine(LauncherUpdate.Home, "launcher-settings.json");
        Text = "Stunt Race FX PC";
        ClientSize = new Size(1100, 800); MinimumSize = new Size(820, 620);
        StartPosition = FormStartPosition.CenterScreen; Font = new Font("Segoe UI", 10); AutoScaleMode = AutoScaleMode.Dpi;
        BuildShell();
        displayTarget.Items.AddRange(["Desktop", "VR"]); displayTarget.SelectedIndex = 0;
        string[] styles = ["Classic", "N64 Mode", "Remastered"];
        visualStyle.Items.AddRange(styles); visualStyle.SelectedIndex = 0;
        graphicsProfile.FormattingEnabled = true;
        graphicsProfile.Format += (_, e) => e.Value = e.ListItem switch {
            "Normal" => "Desktop · Full game (Classic)", "VR" => "VR · Selected visual style",
            "Classic" => "Desktop · Quick Race (Classic)", var name => $"Desktop · {name}"
        };
        graphicsProfile.Items.AddRange(["Normal", .. styles, "VR"]);
        car.Items.AddRange([new CarChoice("4wd", "4WD"), new CarChoice("coupe", "COUPE"), new CarChoice("f-type", "F-TYPE")]); car.SelectedIndex = 0;
        vrCar.Items.AddRange(car.Items.Cast<object>().ToArray()); vrCar.SelectedIndex = 0;
        course.Items.AddRange(GameContent.Courses.Cast<object>().ToArray()); course.SelectedIndex = 0;
        vrCourse.Items.AddRange(GameContent.Courses); vrCourse.SelectedIndex = 0;
        resolution.Items.AddRange(["1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160"]); resolution.SelectedIndex = 1;
        windowMode.Items.AddRange(["Windowed", "Borderless fullscreen"]); windowMode.SelectedIndex = 0;

        var play = Page("Play");
        var multiplayer = Page("Multiplayer");
        settingsPage = Page("Settings");
        settingsPage.Controls.Add(settingsTabs);
        BuildPlay(play);
        BuildGraphics();
        windowMode.SelectedIndexChanged += (_, _) => { resolution.Enabled = active == null && windowMode.SelectedIndex == 0; RefreshLaunch(); };
        BuildControlsGameplayAndContent();
        filesPage = SettingsPage("Game files");
        var filesStack = Stack(); filesPage.Controls.Add(filesStack);
        var files = LauncherDesign.CardBox("Game files and diagnostics");
        filesStack.Controls.Add(files);
        files.Controls.Add(availability);
        files.Controls.Add(Hint("Select Stunt Race FX USA Rev 1 (.sfc or .smc). Verification, extraction and compilation happen locally. Your ROM is kept unchanged."));
        var fields = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 3, RowCount = 2, Margin = new Padding(0, 8, 0, 14) };
        fields.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 140)); fields.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100)); fields.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        var browseRom = new Button { Text = "Browse…", AutoSize = true };
        browseRom.Click += (_, _) =>
        {
            using var dialog = new OpenFileDialog { Title = "Select Stunt Race FX USA Rev 1", Filter = "SNES ROM (*.sfc;*.smc)|*.sfc;*.smc|All files (*.*)|*.*", CheckFileExists = true };
            if (dialog.ShowDialog(this) == DialogResult.OK) rom.Text = dialog.FileName;
        };
        var browseOutput = new Button { Text = "Browse…", AutoSize = true };
        browseOutput.Click += (_, _) =>
        {
            using var dialog = new FolderBrowserDialog { Description = "Choose a folder for extracted assets and local builds", UseDescriptionForTitle = true };
            if (Directory.Exists(output.Text)) dialog.InitialDirectory = output.Text;
            if (dialog.ShowDialog(this) == DialogResult.OK) output.Text = dialog.SelectedPath;
        };
        fields.Controls.Add(new Label { Text = "Game ROM", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 0); fields.Controls.Add(rom, 1, 0); fields.Controls.Add(browseRom, 2, 0);
        fields.Controls.Add(new Label { Text = "Build folder", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 1); fields.Controls.Add(output, 1, 1); fields.Controls.Add(browseOutput, 2, 1); files.Controls.Add(fields);
        var buttons = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true }; buttons.Controls.AddRange([verify, build, cancel]); files.Controls.Add(buttons);
        files.Controls.Add(Hint("Select ROM → Verify → Build → Launch Game\nFailed or cancelled builds preserve the last successful build.", 0, 18));
        files.Controls.Add(progress);
        var buildStatus = Hint("");
        status.TextChanged += (_, _) => buildStatus.Text = status.Text;
        files.Controls.Add(buildStatus);
        var logCard = LauncherDesign.CardBox("Build log"); filesStack.Controls.Add(logCard);
        var updates = LauncherDesign.CardBox($"Launcher · Build {GameContent.CurrentVersion}"); filesStack.Controls.Add(updates);
        var checkUpdates = new Button { Text = "Check for updates", AutoSize = true };
        updates.Controls.Add(checkUpdates);
        checkUpdates.Click += async (_, _) => await CheckUpdates(true);
        log.Dock = DockStyle.Top; log.Height = 150;
        logCard.Controls.Add(log); logCard.Controls.Add(inspect);

        BuildAppearance();
        networkPanel = new MultiplayerPanel((grid, style) => StartRace(grid, style));
        multiplayer.Controls.Add(networkPanel);
        networkPanel.ConnectionChanged += connected =>
        {
            customContent.Enabled = !connected;
            classicLauncher.Enabled = !connected && active == null;
            RefreshDrivingSettings();
        };
        enhancedPhysics.CheckedChanged += (_, _) => { networkPanel.EnhancedPhysics = enhancedPhysics.Checked; RefreshLaunch(); };
        practice.Click += (_, _) =>
        {
            try { SaveSettings(); using var game = RaceLaunch.StartPractice(CurrentSettings()); status.Text = "Practice launched."; }
            catch (Exception error) { ShowError(error); }
        };
        customContent.CheckedChanged += (_, _) =>
        {
            void SetChoice(ComboBox box, object choice)
            {
                if (customContent.Checked) box.Items.Add(choice);
                else {
                    if (Equals(box.SelectedItem, choice)) box.SelectedIndex = 0;
                    box.Items.Remove(choice);
                }
            }
            foreach (var id in GameContent.CustomCars) {
                var choice = new CarChoice(id, GameContent.DisplayCar(id));
                SetChoice(car, choice); SetChoice(vrCar, choice);
            }
            foreach (var track in GameContent.CustomCourses) {
                SetChoice(course, track); SetChoice(vrCourse, track);
            }
            networkPanel.SetCustomContent(customContent.Checked);
            RefreshLaunch();
        };
        tabs.SelectedIndexChanged += (_, _) =>
        {
            bool selected = tabs.SelectedTab == multiplayer;
            AcceptButton = tabs.SelectedTab == play ? launch : null;
            networkPanel.SetTabOpen(selected);
        };

        verify.Click += async (_, _) => await ExecuteAsync(false, fields);
        build.Click += async (_, _) => await ExecuteAsync(true, fields);
        cancel.Click += (_, _) => { active?.Cancel(); status.Text = "Cancelling the active build…"; };
        launch.Click += (_, _) => Launch();
        inspect.Click += (_, _) => { try { using var process = BuildPipeline.Launch(output.Text, inspectModels: true); } catch (Exception error) { ShowError(error); } };
        controller.Click += (_, _) =>
        {
            try
            {
                using var dialog = new ControllerDialog(ControllerProfile.FilePath(output.Text));
                design.Apply(dialog);
                if (dialog.ShowDialog(this) == DialogResult.OK) AppendLog("Controller settings saved. They apply on the next game launch.");
            }
            catch (Exception error) { ShowError(error); }
        };
        output.TextChanged += (_, _) => RefreshLaunch();
        graphicsProfile.SelectedIndexChanged += (_, _) => {
            SelectGraphicsProfile();
            RefreshLaunch();
        };
        graphicsProfile.SelectedIndex = 0;
        launchMode.ModeChanged += (_, _) => SelectRaceProfile();
        displayTarget.SelectedIndexChanged += (_, _) => SelectRaceProfile();
        course.SelectedIndexChanged += (_, _) => SelectRaceProfile();
        car.SelectedIndexChanged += (_, _) => SelectRaceProfile();
        visualStyle.SelectedIndexChanged += (_, _) => {
            if (VrSelected) vrStyle = SelectedStyle;
            else if (QuickRaceSelected && GameContent.SupportsEnhancedStyles(CourseId, CarId)) preferredStyle = SelectedStyle;
            else if (!QuickRaceSelected) normalStyle = SelectedStyle;
            graphicsProfile.SelectedItem = LaunchProfile;
            RefreshLaunch();
        };
        rayShadows.CheckedChanged += (_, _) => RefreshLaunch();
        camera.SelectedIndexChanged += (_, _) => RefreshLaunch();
        vrCar.SelectedIndexChanged += (_, _) => RefreshLaunch();
        vrCourse.SelectedIndexChanged += (_, _) => RefreshLaunch();
        FormClosing += (_, args) => { if (active != null) { args.Cancel = true; closeAfterCancel = true; active.Cancel(); } else if (!switchingInterface) SaveSettings(); };
        LoadSettings(); ApplyLook(look); AcceptButton = launch;
        Load += (_, _) => {
            if (savedWindowSize is not { IsValid: true } size) return;
            var area = Screen.FromControl(this).WorkingArea;
            double scale = DeviceDpi / 96.0;
            Size = new Size((int)Math.Round(Math.Clamp(size.Width * scale, MinimumSize.Width, Math.Max(MinimumSize.Width, area.Width))),
                            (int)Math.Round(Math.Clamp(size.Height * scale, MinimumSize.Height, Math.Max(MinimumSize.Height, area.Height))));
            CenterToScreen();
            if (size.Maximized) WindowState = FormWindowState.Maximized;
        };
        Resize += (_, _) => { if (WindowState != FormWindowState.Minimized) maximized = WindowState == FormWindowState.Maximized; };
        Shown += (_,_) => { if (tabs.SelectedTab == play && launch.Enabled) launch.Focus(); };
        Shown += async (_, _) => await CheckUpdates(false);
    }

    private void Launch()
    {
        try
        {
            SaveSettings();
            using var process = StartRace();
            status.Text = N64ModeSelected || RemasteredSelected ? $"{courseTitle} launched." : NormalVr ? "Game launched in OpenXR." : VrSelected ? $"{SelectedVrCourse.Name} launched in OpenXR." : QuickRaceSelected ? $"Quick Race launched — {course.Text}, {car.Text}." : "Game launched.";
            AppendLog(status.Text);
        }
        catch (Exception error) { ShowError(error); }
    }
    private System.Diagnostics.Process StartRace(LanGrid? lan = null, RaceStyle? lanStyle = null)
    {
        return RaceLaunch.Start(CurrentSettings(), lan, lanStyle);

    }

    private TabPage Page(string title) => AddPage(tabs, title);
    private TabPage SettingsPage(string title) => AddPage(settingsTabs, title);
    private static TabPage AddPage(TabControl owner, string title)
    {
        var page = new TabPage(title) { Padding = new Padding(14), BackColor = LauncherTabs.Surface, ForeColor = LauncherTabs.Ink };
        owner.TabPages.Add(page);
        return page;
    }
    private static TableLayoutPanel Stack()
    {
        var panel = new TableLayoutPanel { Dock = DockStyle.Fill, AutoScroll = true, ColumnCount = 1, GrowStyle = TableLayoutPanelGrowStyle.AddRows };
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100)); return panel;
    }
    private static Label Heading(string text, int top = 0) => new() { Text = text, AutoSize = true, Dock = DockStyle.Top, Font = new Font("Segoe UI", 16, FontStyle.Bold), Margin = new Padding(0, top, 0, 14) };
    private static Label Hint(string text, int bottom = 10, int top = 0) => new() { Text = text, AutoSize = true, Dock = DockStyle.Top, Margin = new Padding(0, top, 0, bottom) };

    private async Task ExecuteAsync(bool compile, Control fields)
    {
        if (active != null) return;
        using var cancellation = new CancellationTokenSource();
        active = cancellation;
        classicLauncher.Enabled = false;
        RefreshDrivingSettings();
        fields.Enabled = verify.Enabled = build.Enabled = launch.Enabled = launchMode.Enabled = displayTarget.Enabled = inspect.Enabled = controller.Enabled = resolution.Enabled = car.Enabled = course.Enabled = visualStyle.Enabled = vrCourse.Enabled = vrCar.Enabled = windowMode.Enabled = false;
        cancel.Enabled = true;
        progress.Style = ProgressBarStyle.Marquee;
        status.Text = compile ? "Building native game…" : "Verifying ROM…";
        try
        {
            SaveSettings();
            var uiLog = new Progress<string>(AppendLog);
            var pipeline = new BuildPipeline(BuildPipeline.FindSource(), line => ((IProgress<string>)uiLog).Report(line));
            if (compile)
            {
                await pipeline.BuildAsync(rom.Text, output.Text, cancellation.Token);
                status.Text = "Build validated. Launch Game is ready.";
            }
            else
            {
                await pipeline.VerifyAsync(rom.Text, output.Text, cancellation.Token);
                status.Text = "ROM verified. Ready to build the native game locally.";
            }
        }
        catch (OperationCanceledException) { status.Text = "Cancelled. The last successful build is preserved."; AppendLog(status.Text); }
        catch (Exception error) { ShowError(error); }
        finally
        {
            active = null;
            classicLauncher.Enabled = customContent.Enabled;
            fields.Enabled = verify.Enabled = build.Enabled = launchMode.Enabled = displayTarget.Enabled = car.Enabled = course.Enabled = windowMode.Enabled = true;
            resolution.Enabled = windowMode.SelectedIndex == 0;
            cancel.Enabled = false;
            progress.Style = ProgressBarStyle.Blocks;
            RefreshLaunch();
            if (closeAfterCancel) Close();
        }
    }

    private DisplaySettings ReadDisplay() => new(resolution.Text, windowMode.SelectedIndex == 1, SelectedNumber(desktopFps));

    private DisplaySettings LaunchDisplay() => editingProfile == LaunchProfile
        ? ReadDisplay() : profiles.GetValueOrDefault(LaunchProfile) ?? new DisplaySettings();

    private void SelectRaceProfile()
    {
        var style = QuickRaceSelected ? preferredStyle : normalStyle;
        if (QuickRaceSelected && !GameContent.SupportsEnhancedStyles(CourseId, CarId))
            style = RaceStyle.Original;
        visualStyle.SelectedIndex = (int)(VrSelected ? vrStyle : style);
        graphicsProfile.SelectedItem = LaunchProfile;
        RefreshLaunch();
    }

    private void SelectGraphicsProfile()
    {
        if (graphicsProfile.SelectedIndex < 0 || editingProfile == (string?)graphicsProfile.SelectedItem) return;
        if (editingProfile != null) profiles[editingProfile] = ReadDisplay();
        CaptureQuality();
        editingProfile = (string)graphicsProfile.SelectedItem!;
        LoadQuality(editingProfile == "VR" ? vrQuality : desktopQuality);
        var display = profiles.GetValueOrDefault(editingProfile) ?? new DisplaySettings();
        resolution.SelectedItem = resolution.Items.Contains(display.Resolution) ? display.Resolution : "1920 x 1080";
        windowMode.SelectedIndex = display.Borderless ? 1 : 0;
        RestoreNumber(desktopFps, display.DesktopFps);
    }

    private RemasteredQuality desktopQuality = new(), vrQuality = new();

    private void LoadQuality(RemasteredQuality quality)
    {
        roughness.Checked = quality.MaterialRoughness;
        clearCoat.Checked = quality.CarClearCoat;
        roadDetail.Checked = quality.HighDetailRoad;
        sceneryDetail.Checked = quality.HighDetailScenery;
        wetTireSpray.Checked = quality.WetTireSpray;
        volumetricLighting.Checked = quality.VolumetricLighting;
        indirect.Checked = quality.RayTracedIndirect;
        rayReflections.Checked = quality.RayTracedReflections;
        rayShadows.Checked = quality.RayTracedShadows;
        shadowQuality.Value = quality.ShadowRays switch { 1 => 0, 2 => 1, _ => 2 };
        RestoreNumber(msaa, quality.RemasteredMsaa);
    }

    private void CaptureQuality()
    {
        if (editingProfile is not ("Remastered" or "VR")) return;
        var previous = editingProfile == "VR" ? vrQuality : desktopQuality;
        var edited = new RemasteredQuality(SelectedNumber(msaa), rayShadows.Checked, rayReflections.Checked,
            indirect.Checked, roughness.Checked, clearCoat.Checked, roadDetail.Checked, sceneryDetail.Checked,
            1 << shadowQuality.Value, wetTireSpray.Checked, volumetricLighting.Checked, previous.Preset);
        if (edited == previous) return;
        edited = edited with { Preset = "Custom" };
        if (editingProfile == "VR") vrQuality = edited;
        else desktopQuality = edited;
    }

    private string? wpfTheme;
    private LauncherWindowSize? savedWindowSize;
    private bool maximized;

    private void LoadSettings()
    {
        output.Text = Path.Combine(LauncherUpdate.Home, "user-builds");
        try
        {
            if (!File.Exists(settingsFile)) { RestoreVrRuntime(""); return; }
            using var json = JsonDocument.Parse(File.ReadAllText(settingsFile));
            var settings = json.RootElement.Deserialize<LauncherSettings>();
            if (settings == null) return;
            savedWindowSize = settings.WindowSize;
            classicDefault = settings.ClassicLauncher;
            enhancedPhysics.Checked = settings.EnhancedPhysics;
            wpfTheme = settings.WpfTheme;
            if (Enum.TryParse<LauncherLook>(settings.LauncherTheme, out var savedLook) && Enum.IsDefined(savedLook)) look = savedLook;
            var savedN64Mode = json.RootElement.TryGetProperty(nameof(settings.N64Mode), out _) ? settings.N64Mode
                : json.RootElement.TryGetProperty("Remastered", out var legacyMode) && legacyMode.ValueKind == JsonValueKind.True;
            rom.Text = settings.RomPath;
            if (!string.IsNullOrWhiteSpace(settings.OutputPath)) output.Text = settings.OutputPath;
            customContent.Checked = settings.CustomContent;
            car.SelectedItem = car.Items.Cast<CarChoice>().FirstOrDefault(c => c.Id == settings.Car) ?? car.Items[0];
            course.SelectedItem = course.Items.Cast<GameCourse>().FirstOrDefault(c => c.Id == settings.Course) ?? course.Items[0];
            vrCar.SelectedItem = vrCar.Items.Cast<CarChoice>().FirstOrDefault(c => c.Id == settings.VrCar) ?? vrCar.Items[0];
            vrCourse.SelectedItem = vrCourse.Items.Cast<GameCourse>().FirstOrDefault(c => c.Id == settings.VrCourse) ?? vrCourse.Items[0];
            var fallback = settings.GraphicsProfiles == null
                ? new DisplaySettings(settings.Resolution, settings.Borderless, settings.DesktopFps)
                : new DisplaySettings();
            foreach (string name in graphicsProfile.Items)
            {
                var oldKey = name switch { "Classic" => "Original", "N64 Mode" => "N64 mode", "VR" => "4WD VR", _ => name };
                profiles[name] = settings.GraphicsProfiles?.GetValueOrDefault(name) ?? settings.GraphicsProfiles?.GetValueOrDefault(oldKey)
                    ?? (name == "Classic" ? settings.GraphicsProfiles?.GetValueOrDefault("Quick Race") : null) ?? fallback;
            }
            editingProfile = null;
            var legacyEnhanced = settings.QuickRaceStyle == null && (settings.RemasteredMode || savedN64Mode);
            if (legacyEnhanced)
            {
                // The former standalone modes always launched this combination,
                // even when another car and course were saved for Quick Race.
                car.SelectedIndex = 0; course.SelectedItem = GameContent.Course(GameContent.DefaultCourse);
                preferredStyle = settings.RemasteredMode ? RaceStyle.Remastered : RaceStyle.N64;
            }
            else
                preferredStyle = settings.QuickRaceStyle switch { "Remastered" => RaceStyle.Remastered, "N64" => RaceStyle.N64, _ => RaceStyle.Original };
            normalStyle = settings.NormalStyle switch { "N64" => RaceStyle.N64, "Remastered" => RaceStyle.Remastered, _ => RaceStyle.Original };
            vrStyle = settings.VrStyle switch { "N64" => RaceStyle.N64, "Remastered" => RaceStyle.Remastered, _ => RaceStyle.Original };
            // Standalone enhanced modes took precedence over the old quick VR flag.
            displayTarget.SelectedIndex = settings.NormalVr || (!legacyEnhanced && settings.EasyRideVr) ? 1 : 0;
            launchMode.SelectedMode = !settings.NormalVr && (legacyEnhanced || settings.EasyRideVr || settings.QuickRace) ? "Quick Race" : "Full game";
            SelectRaceProfile();
            hideRaceHud.Checked = settings.HideRaceHud;
            classicSnes.Checked = settings.ClassicSnes;
            n64LowRaster.Checked = settings.N64LowRaster;
            desktopQuality = RemasteredQuality.Read(settings, false);
            vrQuality = RemasteredQuality.Read(settings, true);
            LoadQuality(editingProfile == "VR" ? vrQuality : desktopQuality);
            weather.SelectedIndex = Math.Max(0, Array.IndexOf(GameContent.WeatherIds, settings.RemasteredWeather));
            camera.SelectedIndex = Math.Max(0, Array.IndexOf(GameContent.CameraIds, settings.RemasteredCamera));
            cameraLook.Checked = settings.CameraLook;
            cameraDistance.Value = Math.Clamp(settings.MaxCameraDistance, cameraDistance.Minimum, cameraDistance.Maximum);
            timeOfDay.SelectedIndex = settings.RemasteredHour is >= 0 and < 24 ? settings.RemasteredHour : 12;
            SelectGraphicsProfile();
            smoothVrMotion.Checked = settings.SmoothVrMotion;
            RestoreVrRuntime(OpenXrRuntime.SavedChoice(json.RootElement, settings.VrRuntime));
            RestoreNumber(vrRefresh, settings.VrRefresh); RestoreNumber(vrScale, settings.VrScale);
        }
        catch (Exception error) when (error is IOException or JsonException or UnauthorizedAccessException)
        { AppendLog("Could not load settings: " + error.Message); }
    }

    private LauncherSettings CurrentSettings()
    {
        if (editingProfile != null) profiles[editingProfile] = ReadDisplay();
        var display = LaunchDisplay();
        CaptureQuality();
        var settings = new LauncherSettings(rom.Text, output.Text, CarId, display.Resolution, display.Borderless, CourseId, QuickRaceSelected, VrSelected,
            display.DesktopFps, SelectedNumber(vrRefresh), SelectedNumber(vrScale), SelectedVrCourse.Id, N64ModeSelected, n64LowRaster.Checked, RemasteredSelected, profiles, timeOfDay.SelectedIndex,
            RemasteredWeather: GameContent.WeatherIds[weather.SelectedIndex], RemasteredCamera: GameContent.CameraIds[camera.SelectedIndex],
            CameraLook: cameraLook.Checked, MaxCameraDistance: cameraDistance.Value, QuickRaceStyle: preferredStyle.ToString(), NormalStyle: normalStyle.ToString(),
            NormalVr: NormalVr, VrCar: VrCarId, ClassicSnes: classicSnes.Checked, HideRaceHud: hideRaceHud.Checked,
            VrStyle: vrStyle.ToString(), SmoothVrMotion: smoothVrMotion.Checked, VrRuntime: (vrRuntime.SelectedItem as OpenXrRuntime)?.Id ?? "", CustomContent: customContent.Checked,
            LauncherTheme: look.ToString(), ClassicLauncher: classicDefault, WpfTheme: wpfTheme, VrQuality: vrQuality, EnhancedPhysics: enhancedPhysics.Checked);
        var bounds = WindowState == FormWindowState.Normal ? Bounds : RestoreBounds;
        var size = IsHandleCreated && bounds.Width > 0 && bounds.Height > 0
            ? new LauncherWindowSize(bounds.Width * 96.0 / DeviceDpi, bounds.Height * 96.0 / DeviceDpi, maximized)
            : savedWindowSize;
        return desktopQuality.Store(settings, false) with { WindowSize = size };
    }

    private void SaveSettings()
    {
        try { BuildPipeline.AtomicWrite(settingsFile, JsonSerializer.Serialize(CurrentSettings(), BuildPipeline.JsonOptions)); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { AppendLog("Could not save portable settings: " + error.Message); }
    }

    private bool physicsAvailable;
    private void RefreshDrivingSettings()
    {
        bool editable = customContent.Enabled && active == null;
        enhancedPhysics.Enabled = editable && (physicsAvailable || enhancedPhysics.Checked);
        practice.Enabled = editable && physicsAvailable;
    }

    private void RefreshLaunch()
    {
        if (car.SelectedItem == null || course.SelectedItem == null) return;
        var quick = QuickRaceSelected;
        var enhanced = N64ModeSelected || RemasteredSelected;
        desktopCourseHeading.Visible = course.Visible = desktopCarHeading.Visible = car.Visible = quick && !VrSelected;
        var styleAvailable = GameContent.SupportsEnhancedStyles(CourseId, CarId);
        visualStyle.Enabled = (VrSelected || !quick || styleAvailable) && active == null;
        vrCarHeading.Visible = vrCar.Visible = vrCourseHeading.Visible = vrCourse.Visible = VrSelected && quick;
        vrCar.Enabled = vrCourse.Enabled = active == null;
        browseCourses.Visible = quick;
        browseCourses.Enabled = active == null;
        conditions.Visible = RemasteredSelected;
        launch.Text = quick ? "Start race" : "Start game";
        preview.AccessibleName = enhanced && quick ? $"{visualStyle.Text} {(VrSelected ? vrCar.Text : car.Text)} on {(VrSelected ? SelectedVrCourse.Name : course.Text)}" : NormalVr ? "Original title screen in VR" : VrSelected ? $"{vrCar.Text} on {SelectedVrCourse.Name}" : quick ? $"{car.Text} on {course.Text}" : "Game startup preview";
        session.Text = NormalVr ? "Use your gamepad or keyboard to navigate the menus in the headset." : VrSelected ? $"{vrCar.Text} • Bonnet view • F2 or right stick to recenter." : quick ? "Three laps against three opponents." : "Start at the title screen and choose an event in-game.";
        controller.Enabled = active == null && !string.IsNullOrWhiteSpace(output.Text);
        BuildRecord? record = null;
        try { if (!string.IsNullOrWhiteSpace(output.Text)) record = BuildPipeline.LastSuccess(output.Text); }
        catch (Exception error) when (error is IOException or InvalidDataException or JsonException or ArgumentException or UnauthorizedAccessException)
        { AppendLog("Build unavailable: " + error.Message); }
        var title = NormalVr ? "Game startup in VR" : VrSelected ? $"{SelectedVrCourse.Name} VR" : quick ? course.Text : "Game startup";
        if (!quick && enhanced)
            title = record?.ContentVersion >= GameContent.StuntStyleVersion ? "Speed Trax / Stunt Trax" : "Speed Trax";
        courseTitle = title;
        inspect.Enabled = active == null && record != null;
        var compatible = record != null && (record.ContentVersion >= 1 || (CarId == "4wd" && !LaunchDisplay().Borderless)) &&
            record.ContentVersion >= GameContent.Course(CourseId).MinimumContentVersion;
        var stage = record == null ? null : Path.Combine(Path.GetFullPath(output.Text), "builds", record.Directory);
        if (graphicsProfile.SelectedItem as string == "VR") lastVrSession.Text = OpenXrRuntime.LastSession(stage);
        var courseAvailable = stage != null && File.Exists(Path.Combine(GameContent.AssetDirectory(stage, LaunchCourse), "world.srfworld"));
        launch.Enabled = active == null && (NormalVr ? courseAvailable && record?.ContentVersion >= GameContent.SeamlessVersion
            : VrSelected ? courseAvailable && GameContent.SupportsVr(SelectedVrCourse, VrCarId, record?.ContentVersion ?? -1)
            : quick ? courseAvailable && (enhanced ? record?.ContentVersion >= GameContent.StyleVersion(CourseId, CarId, SelectedStyle) : compatible)
            : record?.ContentVersion >= (enhanced ? GameContent.ChampionshipStyleVersion : 2));
        bool needsVrStyleBuild = VrSelected && enhanced && record?.ContentVersion < GameContent.VrStyleVersion;
        if (needsVrStyleBuild) launch.Enabled = false;
        smoothVrMotion.Enabled = active == null && record?.ContentVersion >= GameContent.VrStyleVersion;
        hideRaceHud.Enabled = active == null && record?.ContentVersion >= GameContent.RaceHudVersion;
        physicsAvailable = record?.ContentVersion >= GameContent.EnhancedPhysicsVersion;
        RefreshDrivingSettings();
        if (quick && enhancedPhysics.Checked && !physicsAvailable) launch.Enabled = false;
        classicRasterAvailable = record?.ContentVersion >= GameContent.ClassicRasterVersion;
        var editStyle = editingProfile == "VR" ? SelectedStyle : editingProfile switch { "N64 Mode" => RaceStyle.N64, "Remastered" => RaceStyle.Remastered, _ => RaceStyle.Original };
        classicSnes.Visible = editStyle == RaceStyle.Original;
        n64LowRaster.Visible = editStyle == RaceStyle.N64;
        remasteredOptions.Visible = editStyle == RaceStyle.Remastered;
        desktopFps.Visible = fpsLabel.Visible = editingProfile != "VR";
        classicSnes.Enabled = active == null && editStyle == RaceStyle.Original && classicRasterAvailable;
        n64RasterAvailable = record?.ContentVersion >= GameContent.N64RasterVersion;
        n64LowRaster.Enabled = active == null && editStyle == RaceStyle.N64 && n64RasterAvailable;
        bool remasteredGraphics = active == null && editStyle == RaceStyle.Remastered && record?.ContentVersion >= GameContent.SunLightingVersion;
        if (quick && GameContent.IsCustom(LaunchCourse, VrSelected ? VrCarId : CarId) &&
            (record?.ContentVersion ?? -1) < GameContent.RequiredCustomVersion(LaunchCourse, VrSelected ? VrCarId : CarId)) launch.Enabled = false;
        bool nightRace = quick && !VrSelected && (CourseId is "night-owl" or "night-cruise" or "harbor-city");
        timeOfDay.Enabled = active == null && RemasteredSelected && record?.ContentVersion >= GameContent.SunLightingVersion && !nightRace;
        roadDetail.Text = record?.ContentVersion switch
        {
            GameContent.RoadDetailVersion => "High-detail road surfaces (Easy Ride corner)",
            GameContent.FullRoadDetailVersion => "High-detail road surfaces (Easy Ride)",
            GameContent.AquaRoadDetailVersion => "High-detail road surfaces (Easy Ride and Aqua Tunnel)",
            _ => "High-detail road surfaces"
        };
        sceneryDetail.Text = record?.ContentVersion >= GameContent.OwlSceneryVersion
            ? "High-detail roadside scenery"
            : record?.ContentVersion >= GameContent.AquaSceneryVersion
            ? "High-detail roadside scenery (Easy Ride, Aqua Tunnel, Sunset Valley and King's Forest)"
            : record?.ContentVersion >= GameContent.SunsetSceneryVersion
            ? "High-detail roadside scenery (Easy Ride, Sunset Valley and King's Forest)"
            : record?.ContentVersion >= GameContent.EasyRideSceneryVersion
            ? "High-detail roadside scenery (Easy Ride and King's Forest)" : "High-detail roadside scenery (King's Forest)";
        wetTireSpray.Text = record?.ContentVersion == GameContent.WetTireSprayVersion ? "Wet-weather tire spray (Easy Ride)" : "Wet-weather tire spray";
        volumetricLighting.Enabled = remasteredGraphics && record?.ContentVersion >= GameContent.VolumetricLightingVersion;
        wetTireSpray.Enabled = remasteredGraphics && record?.ContentVersion >= GameContent.WetTireSprayVersion;
        sceneryDetail.Enabled = remasteredGraphics && record?.ContentVersion >= GameContent.ForestSceneryVersion;
        roadDetail.Enabled = remasteredGraphics && record?.ContentVersion >= GameContent.RoadDetailVersion;
        roughness.Enabled = clearCoat.Enabled = remasteredGraphics && record?.ContentVersion >= GameContent.MaterialFinishVersion;
        indirect.Enabled = remasteredGraphics && record?.ContentVersion >= GameContent.IndirectLightVersion;
        rayReflections.Enabled = remasteredGraphics && record?.ContentVersion >= GameContent.RayReflectionsVersion;
        rayShadows.Enabled = remasteredGraphics && record?.ContentVersion >= GameContent.RayShadowsVersion;
        shadowQualityLabel.Enabled = shadowQuality.Enabled = rayShadows.Enabled && rayShadows.Checked
            && record?.ContentVersion >= GameContent.SunsetSceneryVersion;
        shadowQuality.Visible = shadowQualityLabel.Visible = rayShadows.Checked;
        weather.Enabled = active == null && RemasteredSelected && record?.ContentVersion >= GameContent.AtmosphereVersion;
        msaa.Enabled = remasteredGraphics && record?.ContentVersion >= GameContent.AtmosphereVersion;
        camera.Enabled = cameraLook.Enabled = remasteredGraphics && record?.ContentVersion >= GameContent.CameraVersion;
        cameraDistance.Enabled = remasteredGraphics && record?.ContentVersion >= GameContent.IsometricVersion;
        bool needsIsometricBuild = camera.Text == "Isometric" && record?.ContentVersion < GameContent.IsometricVersion;
        if (RemasteredSelected && needsIsometricBuild) launch.Enabled = false;
        lightingHint.Text = needsIsometricBuild ? "Rebuild from Game files to use Isometric."
            : nightRace && remasteredGraphics && CourseId == "harbor-city" ? "Harbor City starts at night and brightens on the final lap."
            : nightRace && remasteredGraphics && CourseId == "night-cruise" ? "Night Cruise starts at sunset and darkens during the race."
            : nightRace && remasteredGraphics ? "Night Owl always runs at night. Weather and camera controls are available."
            : record?.ContentVersion >= GameContent.CameraVersion ? "C switches views. Enable camera control to scroll to zoom. Right-drag or move the right stick to look around in the other views. F2 or a right stick click recenters."
            : "Build the game from Game files to enable all Remastered graphics options.";
        graphicsProfile.Enabled = active == null;
        availability.Text = record == null ? "" : $"Built {record.BuiltAt.ToLocalTime():d MMM yyyy}";
        if (active == null)
            status.Text = record == null ? "Choose your ROM and build the game to play."
                : !launch.Enabled && !courseAvailable && (quick || VrSelected) ? $"{GameContent.Course(LaunchCourse).Name} is missing from this build."
                : needsVrStyleBuild ? "Rebuild from Game files to use N64 or Remastered in VR."
                : RemasteredSelected && needsIsometricBuild ? "Rebuild from Game files to use Isometric."
                : !launch.Enabled && enhanced && !quick ? "Rebuild from Game files to enable visual styles in Speed Trax."
                : !launch.Enabled && enhanced ? $"Rebuild to enable {(VrSelected ? vrCar.Text : car.Text)} on {courseTitle}."
                : !launch.Enabled && NormalVr ? "Rebuild to start the full game in VR."
                : !launch.Enabled && VrSelected ? $"Rebuild from Game files to enable {vrCar.Text} on {SelectedVrCourse.Name} in VR."
                : !launch.Enabled ? (quick ? "Rebuild from Game files to use this race selection." : "Choose Quick Race or rebuild for Normal mode.")
                : !quick && record.ContentVersion < GameContent.OriginalBootVersion ? "Rebuild for the original Nintendo splash and menus."
                : "Ready to launch.";
        remedy.Visible = !launch.Enabled && active == null;
        UpdateConditionsSummary(nightRace);
        string? path = null;
        if (stage != null && record != null)
        {
            if (quick && courseAvailable && (VrSelected || compatible) &&
                (!enhanced || record.ContentVersion >= GameContent.StyleVersion(LaunchCourse, VrSelected ? VrCarId : CarId, SelectedStyle)))
                path = Path.Combine(stage, GameContent.CaptureName(LaunchCourse, VrSelected ? VrCarId : CarId, SelectedStyle, record.ContentVersion));
            else if (!quick && record.ContentVersion >= GameContent.OriginalBootVersion)
                path = Path.Combine(stage, "native-smoke.png");
        }
        if (path != previewPath)
        {
            previewPath = path; var previous = preview.CourseImage; preview.CourseImage = null; previous?.Dispose();
            try { if (path != null && File.Exists(path)) { using var image = Image.FromFile(path); preview.CourseImage = new Bitmap(image); } }
            catch (Exception error) when (error is IOException or ArgumentException or OutOfMemoryException) { AppendLog("Preview unavailable: " + error.Message); }
        }
        preview.Text = quick ? (VrSelected ? SelectedVrCourse.Name : course.Text) : "Full game";
        preview.Mode = quick ? "QUICK RACE" : "FULL GAME";
        preview.Subtitle = quick ? $"{(VrSelected ? vrCar.Text : car.Text)} · {visualStyle.Text} · {displayTarget.Text}" : session.Text;
        preview.Invalidate();
        networkPanel?.SetPreviewSource(stage,record?.ContentVersion ?? 0,CourseId);
        if (designReady) UpdateAtlas(stage);

    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) { gridNavigation?.Dispose(); var image = preview.CourseImage; preview.CourseImage = null; image?.Dispose(); }
        base.Dispose(disposing);
    }
    private void ShowError(Exception error) { status.Text = "Failed — see Diagnostics."; AppendLog(error.Message); OpenSettings(filesPage); }
    private readonly UpdatePrompt updatePrompt = new();
    private Task CheckUpdates(bool manual) => updatePrompt.Check(manual,
        () => !IsDisposed && Visible && active == null && networkPanel?.InLobby != true,
        () => Handle, () => { Program.SwitchInterface(settingsFile, CurrentSettings(), true); switchingInterface = true; Close(); });

    private void AppendLog(string message)
    {
        if (IsDisposed) return;
        log.AppendText($"[{DateTime.Now:HH:mm:ss}] {message}{Environment.NewLine}");
        log.SelectionStart = log.TextLength;
        log.ScrollToCaret();
    }
}
