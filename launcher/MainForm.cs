using System.Text.Json;

namespace StuntRaceFX.Launcher;

internal sealed record LauncherSettings(string RomPath = "", string OutputPath = "",
    string Car = "4wd", string Resolution = "1920 x 1080", bool Borderless = false,
    string Course = GameContent.DefaultCourse, bool QuickRace = false, bool EasyRideVr = false);

internal sealed class MainForm : Form
{
    private sealed record CarChoice(string Id, string Name) { public override string ToString() => Name; }
    private readonly TextBox rom = new() { Dock = DockStyle.Fill, AccessibleName = "Game ROM" };
    private readonly TextBox output = new() { Dock = DockStyle.Fill, AccessibleName = "Build folder" };
    private readonly TextBox log = new() { Dock = DockStyle.Fill, Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Both, WordWrap = false, Font = new Font("Consolas", 9) };
    private readonly Button verify = new() { Text = "Verify ROM", AutoSize = true };
    private readonly Button build = new() { Text = "Build game", AutoSize = true };
    private readonly Button launch = new() { Name = "LaunchGame", Text = "Launch Game", AutoSize = true, Padding = new Padding(18, 5, 18, 5) };
    private readonly ComboBox launchMode = new() { Name = "LaunchMode", DropDownStyle = ComboBoxStyle.DropDownList, Width = 135, AccessibleName = "Launch mode", Anchor = AnchorStyles.Left };
    private readonly TableLayoutPanel quickSettings = new() { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 1, Margin = Padding.Empty };
    private readonly Button inspect = new() { Text = "Inspect models", AutoSize = true };
    private readonly Button controller = new() { Text = "Configure controller…", AutoSize = true };
    private readonly ComboBox car = new() { Name = "PlayerCar", DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Top, AccessibleName = "Player car" };
    private readonly ComboBox course = new() { Name = "Course", DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Top, AccessibleName = "Course" };
    private readonly ComboBox resolution = new() { Name = "Resolution", DropDownStyle = ComboBoxStyle.DropDownList, Width = 210, AccessibleName = "Window resolution" };
    private readonly ComboBox windowMode = new() { Name = "WindowMode", DropDownStyle = ComboBoxStyle.DropDownList, Width = 210, AccessibleName = "Window mode" };
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
    private bool VrSelected => launchMode.SelectedIndex == 2;
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
        launchMode.Items.AddRange(["Normal", "Quick Race", "Easy Ride VR"]); launchMode.SelectedIndex = 0;
        footer.Controls.Add(status, 0, 0);
        footer.Controls.Add(new Label { Text = "Launch &mode", AutoSize = true, Anchor = AnchorStyles.Left }, 1, 0);
        footer.Controls.Add(launchMode, 2, 0); footer.Controls.Add(launch, 3, 0); root.Controls.Add(footer); Controls.Add(root);
        car.Items.AddRange([new CarChoice("4wd", "4WD"), new CarChoice("coupe", "COUPE"), new CarChoice("f-type", "F-TYPE")]); car.SelectedIndex = 0;
        course.Items.AddRange(GameContent.Courses.Cast<object>().ToArray()); course.SelectedIndex = 0;
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
        selection.Controls.Add(quickSettings);
        selection.Controls.Add(session); selection.Controls.Add(availability);
        raceLayout.Controls.Add(coursePanel, 0, 0); raceLayout.Controls.Add(selection, 1, 0); racePage.Controls.Add(raceLayout);

