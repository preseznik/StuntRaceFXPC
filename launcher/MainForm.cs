using System.Text.Json;

namespace StuntRaceFX.Launcher;

internal sealed record DisplaySettings(string Resolution = "1920 x 1080", bool Borderless = false, int DesktopFps = 0);

internal sealed record LauncherSettings(string RomPath = "", string OutputPath = "",
    string Car = "4wd", string Resolution = "1920 x 1080", bool Borderless = false,
    string Course = GameContent.DefaultCourse, bool QuickRace = false, bool EasyRideVr = false,
    int DesktopFps = 0, int VrRefresh = 0, int VrScale = 100, string VrCourse = GameContent.DefaultCourse, bool N64Mode = false,
    bool N64LowRaster = false, bool RemasteredMode = false, Dictionary<string, DisplaySettings>? GraphicsProfiles = null, int RemasteredHour = 12,
    int RemasteredMsaa = 0, string RemasteredWeather = "clear", string RemasteredCamera = "chase", bool CameraLook = false,
    int MaxCameraDistance = GameContent.DefaultCameraDistance, string? QuickRaceStyle = null);

internal sealed class MainForm : Form
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
    private readonly ComboBox launchMode = new() { Name = "LaunchMode", DropDownStyle = ComboBoxStyle.DropDownList, Width = 135, AccessibleName = "Launch mode", Anchor = AnchorStyles.Left };
    private readonly TableLayoutPanel quickSettings = new() { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 1, Margin = Padding.Empty };
    private readonly ComboBox visualStyle = new() { Name = "VisualStyle", AccessibleName = "Visual style", DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Top };
    private RaceStyle preferredStyle;
    private readonly Button inspect = new() { Text = "Inspect models", AutoSize = true };
    private readonly Button controller = new() { Text = "Configure controller…", AutoSize = true };
    private readonly ComboBox car = new() { Name = "PlayerCar", DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Top, AccessibleName = "Player car" };
    private readonly ComboBox course = new() { Name = "Course", DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Top, AccessibleName = "Course" };
    private readonly ComboBox vrCourse = new() { Name = "VrCourse", DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Top, AccessibleName = "4WD VR course" };
    private readonly ComboBox resolution = new() { Name = "Resolution", DropDownStyle = ComboBoxStyle.DropDownList, Width = 210, AccessibleName = "Window resolution" };
    private readonly CheckBox n64LowRaster = new() { Name = "N64LowRaster", Text = "Render N64 mode at 320 × 240 (4:3)", AutoSize = true };
    private readonly ComboBox graphicsProfile = new() { Name = "GraphicsProfile", AccessibleName = "Graphics profile", DropDownStyle = ComboBoxStyle.DropDownList, Width = 210 };
    private readonly ComboBox timeOfDay = new() { Name = "TimeOfDay", AccessibleName = "Time of day", DropDownStyle = ComboBoxStyle.DropDownList, Width = 210 };
    private readonly ComboBox msaa = Choices("RemasteredMsaa", "Anti-aliasing", "× MSAA", "Off", 0, 2, 4, 8);
    private readonly ComboBox weather = new() { Name = "RemasteredWeather", AccessibleName = "Weather", DropDownStyle = ComboBoxStyle.DropDownList, Width = 210 };
    private readonly ComboBox camera = new() { Name = "RemasteredCamera", AccessibleName = "Starting camera", DropDownStyle = ComboBoxStyle.DropDownList, Width = 210 };
    private readonly CheckBox cameraLook = new() { Name = "CameraLook", Text = "Mouse and right stick camera control", AutoSize = true };
    private readonly TrackBar cameraDistance = new() { Name = "CameraDistance", AccessibleName = "Maximum camera zoom distance in metres",
        Minimum = GameContent.MinCameraDistance, Maximum = GameContent.MaxCameraDistance, Value = GameContent.DefaultCameraDistance,
        LargeChange = 10, TickFrequency = 10, Width = 210 };
    private readonly Label lightingHint = Hint("Local track time stays fixed during the race.");
    private readonly Dictionary<string, DisplaySettings> profiles = new();
    private string? editingProfile;
    private bool n64RasterAvailable;
    private readonly ComboBox windowMode = new() { Name = "WindowMode", DropDownStyle = ComboBoxStyle.DropDownList, Width = 210, AccessibleName = "Window mode" };
    private readonly ComboBox desktopFps = Choices("DesktopFps", "Desktop driving frame limit", " FPS", "Display sync", 0, 30, 60, 90, 120, 144);
    private readonly ComboBox vrRefresh = Choices("VrRefresh", "Preferred headset refresh", " Hz", "Runtime default", 0, 72, 80, 90, 120, 144);
    private readonly ComboBox vrScale = Choices("VrScale", "VR eye resolution", "%", "", 100, 85, 75, 60);
    private readonly Button cancel = new() { Text = "Cancel build", AutoSize = true, Enabled = false };
    private readonly Label status = new() { Text = "Select your ROM in Game files to begin.", AutoSize = true, Dock = DockStyle.Fill };
    private readonly Label session = new() { AutoSize = true, Dock = DockStyle.Top, Margin = new Padding(0, 16, 0, 0) };
    private readonly Label availability = new() { AutoSize = true, Dock = DockStyle.Top, Margin = new Padding(0, 12, 0, 0) };
    private readonly Label previewHint = new() { Text = "Build from your ROM to see a course preview.", AutoSize = true, Dock = DockStyle.Top };
    private readonly Label courseTitle = Heading("Easy Ride");
    private readonly PictureBox preview = new() { Dock = DockStyle.Fill, SizeMode = PictureBoxSizeMode.Zoom, MinimumSize = new Size(200, 160), AccessibleName = "Selected car on Easy Ride" };
    private readonly TabControl tabs = new LauncherTabs { Name = "LauncherTabs", Dock = DockStyle.Fill, Padding = new Point(16, 8) };
    private readonly ProgressBar progress = new() { Dock = DockStyle.Fill, Height = 6, Style = ProgressBarStyle.Blocks };
    private readonly string settingsFile;
    private CancellationTokenSource? active;
    private bool closeAfterCancel;
    private string? previewPath;
    private string CarId => ((CarChoice)car.SelectedItem!).Id;
    private string CourseId => ((GameCourse)course.SelectedItem!).Id;
    private GameCourse SelectedVrCourse => (GameCourse)vrCourse.SelectedItem!;
    private bool VrSelected => launchMode.SelectedIndex == 2;
    private RaceStyle SelectedStyle => QuickRaceSelected ? (RaceStyle)visualStyle.SelectedIndex : RaceStyle.Original;
    private bool N64ModeSelected => SelectedStyle == RaceStyle.N64;
    private bool RemasteredSelected => SelectedStyle == RaceStyle.Remastered;
    private string LaunchProfile => QuickRaceSelected ? visualStyle.Text : launchMode.Text;
    private bool QuickRaceSelected => launchMode.SelectedIndex == 1;

    public MainForm(string? settingsPath = null)
    {
        settingsFile = settingsPath ?? Path.Combine(AppContext.BaseDirectory, "launcher-settings.json");
        Text = "Stunt Race FX PC";
        ClientSize = new Size(1100, 800); MinimumSize = new Size(820, 620);
        StartPosition = FormStartPosition.CenterScreen; Font = new Font("Segoe UI", 10); AutoScaleMode = AutoScaleMode.Dpi;
        var root = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(20), ColumnCount = 1, RowCount = 5 };
        root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        foreach (var type in new[] { SizeType.AutoSize, SizeType.AutoSize, SizeType.Percent, SizeType.AutoSize, SizeType.AutoSize })
            root.RowStyles.Add(new RowStyle(type, type == SizeType.Percent ? 100 : 0));
        root.Controls.Add(new Label { Text = "STUNT RACE FX  /  PC", AutoSize = true, Font = new Font(Font.FontFamily, 22, FontStyle.Bold), Margin = new Padding(0, 0, 0, 4), ForeColor = LauncherTabs.Accent });
        root.Controls.Add(Hint("Original Stunt Race FX content, running natively on Windows.  •  Development build", 14));
        root.Controls.Add(tabs); root.Controls.Add(progress);
        var footer = new TableLayoutPanel { AutoSize = true, Dock = DockStyle.Fill, ColumnCount = 4, Padding = new Padding(0, 10, 0, 0) };
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        for (var i = 1; i < 4; ++i) footer.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        status.Anchor = AnchorStyles.Left;
        launchMode.Items.AddRange(["Normal", "Quick Race", "4WD VR"]); launchMode.SelectedIndex = 0;
        string[] styles = ["Original", "N64 Mode", "Remastered"];
        visualStyle.Items.AddRange(styles); visualStyle.SelectedIndex = 0;
        graphicsProfile.Items.AddRange(["Normal", .. styles, "4WD VR"]);
        footer.Controls.Add(status, 0, 0);
        footer.Controls.Add(new Label { Text = "Launch &mode", AutoSize = true, Anchor = AnchorStyles.Left }, 1, 0);
        footer.Controls.Add(launchMode, 2, 0); footer.Controls.Add(launch, 3, 0); root.Controls.Add(footer); Controls.Add(root);
        car.Items.AddRange([new CarChoice("4wd", "4WD"), new CarChoice("coupe", "COUPE"), new CarChoice("f-type", "F-TYPE")]); car.SelectedIndex = 0;
        course.Items.AddRange(GameContent.Courses.Cast<object>().ToArray()); course.SelectedIndex = 0;
        vrCourse.Items.AddRange(GameContent.Courses.Where(c => c.VrVersion.HasValue).ToArray()); vrCourse.SelectedIndex = 0;
        resolution.Items.AddRange(["1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160"]); resolution.SelectedIndex = 1;
        windowMode.Items.AddRange(["Windowed", "Borderless fullscreen"]); windowMode.SelectedIndex = 0;

        var racePage = Page("Race");
        var raceLayout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1 };
        raceLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 62)); raceLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 38));
        var coursePanel = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 3, Margin = new Padding(0, 0, 18, 0) };
        coursePanel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        coursePanel.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        coursePanel.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); coursePanel.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        coursePanel.Controls.Add(courseTitle);
        coursePanel.Controls.Add(preview); coursePanel.Controls.Add(previewHint);
        var selection = Stack();
        quickSettings.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        quickSettings.Controls.Add(Heading("Course")); quickSettings.Controls.Add(course);
        quickSettings.Controls.Add(Heading("Your car", 18)); quickSettings.Controls.Add(car);
        quickSettings.Controls.Add(Heading("Visual style", 18)); quickSettings.Controls.Add(visualStyle);
        selection.Controls.Add(quickSettings);
        selection.Controls.Add(vrCourse);
        selection.Controls.Add(session); selection.Controls.Add(availability);
        raceLayout.Controls.Add(coursePanel, 0, 0); raceLayout.Controls.Add(selection, 1, 0); racePage.Controls.Add(raceLayout);

        var displayColumns = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1 };
        displayColumns.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        displayColumns.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        var graphics = Stack(); var headset = Stack();
        displayColumns.Controls.Add(graphics, 0, 0); var modeOptions = new Panel { Dock = DockStyle.Fill };
        modeOptions.Controls.Add(headset); displayColumns.Controls.Add(modeOptions, 1, 0);
        var lighting = Stack();
        modeOptions.Controls.Add(lighting);
        lighting.Controls.Add(Heading("Remastered"));
        lighting.Controls.Add(Hint("Anti-aliasing")); lighting.Controls.Add(msaa);
        weather.Items.AddRange(["Clear", "Partly cloudy", "Overcast", "Fog", "Rain"]);
        weather.SelectedIndex = 0;
        lighting.Controls.Add(Hint("Weather")); lighting.Controls.Add(weather);
        lighting.Controls.Add(Hint("Time of day")); lighting.Controls.Add(timeOfDay);
        camera.Items.AddRange(["Chase", "Far chase", "Cockpit", "Bonnet / hood", "Isometric"]);
        camera.SelectedIndex = 0;
        lighting.Controls.Add(Hint("Starting camera")); lighting.Controls.Add(camera);
        lighting.Controls.Add(cameraLook);
        var distanceLabel = Hint($"Maximum zoom distance: {cameraDistance.Value} m");
        cameraDistance.ValueChanged += (_, _) => distanceLabel.Text = $"Maximum zoom distance: {cameraDistance.Value} m";
        lighting.Controls.Add(distanceLabel); lighting.Controls.Add(cameraDistance);
        lightingHint.MaximumSize = new Size(295, 0);
        lighting.Controls.Add(lightingHint);
        for (int hour = 0; hour < 24; ++hour) timeOfDay.Items.Add($"{hour:00}:00");
        timeOfDay.SelectedIndex = 12;
        Page("Graphics").Controls.Add(displayColumns);
        graphics.Controls.Add(Heading("Graphics profile")); graphics.Controls.Add(graphicsProfile);
        graphics.Controls.Add(Hint("Window mode", 6, 18)); graphics.Controls.Add(windowMode);
        graphics.Controls.Add(Hint("Window resolution", 6, 18)); graphics.Controls.Add(resolution);
        graphics.Controls.Add(n64LowRaster);
        var fpsLabel = Hint("Desktop driving frame limit");
        graphics.Controls.Add(fpsLabel); graphics.Controls.Add(desktopFps);
        graphics.Controls.Add(Hint("F11 switches window mode during a race.", 10, 10));
        headset.Controls.Add(Heading("4WD VR"));
        headset.Controls.Add(Hint("Preferred headset refresh")); headset.Controls.Add(vrRefresh);
        headset.Controls.Add(Hint("Eye resolution", 6, 18)); headset.Controls.Add(vrScale);
        headset.Controls.Add(Hint("Lower eye resolution reduces GPU work. The HUD stays at full resolution.", 6, 10));
        headset.Controls.Add(Hint("The runtime keeps its current refresh if the requested rate is unavailable."));
        foreach (var label in headset.Controls.OfType<Label>().Concat(graphics.Controls.OfType<Label>()))
            label.MaximumSize = new Size(295, 0);
        windowMode.SelectedIndexChanged += (_, _) => { resolution.Enabled = active == null && windowMode.SelectedIndex == 0; RefreshLaunch(); };
        var controls = Stack(); Page("Controls").Controls.Add(controls);
        controls.Controls.Add(Heading("Controller"));
        controls.Controls.Add(Hint("Remap buttons, choose digital or proportional analog steering, and adjust stick deadzone. Settings apply on the next launch.")); controls.Controls.Add(controller);
        controls.Controls.Add(Heading("Keyboard", 28));
        controls.Controls.Add(Hint("W / Up     Accelerate\nS / Down   Brake\nA / D         Steer\nSpace       Boost\nX               Reverse\n\nR   Retire / restart      P   Pause      F11   Fullscreen\n\nGame menus use arrows or the D-pad. Enter, Start or the bottom face button confirms. Backspace or the left face button goes back where available."));

        var files = Stack(); Page("Game files").Controls.Add(files);
        files.Controls.Add(Heading("Build from your ROM"));
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
        var showLog = new Button { Text = "View build log", AutoSize = true }; showLog.Click += (_, _) => tabs.SelectedIndex = 4; files.Controls.Add(showLog);
        var diagnostics = Page("Diagnostics");
        var diagnosticLayout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 3 };
        diagnosticLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        diagnosticLayout.RowStyles.Add(new RowStyle(SizeType.AutoSize)); diagnosticLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); diagnosticLayout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        diagnosticLayout.Controls.Add(Hint("Build output and launch errors.")); diagnosticLayout.Controls.Add(log);
        inspect.Anchor = AnchorStyles.Left;
        diagnosticLayout.Controls.Add(inspect); diagnostics.Controls.Add(diagnosticLayout);

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
                if (dialog.ShowDialog(this) == DialogResult.OK) AppendLog("Controller settings saved. They apply on the next game launch.");
            }
            catch (Exception error) { ShowError(error); }
        };
        output.TextChanged += (_, _) => RefreshLaunch();
        graphicsProfile.SelectedIndexChanged += (_, _) => {
            SelectGraphicsProfile();
            headset.Visible = graphicsProfile.Text == "4WD VR";
            lighting.Visible = graphicsProfile.Text == "Remastered";
            desktopFps.Visible = fpsLabel.Visible = graphicsProfile.Text != "4WD VR";
            n64LowRaster.Visible = graphicsProfile.Text == "N64 Mode";
            RefreshLaunch();
        };
        graphicsProfile.SelectedIndex = 0;
        launchMode.SelectedIndexChanged += (_, _) => SelectRaceProfile();
        course.SelectedIndexChanged += (_, _) => SelectRaceProfile();
        car.SelectedIndexChanged += (_, _) => SelectRaceProfile();
        visualStyle.SelectedIndexChanged += (_, _) => {
            if (QuickRaceSelected && GameContent.SupportsEnhancedStyles(CourseId, CarId)) preferredStyle = SelectedStyle;
            graphicsProfile.SelectedItem = LaunchProfile;
            RefreshLaunch();
        };
        camera.SelectedIndexChanged += (_, _) => RefreshLaunch();
        vrCourse.SelectedIndexChanged += (_, _) => RefreshLaunch();
        FormClosing += (_, args) => { if (active != null) { args.Cancel = true; closeAfterCancel = true; active.Cancel(); } else SaveSettings(); };
        LoadSettings(); RefreshLaunch();
    }

    private void Launch()
    {
        try
        {
            SaveSettings();
            var display = LaunchDisplay();
            var dimensions = display.Resolution.Split('x', StringSplitOptions.TrimEntries);
            using var process = BuildPipeline.Launch(output.Text, width: int.Parse(dimensions[0]), height: int.Parse(dimensions[1]),
                car: CarId, borderless: display.Borderless, course: VrSelected ? SelectedVrCourse.Id : CourseId, game: launchMode.SelectedIndex == 0, vr: VrSelected, style: SelectedStyle,
                desktopFps: display.DesktopFps, vrRefresh: SelectedNumber(vrRefresh), vrScale: SelectedNumber(vrScale),
                n64LowRaster: N64ModeSelected && n64RasterAvailable && n64LowRaster.Checked, remasteredHour: timeOfDay.SelectedIndex,
                remasteredMsaa: SelectedNumber(msaa), remasteredWeather: GameContent.WeatherIds[weather.SelectedIndex],
                remasteredCamera: GameContent.CameraIds[camera.SelectedIndex], cameraLook: cameraLook.Checked, maxCameraDistance: cameraDistance.Value);
            status.Text = N64ModeSelected || RemasteredSelected ? $"{courseTitle.Text} launched." : VrSelected ? $"{SelectedVrCourse.Name} launched in OpenXR." : QuickRaceSelected ? $"Quick Race launched — {course.Text}, {car.Text}." : "Game launched.";
            AppendLog(status.Text);
        }
        catch (Exception error) { ShowError(error); }
    }
    private TabPage Page(string title)
    {
        var page = new TabPage(title) { Padding = new Padding(18), UseVisualStyleBackColor = false, BackColor = LauncherTabs.Surface, ForeColor = LauncherTabs.Ink }; tabs.TabPages.Add(page); return page;
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
        fields.Enabled = verify.Enabled = build.Enabled = launch.Enabled = launchMode.Enabled = inspect.Enabled = controller.Enabled = resolution.Enabled = car.Enabled = course.Enabled = visualStyle.Enabled = vrCourse.Enabled = windowMode.Enabled = false;
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
            fields.Enabled = verify.Enabled = build.Enabled = launchMode.Enabled = car.Enabled = course.Enabled = windowMode.Enabled = true;
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
        visualStyle.SelectedIndex = QuickRaceSelected && GameContent.SupportsEnhancedStyles(CourseId, CarId) ? (int)preferredStyle : 0;
        graphicsProfile.SelectedItem = LaunchProfile;
        RefreshLaunch();
    }

    private void SelectGraphicsProfile()
    {
        if (graphicsProfile.SelectedIndex < 0 || editingProfile == graphicsProfile.Text) return;
        if (editingProfile != null) profiles[editingProfile] = ReadDisplay();
        editingProfile = graphicsProfile.Text;
        var display = profiles.GetValueOrDefault(editingProfile) ?? new DisplaySettings();
        resolution.SelectedItem = resolution.Items.Contains(display.Resolution) ? display.Resolution : "1920 x 1080";
        windowMode.SelectedIndex = display.Borderless ? 1 : 0;
        RestoreNumber(desktopFps, display.DesktopFps);
    }

    private void LoadSettings()
    {
        output.Text = Path.Combine(AppContext.BaseDirectory, "user-builds");
        try
        {
            if (!File.Exists(settingsFile)) return;
            using var json = JsonDocument.Parse(File.ReadAllText(settingsFile));
            var settings = json.RootElement.Deserialize<LauncherSettings>();
            if (settings == null) return;
            var savedN64Mode = json.RootElement.TryGetProperty(nameof(settings.N64Mode), out _) ? settings.N64Mode
                : json.RootElement.TryGetProperty("Remastered", out var legacyMode) && legacyMode.ValueKind == JsonValueKind.True;
            rom.Text = settings.RomPath;
            if (!string.IsNullOrWhiteSpace(settings.OutputPath)) output.Text = settings.OutputPath;
            car.SelectedItem = car.Items.Cast<CarChoice>().FirstOrDefault(c => c.Id == settings.Car) ?? car.Items[0];
            course.SelectedItem = course.Items.Cast<GameCourse>().FirstOrDefault(c => c.Id == settings.Course) ?? course.Items[0];
            vrCourse.SelectedItem = vrCourse.Items.Cast<GameCourse>().FirstOrDefault(c => c.Id == settings.VrCourse) ?? vrCourse.Items[0];
            var fallback = settings.GraphicsProfiles == null
                ? new DisplaySettings(settings.Resolution, settings.Borderless, settings.DesktopFps)
                : new DisplaySettings();
            foreach (string name in graphicsProfile.Items)
            {
                var oldKey = name switch { "Original" => "Quick Race", "N64 Mode" => "N64 mode", _ => name };
                profiles[name] = settings.GraphicsProfiles?.GetValueOrDefault(name) ?? settings.GraphicsProfiles?.GetValueOrDefault(oldKey) ?? fallback;
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
            launchMode.SelectedIndex = legacyEnhanced ? 1 : settings.EasyRideVr ? 2 : settings.QuickRace ? 1 : 0;
            SelectRaceProfile();
            n64LowRaster.Checked = settings.N64LowRaster;
            RestoreNumber(msaa, settings.RemasteredMsaa);
            weather.SelectedIndex = Math.Max(0, Array.IndexOf(GameContent.WeatherIds, settings.RemasteredWeather));
            camera.SelectedIndex = Math.Max(0, Array.IndexOf(GameContent.CameraIds, settings.RemasteredCamera));
            cameraLook.Checked = settings.CameraLook;
            cameraDistance.Value = Math.Clamp(settings.MaxCameraDistance, cameraDistance.Minimum, cameraDistance.Maximum);
            timeOfDay.SelectedIndex = settings.RemasteredHour is >= 0 and < 24 ? settings.RemasteredHour : 12;
            SelectGraphicsProfile();
            RestoreNumber(vrRefresh, settings.VrRefresh); RestoreNumber(vrScale, settings.VrScale);
        }
        catch (Exception error) when (error is IOException or JsonException or UnauthorizedAccessException)
        { AppendLog("Could not load settings: " + error.Message); }
    }

    private void SaveSettings()
    {
        if (editingProfile != null) profiles[editingProfile] = ReadDisplay();
        var display = LaunchDisplay();
        try { BuildPipeline.AtomicWrite(settingsFile, JsonSerializer.Serialize(new LauncherSettings(rom.Text, output.Text, CarId, display.Resolution, display.Borderless, CourseId, QuickRaceSelected, VrSelected,
            display.DesktopFps, SelectedNumber(vrRefresh), SelectedNumber(vrScale), SelectedVrCourse.Id, N64ModeSelected, n64LowRaster.Checked, RemasteredSelected, profiles, timeOfDay.SelectedIndex,
            SelectedNumber(msaa), GameContent.WeatherIds[weather.SelectedIndex], GameContent.CameraIds[camera.SelectedIndex], cameraLook.Checked, cameraDistance.Value, preferredStyle.ToString()), BuildPipeline.JsonOptions)); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { AppendLog("Could not save portable settings: " + error.Message); }
    }

    private void RefreshLaunch()
    {
        if (car.SelectedItem == null || course.SelectedItem == null) return;
        var quick = QuickRaceSelected;
        var enhanced = N64ModeSelected || RemasteredSelected;
        quickSettings.Visible = quick;
        var styleAvailable = GameContent.SupportsEnhancedStyles(CourseId, CarId);
        visualStyle.Enabled = quick && styleAvailable && active == null;
        vrCourse.Visible = VrSelected;
        vrCourse.Enabled = active == null;
        launch.Text = VrSelected ? "Launch VR" : quick ? "Launch Quick Race" : "Launch Game";
        courseTitle.Text = enhanced ? $"{course.Text} ({visualStyle.Text})" : VrSelected ? $"{SelectedVrCourse.Name} VR" : quick ? course.Text : "Game startup";
        preview.AccessibleName = enhanced ? $"{visualStyle.Text} {car.Text} on {course.Text}" : VrSelected ? $"4WD on {SelectedVrCourse.Name}" : quick ? $"{car.Text} on {course.Text}" : "Game startup preview";
        session.Text = VrSelected ? "4WD • Bonnet view • F2 or right stick to recenter." : quick ? "Three laps against three opponents." : "Start the game and choose your car and class in the game menus.";
        controller.Enabled = active == null && !string.IsNullOrWhiteSpace(output.Text);
        BuildRecord? record = null;
        try { if (!string.IsNullOrWhiteSpace(output.Text)) record = BuildPipeline.LastSuccess(output.Text); }
        catch (Exception error) when (error is IOException or InvalidDataException or JsonException or ArgumentException or UnauthorizedAccessException)
        { AppendLog("Build unavailable: " + error.Message); }
        inspect.Enabled = active == null && record != null;
        var compatible = record != null && (record.ContentVersion >= 1 || (CarId == "4wd" && !LaunchDisplay().Borderless)) &&
            record.ContentVersion >= GameContent.Course(CourseId).MinimumContentVersion;
        var stage = record == null ? null : Path.Combine(Path.GetFullPath(output.Text), "builds", record.Directory);
        var courseAvailable = stage != null && File.Exists(Path.Combine(GameContent.AssetDirectory(stage, VrSelected ? SelectedVrCourse.Id : CourseId), "world.srfworld"));
        launch.Enabled = active == null && (enhanced ? record?.ContentVersion >= GameContent.StyleVersion(CourseId, CarId, SelectedStyle) && courseAvailable : VrSelected ? record?.ContentVersion >= SelectedVrCourse.VrVersion && courseAvailable : quick ? compatible && courseAvailable : record?.ContentVersion >= 2);
        n64RasterAvailable = record?.ContentVersion >= GameContent.N64RasterVersion;
        n64LowRaster.Enabled = active == null && editingProfile == "N64 Mode" && n64RasterAvailable;
        bool remasteredGraphics = active == null && editingProfile == "Remastered" && record?.ContentVersion >= GameContent.SunLightingVersion;
        bool nightRace = quick && (CourseId is "night-owl" or "night-cruise" or "harbor-city");
        timeOfDay.Enabled = remasteredGraphics && !nightRace;
        msaa.Enabled = weather.Enabled = remasteredGraphics && record?.ContentVersion >= GameContent.AtmosphereVersion;
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
            status.Text = record == null ? "Build the game from Game files to begin."
                : RemasteredSelected && needsIsometricBuild ? "Rebuild from Game files to use Isometric."
                : !launch.Enabled && enhanced ? $"Rebuild from Game files to enable {car.Text} on {courseTitle.Text}."
                : !launch.Enabled && VrSelected ? $"Rebuild from Game files to enable {SelectedVrCourse.Name} VR."
                : !launch.Enabled ? (quick ? "Rebuild from Game files to use this race selection." : "Choose Quick Race or rebuild for Normal mode.")
                : !quick && record.ContentVersion < GameContent.OriginalBootVersion ? "Rebuild for the original Nintendo splash and menus."
                : "Ready to launch.";
        string? path = null;
        if (stage != null && record != null)
        {
            if (enhanced) path = record.ContentVersion >= GameContent.StyleVersion(CourseId, CarId, SelectedStyle) && courseAvailable
                ? Path.Combine(stage, GameContent.StylePreview(CourseId, CarId, SelectedStyle, record.ContentVersion)) : null;
            else if (VrSelected && courseAvailable) path = Path.Combine(stage, $"preview-{SelectedVrCourse.Id}-4wd.png");
            else if (quick && compatible && courseAvailable)
                path = Path.Combine(stage, record.ContentVersion >= 2 ? $"preview-{CourseId}-{CarId}.png" : $"preview-{CarId}.png");
            else if (!quick && !VrSelected && record.ContentVersion >= GameContent.OriginalBootVersion)
                path = Path.Combine(stage, "native-smoke.png");
        }
        if (path != previewPath)
        {
            previewPath = path; var previous = preview.Image; preview.Image = null; previous?.Dispose();
            try { if (path != null && File.Exists(path)) { using var image = Image.FromFile(path); preview.Image = new Bitmap(image); } }
            catch (Exception error) when (error is IOException or ArgumentException or OutOfMemoryException) { AppendLog("Preview unavailable: " + error.Message); }
        }
        previewHint.Text = preview.Image == null ? "Build from your ROM to see a preview." : VrSelected ? $"4WD • {SelectedVrCourse.Name}" : quick ? $"{car.Text} • {course.Text}" : "Original title screen";
    }

    protected override void Dispose(bool disposing) { if (disposing) preview.Image?.Dispose(); base.Dispose(disposing); }
    private void ShowError(Exception error) { status.Text = "Failed — see Diagnostics."; AppendLog(error.Message); tabs.SelectedIndex = 4; }

    private void AppendLog(string message)
    {
        if (IsDisposed) return;
        log.AppendText($"[{DateTime.Now:HH:mm:ss}] {message}{Environment.NewLine}");
        log.SelectionStart = log.TextLength;
        log.ScrollToCaret();
    }
}
