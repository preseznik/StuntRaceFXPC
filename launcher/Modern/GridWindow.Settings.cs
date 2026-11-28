using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using Microsoft.Win32;

namespace StuntRaceFX.Launcher.Modern;

using Button = System.Windows.Controls.Button;
using CheckBox = System.Windows.Controls.CheckBox;
using ComboBox = System.Windows.Controls.ComboBox;
using TextBox = System.Windows.Controls.TextBox;

internal partial class GridWindow
{
    private readonly Queue<string> buildLog = new();
    private TextBox? visibleLog;
    private void ShowSettings(string section)
    {
        qualityPresetFocus = null;
        settingsSection = section; SettingsBody.Children.Clear(); visibleLog = null;
        foreach (Button b in SettingsRail.Children.OfType<Button>()) b.Tag = Equals(b.Content, section) ? "active" : null;
        SettingsBody.Children.Add(new TextBlock { Text = section, Style = (Style)FindResource("Heading") });
        switch (section)
        {
            case "Graphics": GraphicsSettings(); break;
            case "Gameplay":
                var driving = Group("Driving physics");
                Toggle(driving, "Enhanced physics", settings.EnhancedPhysics, v => { settings = settings with { EnhancedPhysics = v }; RefreshSetup(); },
                    lobby is not { Closed: false } && (installed.Version >= GameContent.EnhancedPhysicsVersion || settings.EnhancedPhysics));
                Hint(driving, GameContent.DrivingPhysicsHelp);
                ActionButton(driving, "Start practice (4WD Test Run)", () =>
                {
                    try { Save(); using var game = RaceLaunch.StartPractice(settings); Say("Practice launched."); }
                    catch (Exception ex) { Say(ex.Message); }
                }, installed.Version >= GameContent.EnhancedPhysicsVersion && build == null && lobby is not { Closed: false });
                break;
            case "Content":
                var content = Group("Custom content");
                Toggle(content, "Include custom courses and vehicles", settings.CustomContent, v =>
                {
                    settings = settings with { CustomContent = v };
                    if (!v) settings = settings with
                    {
                        Course = StockTrack(settings.Course),
                        VrCourse = StockTrack(settings.VrCourse),
                        Car = StockCar(settings.Car),
                        VrCar = StockCar(settings.VrCar)
                    };
                    tileKey = null; RefreshSetup();
                }, lobby is not { Closed: false });
                Hint(content, lobby is { Closed: false } ? "Leave the lobby to change its content catalog." : "Applies to Quick Race and LAN. Enable custom content on each participating PC.");
                break;
            case "Controls": ControllerSettings(); break;
            case "Game files": FilesSettings(); break;
            case "Appearance":
                var themes = Group("WPF launcher design");
                Pick(themes, "Theme", Enum.GetValues<LauncherLook>(), look, ChangeLook, format: value => LauncherDesign.Names[(int)value]);
                Hint(themes, "Race Desk keeps the setup beside the preview. Course Atlas opens on the course gallery. Starting Grid puts the last race first.");
                var appearance = Group("Launcher interface");
                Toggle(appearance, "Classic Look (restarts launcher)", false, enable =>
                {
                    if (!enable) return;
                    try
                    {
                        Program.SwitchInterface(settingsFile, CurrentSettings(), true);
                        switchingInterface = true;
                        Close();
                    }
                    catch (Exception error) { ShowSettings("Appearance"); Say(error.Message); }
                }, loadError == null && build == null && !starting && !connecting && lobby is not { Closed: false });
                Hint(appearance, "Classic includes the Race Desk, Course Atlas and Starting Grid themes. Both launchers use the same game settings.");
                break;
        }
        SettingsScroll.ScrollToTop();
    }
    private static string StockTrack(string id) => GameContent.IsCustomCourse(id) ? GameContent.DefaultCourse : id;
    private static string StockCar(string id) => GameContent.CustomCars.Contains(id) ? "4wd" : id;
    private StackPanel Group(string name)
    {
        var body = new StackPanel();
        body.Children.Add(new TextBlock { Text = name, FontSize = 19, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 0, 0, 8) });
        SettingsBody.Children.Add(new Border { Style = (Style)FindResource("Card"), Child = body }); return body;
    }
    private void Hint(StackPanel panel, string text) => panel.Children.Add(new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, Style = (Style)FindResource("Caption") });
    private void Field(StackPanel panel, string label, FrameworkElement input)
    {
        var row = new Grid { Margin = new Thickness(0, 4, 0, 4) };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1.25, GridUnitType.Star) });
        row.Children.Add(new TextBlock { Text = label, Margin = new Thickness(0, 0, 10, 0), TextWrapping = TextWrapping.Wrap });
        AutomationProperties.SetName(input, label); Grid.SetColumn(input, 1); row.Children.Add(input); panel.Children.Add(row);
    }
    private ComboBox Pick<T>(StackPanel panel, string label, IEnumerable<T> choices, T value, Action<T> changed, bool enabled = true, Func<T, string>? format = null)
    {
        var items = choices.ToArray();
        var box = new ComboBox { ItemsSource = items.Select(x => format?.Invoke(x) ?? x?.ToString() ?? "").ToArray(), IsEnabled = enabled };
        box.SelectedIndex = Array.IndexOf(items, value);
        box.SelectionChanged += (_, _) => { if (box.SelectedIndex >= 0) changed(items[box.SelectedIndex]); };
        Field(panel, label, box); return box;
    }
    private CheckBox Toggle(StackPanel panel, string text, bool value, Action<bool> changed, bool enabled = true)
    {
        var check = new CheckBox { Content = text, IsChecked = value, IsEnabled = enabled };
        check.Checked += (_, _) => changed(true); check.Unchecked += (_, _) => changed(false); panel.Children.Add(check); return check;
    }
    private Button ActionButton(StackPanel panel, string label, Action action, bool enabled = true)
    {
        var button = new Button { Content = label, IsEnabled = enabled, Margin = new Thickness(0, 5, 0, 0), HorizontalAlignment = System.Windows.HorizontalAlignment.Left };
        button.Click += (_, _) => action(); panel.Children.Add(button); return button;
    }
    private void SetDisplay(DisplaySettings display)
    {
        var profiles = new Dictionary<string, DisplaySettings>(settings.GraphicsProfiles ?? []) { [profile] = display };
        settings = settings with { GraphicsProfiles = profiles };
    }
    private ComboBox? qualityPresetFocus;
    private void GraphicsSettings()
    {
        if (profile.Length == 0) profile = RaceLaunch.Profile(settings);
        var display = Group("Display profile");
        Pick(display, "Settings for", new[] { "Normal", "Classic", "N64 Mode", "Remastered", "VR" }, profile, p => { profile = p; ShowSettings("Graphics"); });
        DisplaySettings Current() => RaceLaunch.Display(settings, profile);
        var resolution = Pick(display, "Window resolution", new[] { "1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160", Current().Resolution }.Distinct(),
            Current().Resolution, v => SetDisplay(Current() with { Resolution = v }), !Current().Borderless);
        Toggle(display, "Borderless fullscreen", Current().Borderless, v => { SetDisplay(Current() with { Borderless = v }); resolution.IsEnabled = !v; });
        if (profile != "VR") Pick(display, "Driving frame limit", new[] { 0, 30, 60, 90, 120, 144 }, Current().DesktopFps, v => SetDisplay(Current() with { DesktopFps = v }), format: v => v == 0 ? "Display sync" : $"{v} FPS");
        Toggle(display, "Hide HUD during races", settings.HideRaceHud, v => settings = settings with { HideRaceHud = v }, installed.Version >= GameContent.RaceHudVersion);
        if (profile == "VR")
        {
            var headset = Group("VR headset");
            var runtimes = OpenXrRuntime.Choices(settings.VrRuntime);
            var selected = runtimes.First(r => r.Id.Equals(settings.VrRuntime, StringComparison.OrdinalIgnoreCase));
            Pick(headset, "OpenXR runtime", runtimes, selected, r => settings = settings with { VrRuntime = r.Id });
            ActionButton(headset, "Choose runtime file…", () =>
            {
                var pick = new Microsoft.Win32.OpenFileDialog { Filter = "OpenXR runtime manifests (*.json)|*.json" };
                if (pick.ShowDialog(this) != true) return;
                try
                {
                    OpenXrRuntime.ReadManifest(pick.FileName);
                    settings = settings with { VrRuntime = pick.FileName };
                    ShowSettings("Graphics");
                }
                catch (Exception e) when (OpenXrRuntime.DiscoveryError(e)) { Say(e.Message); }
            });
            Hint(headset, OpenXrRuntime.LastSession(installed.Stage));
            Pick(headset, "Refresh rate", new[] { 0, 72, 80, 90, 120, 144 }, settings.VrRefresh, v => settings = settings with { VrRefresh = v }, format: v => v == 0 ? "Runtime default" : $"{v} Hz");
            Pick(headset, "Eye resolution", new[] { 60, 75, 85, 100 }, settings.VrScale, v => settings = settings with { VrScale = v }, format: v => $"{v}%");
            Toggle(headset, "Smooth vehicle motion", settings.SmoothVrMotion, v => settings = settings with { SmoothVrMotion = v }, installed.Version >= GameContent.VrStyleVersion);
        }
        var style = profile == "VR" ? settings.VrStyle : profile;
        if (style != "Remastered")
        {
            var raster = Group(style is "N64" or "N64 Mode" ? "N64 visual style" : "Classic visual style");
            if (style is "N64" or "N64 Mode") Toggle(raster, "320 × 240 raster (4:3)", settings.N64LowRaster, v => settings = settings with { N64LowRaster = v }, installed.Version >= GameContent.N64RasterVersion);
            else Toggle(raster, "SNES resolution (256 × 224, 4:3)", settings.ClassicSnes, v => settings = settings with { ClassicSnes = v }, installed.Version >= GameContent.ClassicRasterVersion);
            return;
        }
        bool Has(int version) => installed.Version >= version;
        bool vr = profile == "VR";
        var current = RemasteredQuality.Read(settings, vr);
        var preset = Pick(display, "Remastered quality", new[] { "Performance", "Balanced", "Quality", "Custom" }, current.Preset, name =>
        {
            if (name == current.Preset) return;
            current = name switch
            {
                "Performance" => RemasteredQuality.Performance,
                "Balanced" => RemasteredQuality.Balanced,
                "Quality" => RemasteredQuality.Quality,
                _ => current with { Preset = "Custom" }
            };
            settings = current.Store(settings, vr);
            if (name != "Custom")
            {
                ShowSettings("Graphics");
                qualityPresetFocus?.Focus();
            }
        });
        qualityPresetFocus = preset;
        void Edit(RemasteredQuality edited)
        {
            current = edited with { Preset = "Custom" };
            settings = current.Store(settings, vr);
            preset.SelectedItem = current.Preset;
        }
        var surface = Group("Surfaces and scenery");
        Pick(surface, "Anti-aliasing", new[] { 0, 2, 4, 8 }, current.RemasteredMsaa, v => Edit(current with { RemasteredMsaa = v }), Has(GameContent.AtmosphereVersion), v => v == 0 ? "Off" : $"{v}× MSAA");
        Toggle(surface, "High-detail road surfaces", current.HighDetailRoad, v => Edit(current with { HighDetailRoad = v }), Has(GameContent.RoadDetailVersion));
        Toggle(surface, "High-detail roadside scenery", current.HighDetailScenery, v => Edit(current with { HighDetailScenery = v }), Has(GameContent.ForestSceneryVersion));
        Toggle(surface, "Material roughness", current.MaterialRoughness, v => Edit(current with { MaterialRoughness = v }), Has(GameContent.MaterialFinishVersion));
        Toggle(surface, "Car clear coat", current.CarClearCoat, v => Edit(current with { CarClearCoat = v }), Has(GameContent.MaterialFinishVersion));
        var light = Group("Lighting and atmosphere");
        var shadow = Toggle(light, "Ray-traced scenery shadows", current.RayTracedShadows, v => Edit(current with { RayTracedShadows = v }), Has(GameContent.RayShadowsVersion));
        var quality = Pick(light, "Shadow quality", new[] { 1, 2, 4 }, current.ShadowRays, v => Edit(current with { ShadowRays = v }), current.RayTracedShadows && Has(GameContent.SunsetSceneryVersion),
            v => v == 1 ? "Performance · 1 ray" : v == 2 ? "Balanced · 2 rays" : "High · 4 rays");
        shadow.Checked += (_, _) => quality.IsEnabled = Has(GameContent.SunsetSceneryVersion); shadow.Unchecked += (_, _) => quality.IsEnabled = false;
        Toggle(light, "Ray-traced indirect lighting", current.RayTracedIndirect, v => Edit(current with { RayTracedIndirect = v }), Has(GameContent.IndirectLightVersion));
        Toggle(light, "Ray-traced wet-road reflections", current.RayTracedReflections, v => Edit(current with { RayTracedReflections = v }), Has(GameContent.RayReflectionsVersion));
        Toggle(light, "Volumetric lighting", current.VolumetricLighting, v => Edit(current with { VolumetricLighting = v }), Has(GameContent.VolumetricLightingVersion));
        Toggle(light, "Wet-weather tire spray", current.WetTireSpray, v => Edit(current with { WetTireSpray = v }), Has(GameContent.WetTireSprayVersion));
        var camera = Group("Camera");
        Pick(camera, "Starting camera", GameContent.CameraIds, settings.RemasteredCamera, v => settings = settings with { RemasteredCamera = v }, Has(GameContent.CameraVersion));
        Toggle(camera, "Mouse and right-stick camera control", settings.CameraLook, v => settings = settings with { CameraLook = v }, Has(GameContent.CameraVersion));
        var zoom = new Slider
        {
            Minimum = GameContent.MinCameraDistance,
            Maximum = GameContent.MaxCameraDistance,
            Value = settings.MaxCameraDistance,
            TickFrequency = 1,
            IsSnapToTickEnabled = true,
            IsEnabled = Has(GameContent.IsometricVersion),
            AutoToolTipPlacement = System.Windows.Controls.Primitives.AutoToolTipPlacement.TopLeft
        };
        var zoomLabel = new TextBlock { Text = $"{settings.MaxCameraDistance} m" };
        zoom.ValueChanged += (_, _) => { settings = settings with { MaxCameraDistance = (int)zoom.Value }; zoomLabel.Text = $"{settings.MaxCameraDistance} m"; };
        Field(camera, "Maximum zoom distance", zoom); camera.Children.Add(zoomLabel);
    }

    private void ControllerSettings(ControllerProfile? draft = null)
    {
        var panel = Group("Controller bindings");
        if (string.IsNullOrWhiteSpace(settings.OutputPath)) { Hint(panel, "Choose a build folder in Game files to configure a controller."); return; }
        string path;
        try { path = ControllerProfile.FilePath(settings.OutputPath); }
        catch (ArgumentException error) { Hint(panel, error.Message); return; }
        ControllerProfile config;
        try { config = draft ?? ControllerProfile.Load(path); }
        catch (Exception ex) when (ex is IOException or InvalidDataException or UnauthorizedAccessException) { config = new(); Hint(panel, ex.Message); }
        foreach (var binding in ControllerProfile.Bindings)
            Pick(panel, binding.Label, binding.Axis ? ControllerProfile.Axes : ControllerProfile.Buttons, config[binding.Key], v => config.Set(binding.Key, v), format: ControllerProfile.LabelFor);
        Pick(panel, "Steering response", new[] { "digital", "analog" }, config["steering_mode"], v => config.Set("steering_mode", v));
        var dead = new TextBox { Text = config["deadzone"] }; var trigger = new TextBox { Text = config["trigger_threshold"] };
        Field(panel, "Stick deadzone (1–30000)", dead); Field(panel, "Trigger threshold (1–30000)", trigger);
        ActionButton(panel, "Save controller settings", () =>
        {
            try { config.Set("deadzone", dead.Text); config.Set("trigger_threshold", trigger.Text); config.Save(path); Say("Controller settings saved."); }
            catch (Exception ex) { Say(ex.Message); }
        });
        ActionButton(panel, "Restore defaults", () => { SettingsBody.Children.Clear(); ControllerSettings(new()); });
        var keyboard = Group("Keyboard");
        Hint(keyboard, "W / Up   Accelerate       S / Down   Brake       A / D   Steer\nSpace   Boost       Hold Shift   Drift (Enhanced)       X   Reverse\nR   Retire or restart       P   Pause       F11   Fullscreen");
    }

    private readonly UpdatePrompt updatePrompt = new();
    private Task CheckUpdates(bool manual) => updatePrompt.Check(manual,
        () => !closed && IsVisible && loadError == null && build == null && !starting && !connecting && lobby is not { Closed: false },
        () => new System.Windows.Interop.WindowInteropHelper(this).Handle,
        () => { Program.SwitchInterface(settingsFile, CurrentSettings(), false); switchingInterface = true; Close(); });

    private GameRebuild? RebuildOffer => settings.OutputPath == loadedOutput ? GameRebuild.Detect(installed.Record, settings.RomPath) : null;

    private string? ChooseRom()
    {
        var pick = new Microsoft.Win32.OpenFileDialog { Filter = "SNES ROM (*.sfc;*.smc)|*.sfc;*.smc|All files|*.*" };
        return pick.ShowDialog(this) == true ? pick.FileName : null;
    }

    private void FilesSettings()
    {
        var updates = Group($"Launcher · Build {GameContent.CurrentVersion}");
        ActionButton(updates, "Check for updates", () => _ = CheckUpdates(true), build == null && lobby is not { Closed: false });
        var group = Group("Local game files");
        if (buildReadError != null) Hint(group, buildReadError);
        if (build == null && RebuildOffer is { } offer) Hint(group, offer.Message);
        var rom = new TextBox { Text = settings.RomPath, IsEnabled = build == null };
        var output = new TextBox { Text = settings.OutputPath, IsEnabled = build == null };
        Field(group, "Game ROM", rom); ActionButton(group, "Choose ROM…", () =>
        {
            if (ChooseRom() is string path) rom.Text = path;
        }, build == null);
        Field(group, "Build folder", output); ActionButton(group, "Choose folder…", () =>
        {
            var pick = new OpenFolderDialog(); if (pick.ShowDialog(this) == true) { output.Text = pick.FolderName; _ = ReadBuild(); }
        }, build == null);
        rom.TextChanged += (_, _) => settings = settings with { RomPath = rom.Text };
        output.TextChanged += (_, _) => settings = settings with { OutputPath = output.Text };
        output.LostKeyboardFocus += async (_, _) => { if (output.Text != loadedOutput && !string.IsNullOrWhiteSpace(output.Text)) await ReadBuild(); };
        ActionButton(group, "Verify ROM", () => _ = RunBuild(false), build == null);
        ActionButton(group, "Build game", () => _ = RunBuild(true), build == null);
        if (build != null) ActionButton(group, "Cancel build", () => build.Cancel());
        ActionButton(group, "Inspect models", () => { try { using var process = BuildPipeline.Launch(settings.OutputPath, inspectModels: true); } catch (Exception ex) { Say(ex.Message); } }, installed.Record != null && build == null);
        var log = Group("Build log"); visibleLog = new TextBox
        {
            Text = string.Join(Environment.NewLine, buildLog),
            IsReadOnly = true,
            TextWrapping = TextWrapping.Wrap,
            Height = 120,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto
        }; log.Children.Add(visibleLog);
    }
    private string? loadedOutput;
    private async Task RunBuild(bool compile)
    {
        if (build != null) return;
        if (compile && RebuildOffer is { } offer)
        {
            var path = offer.Rom ?? ChooseRom();
            if (path == null) return;
            settings = settings with { RomPath = path };
        }
        using var cancel = new CancellationTokenSource(); build = cancel;
        try
        {
            Save(); ShowSettings("Game files"); var snapshot = settings;
            var progress = new Progress<string>(line =>
            {
                if (closed) return; if (buildLog.Count >= 350) buildLog.Dequeue(); buildLog.Enqueue(line);
                if (visibleLog != null) { visibleLog.Text = string.Join(Environment.NewLine, buildLog); visibleLog.ScrollToEnd(); }
            });
            var pipeline = new BuildPipeline(BuildPipeline.FindSource(), ((IProgress<string>)progress).Report);
            if (compile) await pipeline.BuildAsync(snapshot.RomPath, snapshot.OutputPath, cancel.Token);
            else await pipeline.VerifyAsync(snapshot.RomPath, snapshot.OutputPath, cancel.Token);
            Say(compile ? "Build complete." : "ROM verified.");
        }
        catch (OperationCanceledException) { Say("Build cancelled."); }
        catch (Exception ex) { Say(ex.Message); }
        finally { build = null; if (closeAfterBuild) Close(); else await ReadBuild(); }
    }
}