        var graphics = Stack(); Page("Graphics").Controls.Add(graphics);
        graphics.Controls.Add(Heading("Display")); graphics.Controls.Add(Hint("Window mode")); graphics.Controls.Add(windowMode);
        graphics.Controls.Add(Hint("Window resolution", 6, 18)); graphics.Controls.Add(resolution);
        graphics.Controls.Add(Hint("Borderless uses your monitor's desktop resolution. F11 switches window mode during a race.", 14, 18));
        graphics.Controls.Add(Hint("Rendering follows the display's refresh rate. Changing resolution does not change race speed."));
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
        output.TextChanged += (_, _) => RefreshLaunch(); car.SelectedIndexChanged += (_, _) => RefreshLaunch();
        launchMode.SelectedIndexChanged += (_, _) => RefreshLaunch();
        course.SelectedIndexChanged += (_, _) => RefreshLaunch();
        FormClosing += (_, args) => { if (active != null) { args.Cancel = true; closeAfterCancel = true; active.Cancel(); } else SaveSettings(); };
        LoadSettings(); RefreshLaunch();
    }

    private void Launch()
    {
        try
        {
            SaveSettings();
            var dimensions = resolution.Text.Split('x', StringSplitOptions.TrimEntries);
            using var process = BuildPipeline.Launch(output.Text, width: int.Parse(dimensions[0]), height: int.Parse(dimensions[1]),
                car: CarId, borderless: windowMode.SelectedIndex == 1, course: CourseId, game: !QuickRaceSelected && !VrSelected, vr: VrSelected);
            status.Text = VrSelected ? "Easy Ride launched in OpenXR." : QuickRaceSelected ? $"Quick Race launched — {course.Text}, {car.Text}." : "Game launched.";
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
        fields.Enabled = verify.Enabled = build.Enabled = launch.Enabled = launchMode.Enabled = inspect.Enabled = controller.Enabled = resolution.Enabled = car.Enabled = course.Enabled = windowMode.Enabled = false;
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

    private void LoadSettings()
    {
        output.Text = Path.Combine(AppContext.BaseDirectory, "user-builds");
        try
        {
            if (!File.Exists(settingsFile)) return;
            var settings = JsonSerializer.Deserialize<LauncherSettings>(File.ReadAllText(settingsFile));
            if (settings == null) return;
            rom.Text = settings.RomPath;
            if (!string.IsNullOrWhiteSpace(settings.OutputPath)) output.Text = settings.OutputPath;
            car.SelectedItem = car.Items.Cast<CarChoice>().FirstOrDefault(c => c.Id == settings.Car) ?? car.Items[0];
            course.SelectedItem = course.Items.Cast<GameCourse>().FirstOrDefault(c => c.Id == settings.Course) ?? course.Items[0];
            launchMode.SelectedIndex = settings.EasyRideVr ? 2 : settings.QuickRace ? 1 : 0;
            if (resolution.Items.Contains(settings.Resolution)) resolution.SelectedItem = settings.Resolution;
            windowMode.SelectedIndex = settings.Borderless ? 1 : 0;
        }
        catch (Exception error) when (error is IOException or JsonException or UnauthorizedAccessException)
        { AppendLog("Could not load settings: " + error.Message); }
    }

    private void SaveSettings()
    {
        try { BuildPipeline.AtomicWrite(settingsFile, JsonSerializer.Serialize(new LauncherSettings(rom.Text, output.Text, CarId, resolution.Text, windowMode.SelectedIndex == 1, CourseId, QuickRaceSelected, VrSelected), BuildPipeline.JsonOptions)); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { AppendLog("Could not save portable settings: " + error.Message); }
    }

    private void RefreshLaunch()
    {
        if (car.SelectedItem == null || course.SelectedItem == null) return;
        var quick = QuickRaceSelected;
        quickSettings.Visible = quick;
        launch.Text = VrSelected ? "Launch VR" : quick ? "Launch Quick Race" : "Launch Game";
        courseTitle.Text = VrSelected ? "Easy Ride VR" : quick ? course.Text : "Game startup";
        preview.AccessibleName = VrSelected ? "4WD on Easy Ride" : quick ? $"{car.Text} on {course.Text}" : "Game startup preview";
        session.Text = VrSelected ? "4WD • Bonnet view • F2 or right stick to recenter." : quick ? "Three laps against three opponents." : "Start the game and choose your car and class in the game menus.";
        controller.Enabled = active == null && !string.IsNullOrWhiteSpace(output.Text);
        BuildRecord? record = null;
        try { if (!string.IsNullOrWhiteSpace(output.Text)) record = BuildPipeline.LastSuccess(output.Text); }
        catch (Exception error) when (error is IOException or InvalidDataException or JsonException or ArgumentException or UnauthorizedAccessException)
        { AppendLog("Build unavailable: " + error.Message); }
        inspect.Enabled = active == null && record != null;
        var compatible = record != null && (record.ContentVersion >= 1 || (CarId == "4wd" && windowMode.SelectedIndex == 0)) &&
            record.ContentVersion >= GameContent.Course(CourseId).MinimumContentVersion;
        var stage = record == null ? null : Path.Combine(Path.GetFullPath(output.Text), "builds", record.Directory);
        var courseAvailable = stage != null && File.Exists(Path.Combine(GameContent.AssetDirectory(stage, CourseId), "world.srfworld"));
        launch.Enabled = active == null && (VrSelected ? record?.ContentVersion >= 28 : quick ? compatible && courseAvailable : record?.ContentVersion >= 2);
        availability.Text = record == null ? "" : $"Built {record.BuiltAt.ToLocalTime():d MMM yyyy}";
        if (active == null)
            status.Text = record == null ? "Build the game from Game files to begin."
                : !launch.Enabled && VrSelected ? "Rebuild from Game files to enable Easy Ride VR."
                : !launch.Enabled ? (quick ? "Rebuild from Game files to use this race selection." : "Choose Quick Race or rebuild for Normal mode.")
                : !quick && record.ContentVersion < GameContent.OriginalBootVersion ? "Rebuild for the original Nintendo splash and menus."
                : "Ready to launch.";
        string? path = null;
        if (stage != null && record != null)
        {
            if (VrSelected) path = Path.Combine(stage, "preview-easy-ride-4wd.png");
            else if (quick && compatible && courseAvailable)
                path = Path.Combine(stage, record.ContentVersion >= 2 ? $"preview-{CourseId}-{CarId}.png" : $"preview-{CarId}.png");
            else if (!quick && record.ContentVersion >= GameContent.OriginalBootVersion)
                path = Path.Combine(stage, "native-smoke.png");
        }
        if (path != previewPath)
        {
            previewPath = path; var previous = preview.Image; preview.Image = null; previous?.Dispose();
            try { if (path != null && File.Exists(path)) { using var image = Image.FromFile(path); preview.Image = new Bitmap(image); } }
            catch (Exception error) when (error is IOException or ArgumentException or OutOfMemoryException) { AppendLog("Preview unavailable: " + error.Message); }
        }
        previewHint.Text = preview.Image == null ? "Build from your ROM to see a preview." : VrSelected ? "4WD • Easy Ride" : quick ? $"{car.Text} • {course.Text}" : "Original title screen";
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
