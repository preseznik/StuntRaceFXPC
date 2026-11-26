using System.Diagnostics;
using System.Text.Json;

namespace StuntRaceFX.Launcher;

internal static class CommandLine
{
    internal static async Task<int> RunAsync(string[] args)
    {
        try
        {
            if (args[0] == "--test-lan-peer")
                return await LanRegression.Run(Required(args, "--output"), Required(args, "--capture"), Required(args, "--role"),
                    int.Parse(Required(args, "--port")), args.Contains("--force"), args.Contains("--drop-in"), Optional(args, "--course") ?? GameContent.DefaultCourse, Optional(args, "--discover-host"), Optional(args, "--discovered-address"));
            if (args[0] == "--test-child") return await TestChildAsync(args[1..]);
            if (args[0] == "--self-test") { await SelfTestAsync(Required(args, "--output")); return 0; }
            if (args[0] == "--configure-controller")
            {
                ApplicationConfiguration.Initialize();
                using var dialog = new ControllerDialog(ControllerProfile.FilePath(Required(args, "--output")));
                dialog.ShowDialog();
                return 0;
            }
            if (args[0] is "--ui-snapshot" or "--controller-snapshot")
            {
                ApplicationConfiguration.Initialize();
                Application.SetColorMode(Optional(args, "--theme") switch { "dark" => SystemColorMode.Dark, "light" => SystemColorMode.Classic, _ => SystemColorMode.System });
                using Form form = args[0] == "--controller-snapshot"
                    ? new ControllerDialog(Required(args, "--config")) : new MainForm(Optional(args, "--settings"));
                if (Optional(args, "--tab") is { } tab)
                    ((TabControl)form.Controls.Find("LauncherTabs", true).Single()).SelectedIndex = int.Parse(tab);
                if (Optional(args, "--width") is { } width && Optional(args, "--height") is { } height)
                    form.ClientSize = new Size(int.Parse(width), int.Parse(height));
                form.ShowInTaskbar = false;
                form.StartPosition = FormStartPosition.Manual;
                form.Location = new Point(-32000, -32000);
                form.Show();
                Application.DoEvents();
                using var bitmap = new Bitmap(form.Width, form.Height);
                form.DrawToBitmap(bitmap, form.ClientRectangle with { Width = form.Width, Height = form.Height });
                bitmap.Save(Required(args, "--output"), System.Drawing.Imaging.ImageFormat.Png);
                return 0;
            }
            if (args[0] is not ("--verify-rom" or "--build-preview" or "--build-race")) throw new ArgumentException("Use --verify-rom or --build-race, with --rom <file> --output <folder> [--source <folder>] [--cancel-after-ms <n>]. Or --configure-controller --output <folder>; --self-test --output <folder>.");
            var source = Optional(args, "--source") ?? BuildPipeline.FindSource();
            var pipeline = new BuildPipeline(source, Console.WriteLine);
            using var cancel = new CancellationTokenSource();
            if (Optional(args, "--cancel-after-ms") is { } milliseconds) cancel.CancelAfter(int.Parse(milliseconds));
            if (args[0] == "--verify-rom") await pipeline.VerifyAsync(Required(args, "--rom"), Required(args, "--output"), cancel.Token);
            else await pipeline.BuildAsync(Required(args, "--rom"), Required(args, "--output"), cancel.Token);
            return 0;
        }
        catch (OperationCanceledException) { Console.Error.WriteLine("Cancelled. Previous successful build preserved."); return 2; }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }

    private static string? Optional(string[] args, string name)
    {
        var index = Array.IndexOf(args, name);
        if (index < 0) return null;
        if (index + 1 == args.Length) throw new ArgumentException("Missing value for " + name);
        return args[index + 1];
    }
    private static string Required(string[] args, string name) => Optional(args, name) ?? throw new ArgumentException("Missing " + name);

    private static async Task<int> TestChildAsync(string[] args)
    {
        if (args[0] == "echo") { Console.WriteLine(JsonSerializer.Serialize(args[1..])); return 0; }
        if (args[0] == "fail") return 7;
        if (args[0] == "wait") { await Task.Delay(TimeSpan.FromMinutes(2)); return 0; }
        if (args[0] == "tree")
        {
            var info = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, CreateNoWindow = true };
            info.ArgumentList.Add("--test-child");
            info.ArgumentList.Add("wait");
            using var child = Process.Start(info)!;
            File.WriteAllText(args[1], JsonSerializer.Serialize(new[] { Environment.ProcessId, child.Id }));
            await child.WaitForExitAsync();
            return 0;
        }
        throw new ArgumentException("Unknown test child command.");
    }

    private static async Task SelfTestAsync(string output)
    {
        var directory = Path.Combine(Path.GetFullPath(output), "launcher-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        var checks = new List<string>();
        void Check(bool value, string label) { if (!value) throw new InvalidOperationException("FAIL: " + label); checks.Add(label); Console.WriteLine("PASS: " + label); }
        LauncherChecks(directory, Check);
        ReplayValidationChecks(Check);
        ControllerChecks(directory, Check);
        var executable = Environment.ProcessPath!;
        var arguments = new[] { "path with spaces", "č ž Unicode", "literal \"quote\"", "C:\\directory with spaces\\", "$(not-a-command);&|" };
        var echoed = await BuildPipeline.RunAsync(executable, new[] { "--test-child", "echo" }.Concat(arguments), directory, _ => { }, CancellationToken.None);
        Check(JsonSerializer.Deserialize<string[]>(echoed)!.SequenceEqual(arguments), "ArgumentList preserves spaces, Unicode, quotes and shell metacharacters");
        bool failed = false;
        try { await BuildPipeline.RunAsync(executable, ["--test-child", "fail"], directory, _ => { }, CancellationToken.None); }
        catch (InvalidOperationException error) { failed = error.Message.Contains("exit 7"); }
        Check(failed, "Nonzero process exits fail the build");

        var pointer = Path.Combine(directory, "last-success.json");
        BuildPipeline.AtomicWrite(pointer, "previous-success");
        using var cancel = new CancellationTokenSource();
        var pidFile = Path.Combine(directory, "child-pids.json");
        var running = BuildPipeline.RunAsync(executable, ["--test-child", "tree", pidFile], directory, _ => { }, cancel.Token);
        var timer = Stopwatch.StartNew();
        while (!File.Exists(pidFile) && timer.Elapsed < TimeSpan.FromSeconds(10)) await Task.Delay(50);
        Check(File.Exists(pidFile), "Cancellation fixture started a process tree");
        // File writes may still be completing when its directory entry first appears.
        int[]? ids = null;
        while (ids == null && timer.Elapsed < TimeSpan.FromSeconds(10))
        {
            try { ids = JsonSerializer.Deserialize<int[]>(File.ReadAllText(pidFile)); }
            catch (Exception error) when (error is IOException or JsonException) { await Task.Delay(25); }
        }
        cancel.Cancel();
        bool cancelled = false;
        try { await running.WaitAsync(TimeSpan.FromSeconds(10)); } catch (OperationCanceledException) { cancelled = true; }
        Check(cancelled, "Cancellation stops the command promptly");
        foreach (var id in ids ?? throw new InvalidDataException("Missing child PIDs"))
        {
            bool alive;
            try { using var process = Process.GetProcessById(id); alive = !process.HasExited; } catch (ArgumentException) { alive = false; }
            Check(!alive, "Cancellation killed process " + id);
        }
        Check(File.ReadAllText(pointer) == "previous-success", "Failed/cancelled commands do not modify success pointer");
        BuildPipeline.AtomicWrite(pointer, "new-success");
        Check(File.ReadAllText(pointer) == "new-success", "Success pointer replacement works");
        var malicious = new BuildRecord("../escape", "{}", "hash", "test", false, DateTimeOffset.UtcNow);
        BuildPipeline.AtomicWrite(pointer, JsonSerializer.Serialize(malicious));
        bool rejected = false;
        try { BuildPipeline.LastSuccess(directory); } catch (InvalidDataException) { rejected = true; }
        Check(rejected, "Build pointer rejects traversal");
        File.WriteAllText(Path.Combine(directory, "results.json"), JsonSerializer.Serialize(new { passed = checks.Count, checks }, BuildPipeline.JsonOptions));
    }

    private static void ReplayValidationChecks(Action<bool, string> check)
    {
        string RaceResult(GameCourse course) => JsonSerializer.Serialize(new
        {
            native_phases = course.ReplayPhases, finished = true, failure = 0, laps = 3, restart_deterministic = true
        });
        foreach (var course in GameContent.Courses)
        {
            BuildPipeline.ValidateRaceReplay(RaceResult(course) + "\n{\"screenshot_written\":true}\n", course);
            check(true, course.Name + " full-race result validates with screenshot diagnostics");
        }
        var easyRide = GameContent.Course("easy-ride");
        var valid = RaceResult(easyRide);
        foreach (var (label, result) in new[]
        {
            ("wrong phase count", valid.Replace("1637", "1638")),
            ("unfinished race", valid.Replace("\"finished\":true", "\"finished\":false")),
            ("race failure", valid.Replace("\"failure\":0", "\"failure\":1")),
            ("incomplete laps", valid.Replace("\"laps\":3", "\"laps\":2")),
            ("failed restart", valid.Replace("\"restart_deterministic\":true", "\"restart_deterministic\":false")),
            ("missing restart field", valid.Replace(",\"restart_deterministic\":true", "")),
            ("missing race result", "{\"screenshot_written\":true}"),
            ("duplicate race results", valid + "\n" + valid),
            ("malformed output", "not JSON")
        })
        {
            bool rejected = false;
            try { BuildPipeline.ValidateRaceReplay(result, easyRide); }
            catch (Exception error) when (error is InvalidDataException or JsonException or InvalidOperationException or KeyNotFoundException) { rejected = true; }
            check(rejected, "Build promotion rejects " + label);
        }
    }

    private static void LauncherChecks(string directory, Action<bool, string> check)
    {
        var legacy = new BuildRecord("test", "{}", "test", "test", true, DateTimeOffset.UtcNow);
        check(BuildPipeline.ContentArguments(legacy, "4wd", false).Length == 0, "Legacy builds launch without unsupported switches");
        foreach (var choice in new[] { ("coupe", false), ("f-type", false), ("4wd", true) })
        {
            bool rejected = false;
            try { BuildPipeline.ContentArguments(legacy, choice.Item1, choice.Item2); } catch (InvalidOperationException) { rejected = true; }
            check(rejected, "Legacy build requires rebuild for " + choice);
        }
        var current = legacy with { ContentVersion = 1 };
        check(BuildPipeline.ContentArguments(current, "f-type", true).SequenceEqual(new[] { "--car", "f-type", "--borderless" }), "Car and window mode reach the native process");
        bool invalid = false;
        try { BuildPipeline.ContentArguments(current, "unknown", false); } catch (ArgumentException) { invalid = true; }
        check(invalid, "Unknown car is rejected before launch");
        var gameBuild = legacy with { ContentVersion = 2 };
        check(BuildPipeline.FrameArguments(29, true, 60, 120, 75).SequenceEqual(new[]{"--vr-hz","120","--vr-scale","75"}) &&
              BuildPipeline.FrameArguments(29, false, 60, 120, 75).SequenceEqual(new[]{"--desktop-fps","60"}),
              "VR refresh and desktop FPS are routed to their own displays");
        check(BuildPipeline.FrameArguments(28, true, 60, 0, 100).Length == 0, "Previous VR build accepts default settings");
        bool oldFrames = false;
        try { BuildPipeline.FrameArguments(28, true, 0, 120, 75); } catch (InvalidOperationException) { oldFrames = true; }
        check(oldFrames,"Previous VR build requests a rebuild for custom settings");
        check(BuildPipeline.ContentArguments(gameBuild, "f-type", true).SequenceEqual(new[] { "--car", "f-type", "--borderless", "--course", "easy-ride" }),
            "Current builds receive the selected car, display mode and course");
        check(BuildPipeline.EntryPointArgument(gameBuild, true) == "--game" && BuildPipeline.EntryPointArgument(gameBuild, false) == "--race",
            "Normal and Quick Race use distinct native entry points");
        foreach (var previous in new[] { legacy, current })
        {
            bool rejected = false;
            try { BuildPipeline.EntryPointArgument(previous, true); } catch (InvalidOperationException) { rejected = true; }
            check(rejected && BuildPipeline.EntryPointArgument(previous, false) == "--race",
                $"Content version {previous.ContentVersion} retains Quick race without pretending to support game boot");
        }
        check(GameContent.AssetDirectory(directory, "easy-ride") == Path.Combine(directory, "generated"),
            "Easy Ride retains the legacy generated asset directory");
        foreach (var course in GameContent.Courses.Where(course => course.Id != GameContent.DefaultCourse))
        {
            bool rejected = false;
            var previous = legacy with { ContentVersion = course.MinimumContentVersion - 1 };
            var supported = legacy with { ContentVersion = course.MinimumContentVersion };
            try { BuildPipeline.ContentArguments(previous, "4wd", false, course.Id); } catch (InvalidOperationException) { rejected = true; }
            check(rejected && BuildPipeline.ContentArguments(supported, "coupe", false, course.Id).SequenceEqual(new[] { "--car", "coupe", "--course", course.Id }) &&
                GameContent.AssetDirectory(directory, course.Id) == Path.Combine(directory, "generated", course.Id),
                $"{course.Name} uses its own assets and requires a current build");
        }
        bool invalidCourse = false;
        try { BuildPipeline.ContentArguments(gameBuild, "4wd", false, "../escape"); } catch (ArgumentException) { invalidCourse = true; }
        check(invalidCourse, "Unknown or traversal course IDs are rejected before path construction");
        var settings = Path.Combine(directory, "launcher-settings.json");
        File.WriteAllText(settings, JsonSerializer.Serialize(new { RomPath = "user ROM.sfc", OutputPath = directory }));
        Exception? uiError = null;
        var ui = new Thread(() =>
        {
            try
            {
                using (var form = new MainForm(settings) { ShowInTaskbar = false, StartPosition = FormStartPosition.Manual, Location = new Point(-32000, -32000) })
                {
                    var car = (ComboBox)form.Controls.Find("PlayerCar", true).Single();
                    var course = (ComboBox)form.Controls.Find("Course", true).Single();
                    var resolution = (ComboBox)form.Controls.Find("Resolution", true).Single();
                    var mode = (ComboBox)form.Controls.Find("WindowMode", true).Single();
                    var launchMode = (ComboBox)form.Controls.Find("LaunchMode", true).Single();
                    if (launchMode.Text != "Normal" || car.Text != "4WD" || course.Text != "Easy Ride" || resolution.Text != "1920 x 1080" || mode.SelectedIndex != 0)
                        throw new InvalidOperationException("Legacy settings defaults changed");
                    var launch = (Button)form.Controls.Find("LaunchGame", true).Single();
                    if (launch.Enabled) throw new InvalidOperationException("Normal enabled without a build");
                    launchMode.SelectedItem = "Quick Race";
                    if (launch.Enabled) throw new InvalidOperationException("Quick Race enabled without a build");
                    form.Shown += (_, _) => form.BeginInvoke(() =>
                    {
                        car.SelectedIndex = 2; course.SelectedIndex = course.Items.Count - 1;
                        var target = (ComboBox)form.Controls.Find("DisplayTarget", true).Single();
                        target.SelectedItem = "VR";
                        ((ComboBox)form.Controls.Find("VrCourse", true).Single()).SelectedIndex = 1;
                        target.SelectedItem = "Desktop";
                        ((ComboBox)form.Controls.Find("DesktopFps", true).Single()).SelectedIndex = 2;
                        ((ComboBox)form.Controls.Find("VrRefresh", true).Single()).SelectedIndex = 4;
                        ((ComboBox)form.Controls.Find("VrScale", true).Single()).SelectedIndex = 2;
                        resolution.SelectedIndex = 3; mode.SelectedIndex = 1;
                        var profile = (ComboBox)form.Controls.Find("GraphicsProfile", true).Single();
                        profile.SelectedItem = "N64 Mode";
                        resolution.SelectedIndex = 0;
                        profile.SelectedItem = "Remastered";
                        ((CheckBox)form.Controls.Find("RayTracedShadows", true).Single()).Checked = true;
                        ((TrackBar)form.Controls.Find("ShadowQuality", true).Single()).Value = 1;
                        ((CheckBox)form.Controls.Find("HighDetailScenery", true).Single()).Checked = true;
                        ((CheckBox)form.Controls.Find("VolumetricLighting", true).Single()).Checked = true;
                        ((CheckBox)form.Controls.Find("WetTireSpray", true).Single()).Checked = true;
                        ((CheckBox)form.Controls.Find("HighDetailRoad", true).Single()).Checked = true;
                        ((CheckBox)form.Controls.Find("CarClearCoat", true).Single()).Checked = true;
                        ((CheckBox)form.Controls.Find("RayTracedIndirect", true).Single()).Checked = true;
                        ((CheckBox)form.Controls.Find("RayTracedReflections", true).Single()).Checked = true;
                        ((ComboBox)form.Controls.Find("RemasteredMsaa", true).Single()).SelectedIndex = 2;
                        ((ComboBox)form.Controls.Find("RemasteredWeather", true).Single()).SelectedItem = "Rain";
                        ((ComboBox)form.Controls.Find("RemasteredCamera", true).Single()).SelectedItem = "Isometric";
                        ((TrackBar)form.Controls.Find("CameraDistance", true).Single()).Value = 115;
                        ((CheckBox)form.Controls.Find("CameraLook", true).Single()).Checked = true;
                        ((ComboBox)form.Controls.Find("TimeOfDay", true).Single()).SelectedIndex = 17;
                        resolution.SelectedIndex = 2;
                        ((ComboBox)form.Controls.Find("DesktopFps", true).Single()).SelectedIndex = 3;
                        ((CheckBox)form.Controls.Find("HideRaceHud", true).Single()).Checked = true;
                        form.Close();
                    });
                    form.ShowDialog();
                }
                using var restored = new MainForm(settings);
                if (!((CheckBox)restored.Controls.Find("HideRaceHud", true).Single()).Checked)
                    throw new InvalidOperationException("Shared HUD visibility did not persist");
                if (((ComboBox)restored.Controls.Find("DesktopFps", true).Single()).Text != "60 FPS" ||
                    ((ComboBox)restored.Controls.Find("VrRefresh", true).Single()).Text != "120 Hz" ||
                    ((ComboBox)restored.Controls.Find("VrScale", true).Single()).Text != "75%")
                    throw new InvalidOperationException("Frame settings did not persist");
                if (((ComboBox)restored.Controls.Find("LaunchMode", true).Single()).Text != "Quick Race" ||
                    ((ComboBox)restored.Controls.Find("VrCourse", true).Single()).Text != "Aqua Tunnel" ||
                    ((ComboBox)restored.Controls.Find("PlayerCar", true).Single()).Text != "F-TYPE" ||
                    ((ComboBox)restored.Controls.Find("Course", true).Single()).Text != GameContent.Courses[^1].Name ||
                    ((ComboBox)restored.Controls.Find("Resolution", true).Single()).Text != "3840 x 2160" ||
                    ((ComboBox)restored.Controls.Find("WindowMode", true).Single()).SelectedIndex != 1)
                    throw new InvalidOperationException("Saved selections did not restore");
                var restoredProfile = (ComboBox)restored.Controls.Find("GraphicsProfile", true).Single();
                var restoredResolution = (ComboBox)restored.Controls.Find("Resolution", true).Single();
                restoredProfile.SelectedItem = "N64 Mode";
                if (restoredResolution.Text != "1280 x 720")
                    throw new InvalidOperationException("N64 graphics profile did not persist");
                restoredProfile.SelectedItem = "Remastered";
                if (((ComboBox)restored.Controls.Find("RemasteredCamera", true).Single()).Text != "Isometric" ||
                    ((TrackBar)restored.Controls.Find("CameraDistance", true).Single()).Value != 115 ||
                    !((CheckBox)restored.Controls.Find("CameraLook", true).Single()).Checked)
                    throw new InvalidOperationException("Remastered camera settings did not persist");
                if (!((CheckBox)restored.Controls.Find("VolumetricLighting", true).Single()).Checked)
                    throw new InvalidOperationException("Volumetric lighting setting did not persist");
                if (!((CheckBox)restored.Controls.Find("WetTireSpray", true).Single()).Checked)
                    throw new InvalidOperationException("Wet tire spray setting did not persist");
                if (!((CheckBox)restored.Controls.Find("HighDetailScenery", true).Single()).Checked)
                    throw new InvalidOperationException("Roadside scenery setting did not persist");
                if (!((CheckBox)restored.Controls.Find("HighDetailRoad", true).Single()).Checked)
                    throw new InvalidOperationException("High-detail road setting did not persist");
                if (!((CheckBox)restored.Controls.Find("CarClearCoat", true).Single()).Checked ||
                    ((CheckBox)restored.Controls.Find("MaterialRoughness", true).Single()).Checked)
                    throw new InvalidOperationException("Independent Remastered material settings did not persist");
                if (!((CheckBox)restored.Controls.Find("RayTracedIndirect", true).Single()).Checked)
                    throw new InvalidOperationException("Remastered indirect lighting did not persist");
                if (!((CheckBox)restored.Controls.Find("RayTracedReflections", true).Single()).Checked)
                    throw new InvalidOperationException("Remastered wet-road reflections did not persist");
                if (!((CheckBox)restored.Controls.Find("RayTracedShadows", true).Single()).Checked)
                    throw new InvalidOperationException("Remastered ray-traced shadows did not persist");
                if (((TrackBar)restored.Controls.Find("ShadowQuality", true).Single()).Value != 1)
                    throw new InvalidOperationException("Remastered shadow quality did not persist");
                if (((ComboBox)restored.Controls.Find("RemasteredMsaa", true).Single()).Text != "4× MSAA" ||
                    ((ComboBox)restored.Controls.Find("RemasteredWeather", true).Single()).Text != "Rain")
                    throw new InvalidOperationException("Remastered weather or anti-aliasing did not persist");
                if (((ComboBox)restored.Controls.Find("TimeOfDay", true).Single()).Text != "17:00")
                    throw new InvalidOperationException("Remastered time of day did not persist");
                if (restoredResolution.Text != "2560 x 1440" || ((ComboBox)restored.Controls.Find("DesktopFps", true).Single()).Text != "90 FPS")
                    throw new InvalidOperationException("Remastered graphics profile did not persist");
                var partial = Path.Combine(directory, "partial-profiles.json");
                File.WriteAllText(partial, JsonSerializer.Serialize(new LauncherSettings(Car: "coupe", Course: "aqua-tunnel", RemasteredMode: true,
                    GraphicsProfiles: new() { ["Quick Race"] = new("2560 x 1440"), ["N64 mode"] = new("1280 x 720"), ["Remastered"] = new("3840 x 2160") })));
                using var fresh = new MainForm(partial);
                var freshMode = (ComboBox)fresh.Controls.Find("LaunchMode", true).Single();
                var style = (ComboBox)fresh.Controls.Find("VisualStyle", true).Single();
                var carChoice = (ComboBox)fresh.Controls.Find("PlayerCar", true).Single();
                var display = (ComboBox)fresh.Controls.Find("Resolution", true).Single();
                if (freshMode.Text != "Quick Race" || style.Text != "Remastered" || carChoice.Text != "4WD" || display.Text != "3840 x 2160")
                    throw new InvalidOperationException("The former Remastered launch did not migrate to its Easy Ride 4WD race");
                style.SelectedItem = "N64 Mode";
                if (display.Text != "1280 x 720") throw new InvalidOperationException("The legacy N64 profile did not migrate");
                carChoice.SelectedIndex = 2;
                if (!style.Enabled || style.Text != "N64 Mode" || display.Text != "1280 x 720")
                    throw new InvalidOperationException("F-TYPE lost the selected Quick Race profile");
                carChoice.SelectedIndex = 0;
                if (style.Text != "N64 Mode") throw new InvalidOperationException("Returning to 4WD lost its visual style");
                freshMode.SelectedItem = "Normal";
                if (style.Text != "Classic" || display.Text != "1920 x 1080")
                    throw new InvalidOperationException("A new profile inherited another mode's resolution");
                style.SelectedItem = "Remastered";
                if (display.Text != "3840 x 2160")
                    throw new InvalidOperationException("Normal did not load the Remastered graphics profile");
                fresh.Shown += (_, _) => fresh.BeginInvoke(fresh.Close);
                fresh.ShowDialog();
                using var migrated = new MainForm(partial);
                var migratedMode = (ComboBox)migrated.Controls.Find("LaunchMode", true).Single();
                if (migratedMode.Text != "Normal" ||
                    ((ComboBox)migrated.Controls.Find("VisualStyle", true).Single()).Text != "Remastered")
                    throw new InvalidOperationException("The preferred Normal style did not survive reopening");
                migratedMode.SelectedItem = "Quick Race";
                if (((ComboBox)migrated.Controls.Find("VisualStyle", true).Single()).Text != "N64 Mode" ||
                    ((ComboBox)migrated.Controls.Find("Resolution", true).Single()).Text != "1280 x 720")
                    throw new InvalidOperationException("The preferred Quick Race style and profile did not survive reopening");
                foreach (var (quick, vr, normalVr, expectedMode) in new[] {
                    (false, false, false, "Normal"), (true, false, false, "Quick Race"),
                    (false, true, false, "Quick Race"), (false, true, true, "Normal") })
                {
                    File.WriteAllText(partial, JsonSerializer.Serialize(new LauncherSettings(
                        QuickRace: quick, EasyRideVr: vr, NormalVr: normalVr, VrCourse: "aqua-tunnel",
                        Car: "coupe", Course: "night-owl", NormalStyle: "N64", QuickRaceStyle: "Remastered",
                        GraphicsProfiles: new() { ["4WD VR"] = new("1280 x 720"), ["Original"] = new("3840 x 2160") })));
                    for (int pass = 0; pass < 2; ++pass)
                    {
                        using var savedForm = new MainForm(partial);
                        ComboBox Box(string name) => (ComboBox)savedForm.Controls.Find(name, true).Single();
                        var launchMode = Box("LaunchMode");
                        var target = Box("DisplayTarget");
                        if (!launchMode.Items.Cast<string>().SequenceEqual(new[] { "Normal", "Quick Race" }) ||
                            launchMode.Text != expectedMode || target.Text != (vr ? "VR" : "Desktop") ||
                            Box("VrCourse").Text != "Aqua Tunnel" || Box("Course").Text != "Night Owl" ||
                            Box("PlayerCar").Text != "COUPE" || Box("VrCar").Text != (pass == 0 ? "4WD" : "F-TYPE"))
                            throw new InvalidOperationException("Launch mode, display or course changed during migration");
                        target.SelectedItem = "VR";
                        if (Box("VisualStyle").Text != (pass == 0 ? "Classic" : "Remastered"))
                            throw new InvalidOperationException("VR style did not survive reopening");
                        Box("VisualStyle").SelectedItem = "Remastered";
                        Box("VrCar").SelectedIndex = 2;
                        if (Box("GraphicsProfile").Text != "VR" || Box("Resolution").Text != "1280 x 720")
                            throw new InvalidOperationException("VR graphics profile lost during migration");
                        target.SelectedItem = "Desktop";
                        if (Box("VisualStyle").Text != (expectedMode == "Normal" ? "N64 Mode" : "Remastered"))
                            throw new InvalidOperationException("Switching displays lost the desktop style");
                        Box("GraphicsProfile").SelectedItem = "Classic";
                        if (Box("Resolution").Text != "3840 x 2160")
                            throw new InvalidOperationException("Classic graphics profile lost during migration");
                        target.SelectedItem = vr ? "VR" : "Desktop";
                        savedForm.Shown += (_, _) => savedForm.BeginInvoke(savedForm.Close);
                        savedForm.ShowDialog();
                    }
                }
            }
            catch (Exception error) { uiError = error; }
        });
        ui.SetApartmentState(ApartmentState.STA); ui.Start(); ui.Join();
        if (uiError != null) throw new InvalidOperationException("Launcher UI test failed", uiError);
        var saved = JsonSerializer.Deserialize<LauncherSettings>(File.ReadAllText(settings))!;
        check(saved.RomPath == "user ROM.sfc" && saved.OutputPath == directory && saved.Car == "f-type" && saved.Borderless && saved.Course == GameContent.Courses[^1].Id && saved.QuickRace,
            "Native launcher defaults old profiles to Normal and restores the saved launch mode, race choices and paths");
        LauncherBuildCompatibilityChecks(directory, check);
    }

    private static void LauncherBuildCompatibilityChecks(string directory, Action<bool, string> check)
    {
        Exception? uiError = null;
        var ui = new Thread(() =>
        {
            try
            {
                for (var version = 0; version <= GameContent.CurrentVersion; ++version)
                {
                    var output = Path.Combine(directory, "compatibility-" + version);
                    var stage = Path.Combine(output, "builds", "test");
                    Directory.CreateDirectory(Path.Combine(stage, "native"));
                    Directory.CreateDirectory(Path.Combine(stage, "generated"));
                    foreach (var file in new[] { "srf_preview.exe", "SDL3.dll" }) File.WriteAllText(Path.Combine(stage, "native", file), "fixture");
                    foreach (var file in new[] { "world.srfworld", "sprites.srfsprites" }) File.WriteAllText(Path.Combine(stage, "generated", file), "fixture");
                    var record = new BuildRecord("test", "{}", "test", "test", true, DateTimeOffset.UtcNow) { ContentVersion = version };
                    File.WriteAllText(Path.Combine(output, "last-success.json"), JsonSerializer.Serialize(record));
                    var settings = Path.Combine(output, "launcher-settings.json");
                    File.WriteAllText(settings, version == 31
                        ? JsonSerializer.Serialize(new { OutputPath = output, Remastered = true })
                        : JsonSerializer.Serialize(new LauncherSettings(OutputPath: output, N64Mode: version >= 32)));
                    using var form = new MainForm(settings);
                    var launch = (Button)form.Controls.Find("LaunchGame", true).Single();
                    var mode = (ComboBox)form.Controls.Find("LaunchMode", true).Single();
                    var style = (ComboBox)form.Controls.Find("VisualStyle", true).Single();
                    if (version >= 31 && (mode.Text != "Quick Race" || style.Text != "N64 Mode" || !launch.Enabled))
                        throw new InvalidOperationException($"N64 mode selection lost for content version {version}");
                    mode.SelectedItem = "Quick Race";
                    var target = (ComboBox)form.Controls.Find("DisplayTarget", true).Single();
                    target.SelectedItem = "VR";
                    if (launch.Text != "Launch VR" || launch.Enabled != (version >= 28))
                        throw new InvalidOperationException($"Wrong Easy Ride VR availability for content version {version}");
                    var vrCourses = (ComboBox)form.Controls.Find("VrCourse", true).Single();
                    if (!vrCourses.Items.Cast<GameCourse>().Select(c => c.Id).SequenceEqual(GameContent.Courses.Select(c => c.Id)))
                        throw new InvalidOperationException("Wrong VR Quick Race course list");
                    vrCourses.SelectedIndex = 1;
                    if (launch.Enabled) throw new InvalidOperationException("Aqua Tunnel VR enabled without its world");
                    mode.SelectedItem = "Normal";
                    if (launch.Enabled != (version >= GameContent.SeamlessVersion))
                        throw new InvalidOperationException($"Wrong Normal VR availability for version {version}");
                    target.SelectedItem = "Desktop";
                    if (launch.Text != "Launch Game" || launch.Enabled != (version >= 2))
                        throw new InvalidOperationException($"Wrong Normal availability for content version {version}");
                    foreach (var look in new[] { "N64 Mode", "Remastered" })
                    {
                        style.SelectedItem = look;
                        if (launch.Enabled != (version >= GameContent.ChampionshipStyleVersion))
                            throw new InvalidOperationException($"Wrong Normal {look} availability for version {version}");
                    }
                    style.SelectedItem = "Classic";
                    var classicRaster = (CheckBox)form.Controls.Find("ClassicSnes", true).Single();
                    if (classicRaster.Checked || classicRaster.Enabled != (version >= GameContent.ClassicRasterVersion))
                        throw new InvalidOperationException($"Wrong Classic SNES default or availability for version {version}");
                    var hideHud = (CheckBox)form.Controls.Find("HideRaceHud", true).Single();
                    if (hideHud.Checked || hideHud.Enabled != (version >= GameContent.RaceHudVersion))
                        throw new InvalidOperationException($"Wrong race HUD default or availability for version {version}");
                    mode.SelectedItem = "Quick Race";
                    if (launch.Text != "Launch Quick Race" || !launch.Enabled)
                        throw new InvalidOperationException($"Wrong Quick Race availability for content version {version}");
                    if (version >= 2)
                    {
                        var courses = (ComboBox)form.Controls.Find("Course", true).Single();
                        courses.SelectedIndex = 1;
                        if (launch.Enabled) throw new InvalidOperationException("Quick Race enabled for a missing course");
                        if (!style.Enabled) throw new InvalidOperationException("Aqua Tunnel is missing its style selector");
                        courses.SelectedIndex = 0;
                        style.SelectedItem = "N64 Mode";
                        var carChoice = (ComboBox)form.Controls.Find("PlayerCar", true).Single();
                        carChoice.SelectedIndex = 1;
                        foreach (var look in new[] { "Remastered", "N64 Mode" })
                        {
                            style.SelectedItem = look;
                            if (!style.Enabled || launch.Enabled != (version >= GameContent.CoupeStyleVersion))
                                throw new InvalidOperationException($"Wrong COUPE {look} availability for version {version}");
                        }
                        carChoice.SelectedIndex = 2;
                        foreach (var look in new[] { "Remastered", "N64 Mode" })
                        {
                            style.SelectedItem = look;
                            if (!style.Enabled || launch.Enabled != (version >= GameContent.FTypeStyleVersion))
                                throw new InvalidOperationException($"Wrong F-TYPE {look} availability for version {version}");
                        }
                        carChoice.SelectedIndex = 0;
                        var lowRaster = (CheckBox)form.Controls.Find("N64LowRaster", true).Single();
                        if (lowRaster.Checked || lowRaster.Enabled != (version >= GameContent.N64RasterVersion))
                            throw new InvalidOperationException($"Wrong N64 raster default or availability for version {version}");
                        if (launch.Text != "Launch Quick Race" || launch.Enabled != (version >= GameContent.N64ModeVersion))
                            throw new InvalidOperationException($"Wrong Easy Ride (N64 mode) availability for content version {version}");
                        var timeOfDay = (ComboBox)form.Controls.Find("TimeOfDay", true).Single();
                        var roughness = (CheckBox)form.Controls.Find("MaterialRoughness", true).Single();
                        var volumes = (CheckBox)form.Controls.Find("VolumetricLighting", true).Single();
                        if (volumes.Enabled) throw new InvalidOperationException("N64 exposed volumetric lighting");
                        var tireSpray = (CheckBox)form.Controls.Find("WetTireSpray", true).Single();
                        var sceneryDetail = (CheckBox)form.Controls.Find("HighDetailScenery", true).Single();
                        var roadDetail = (CheckBox)form.Controls.Find("HighDetailRoad", true).Single();
                        var coat = (CheckBox)form.Controls.Find("CarClearCoat", true).Single();
                        var indirect = (CheckBox)form.Controls.Find("RayTracedIndirect", true).Single();
                        var reflections = (CheckBox)form.Controls.Find("RayTracedReflections", true).Single();
                        var rayShadows = (CheckBox)form.Controls.Find("RayTracedShadows", true).Single();
                        var msaa = (ComboBox)form.Controls.Find("RemasteredMsaa", true).Single();
                        var weather = (ComboBox)form.Controls.Find("RemasteredWeather", true).Single();
                        var camera = (ComboBox)form.Controls.Find("RemasteredCamera", true).Single();
                        var cameraLook = (CheckBox)form.Controls.Find("CameraLook", true).Single();
                        var distance = (TrackBar)form.Controls.Find("CameraDistance", true).Single();
                        if (distance.Enabled) throw new InvalidOperationException("N64 exposed the camera distance limit");
                        if (camera.Enabled || cameraLook.Enabled) throw new InvalidOperationException("N64 exposed Remastered camera settings");
                        if (roughness.Enabled || coat.Enabled || roadDetail.Enabled || sceneryDetail.Enabled || tireSpray.Enabled) throw new InvalidOperationException("N64 exposed material settings");
                        if (indirect.Enabled) throw new InvalidOperationException("N64 exposed indirect lighting");
                        if (reflections.Enabled) throw new InvalidOperationException("N64 exposed wet-road reflections");
                        if (rayShadows.Enabled) throw new InvalidOperationException("N64 exposed ray-traced shadows");
                        if (msaa.Enabled || weather.Enabled) throw new InvalidOperationException("N64 exposed Remastered weather or MSAA");
                        if (timeOfDay.Enabled) throw new InvalidOperationException("N64 exposed Remastered lighting");
                        style.SelectedItem = "Remastered";
                        if (camera.Text != "Chase" || cameraLook.Checked || camera.Enabled != (version >= GameContent.CameraVersion) || cameraLook.Enabled != camera.Enabled)
                            throw new InvalidOperationException($"Wrong Remastered camera defaults or availability for version {version}");
                        if (distance.Value != GameContent.DefaultCameraDistance || distance.Enabled != (version >= GameContent.IsometricVersion))
                            throw new InvalidOperationException($"Wrong camera distance defaults or availability for version {version}");
                        if (volumes.Checked || volumes.Enabled != (version >= GameContent.VolumetricLightingVersion))
                            throw new InvalidOperationException($"Wrong volumetric lighting default or availability for version {version}");
                        if (tireSpray.Checked || tireSpray.Enabled != (version >= GameContent.WetTireSprayVersion))
                            throw new InvalidOperationException($"Wrong wet tire spray default or availability for version {version}");
                        if (sceneryDetail.Checked || sceneryDetail.Enabled != (version >= GameContent.ForestSceneryVersion))
                            throw new InvalidOperationException($"Wrong roadside scenery default or availability for version {version}");
                        if (roadDetail.Checked || roadDetail.Enabled != (version >= GameContent.RoadDetailVersion))
                            throw new InvalidOperationException($"Wrong road detail default or availability for version {version}");
                        if (roughness.Checked || coat.Checked || roughness.Enabled != (version >= GameContent.MaterialFinishVersion) || coat.Enabled != roughness.Enabled)
                            throw new InvalidOperationException($"Wrong material settings for version {version}");
                        if (indirect.Checked || indirect.Enabled != (version >= GameContent.IndirectLightVersion))
                            throw new InvalidOperationException($"Wrong indirect lighting default or availability for version {version}");
                        if (reflections.Checked || reflections.Enabled != (version >= GameContent.RayReflectionsVersion))
                            throw new InvalidOperationException($"Wrong reflection defaults or availability for version {version}");
                        if (rayShadows.Checked || rayShadows.Enabled != (version >= GameContent.RayShadowsVersion))
                            throw new InvalidOperationException($"Wrong ray-traced shadow defaults or availability for version {version}");
                        if (msaa.Text != "Off" || weather.Text != "Clear" || msaa.Enabled != (version >= GameContent.AtmosphereVersion) || weather.Enabled != msaa.Enabled)
                            throw new InvalidOperationException($"Wrong Remastered graphics defaults or availability for version {version}");
                        if (timeOfDay.Text != "12:00" || timeOfDay.Enabled != (version >= GameContent.SunLightingVersion))
                            throw new InvalidOperationException($"Wrong time-of-day default or availability for version {version}");
                        if (launch.Text != "Launch Quick Race" || launch.Enabled != (version >= GameContent.RemasteredVersion) || lowRaster.Enabled)
                            throw new InvalidOperationException($"Wrong Remastered availability or raster settings for version {version}");
                        camera.SelectedItem = "Isometric";
                        if (launch.Enabled != (version >= GameContent.IsometricVersion))
                            throw new InvalidOperationException($"Wrong Isometric launch availability for version {version}");
                        camera.SelectedItem = "Chase";
                        courses.SelectedIndex = 1;
                        mode.SelectedItem = "Normal";
                        if (!launch.Enabled) throw new InvalidOperationException("Missing Quick Race course disabled Normal");
                        mode.SelectedItem = "Quick Race";
                        if (launch.Enabled || courses.SelectedIndex != 1)
                            throw new InvalidOperationException("Switching modes lost the Quick Race selection");
                        var aquaAssets = Directory.CreateDirectory(GameContent.AssetDirectory(stage, "aqua-tunnel"));
                        File.WriteAllText(Path.Combine(aquaAssets.FullName, "world.srfworld"), "fixture");
                        foreach (var look in new[] { "Classic", "N64 Mode", "Remastered" })
                        {
                            style.SelectedItem = look;
                            if (launch.Enabled != (look == "Classic" || version >= GameContent.AquaStyleVersion))
                                throw new InvalidOperationException($"Wrong Aqua Tunnel {look} availability for version {version}");
                        }
                        target.SelectedItem = "VR";
                        if (launch.Enabled != (version >= 30) || vrCourses.SelectedIndex != 1)
                            throw new InvalidOperationException($"Wrong Aqua Tunnel VR availability for content version {version}");
                        target.SelectedItem = "Desktop";
                        foreach (var entry in GameContent.Courses.Skip(2))
                        {
                            var assets = GameContent.AssetDirectory(stage, entry.Id);
                            Directory.CreateDirectory(assets);
                            File.WriteAllText(Path.Combine(assets, "world.srfworld"), "fixture");
                            // Check the whole VR list once every course fixture exists.
                            if (entry == GameContent.Courses[^1])
                            {
                                target.SelectedItem = "VR";
                                var vrCar = (ComboBox)form.Controls.Find("VrCar", true).Single();
                                foreach (var vrCourse in vrCourses.Items.Cast<GameCourse>())
                                {
                                    vrCourses.SelectedItem = vrCourse;
                                    for (int i = 0; i < 3; ++i)
                                    {
                                        vrCar.SelectedIndex = i;
                                        var required = Math.Max(vrCourse.VrVersion, i == 0 ? 0 : GameContent.AllCarsVrVersion);
                                        if (launch.Enabled != (version >= required))
                                            throw new InvalidOperationException($"Wrong VR availability for {vrCar.Text} on {vrCourse.Name}");
                                    }
                                }
                                target.SelectedItem = "Desktop";
                            }
                            courses.SelectedItem = entry;
                            foreach (var look in new[] { "Remastered", "N64 Mode", "Classic" })
                            {
                                style.SelectedItem = look;
                                var minimum = look == "Classic" ? entry.MinimumContentVersion : entry.Id switch
                                {
                                    "harbor-city" => GameContent.HarborCityStyleVersion,
                                    "sky-ramp" => GameContent.SkyRampStyleVersion,
                                    "big-ravine" => GameContent.BigRavineStyleVersion,
                                    "lake-side" => GameContent.LakeSideStyleVersion,
                                    "night-cruise" => GameContent.NightCruiseStyleVersion,
                                    "white-land" => GameContent.WhiteLandStyleVersion,
                                    "sea-breeze" => GameContent.SeaBreezeStyleVersion,
                                    "kings-forest" => GameContent.ForestStyleVersion,
                                    "night-owl" => GameContent.NightOwlStyleVersion,
                                    _ => GameContent.SunsetStyleVersion
                                };
                                if (launch.Enabled != (version >= minimum))
                                    throw new InvalidOperationException($"Wrong {entry.Name} {look} availability for version {version}");
                                if ((entry.Id is "night-owl" or "night-cruise" or "harbor-city") && look == "Remastered" && launch.Enabled &&
                                    (timeOfDay.Enabled || timeOfDay.Text != "12:00" || !weather.Enabled || !camera.Enabled))
                                    throw new InvalidOperationException("Night courses must lock the hour and keep other graphics controls available");
                            }
                            bool supported = version >= entry.MinimumContentVersion;
                            if (launch.Enabled != supported)
                                throw new InvalidOperationException($"{entry.Name} availability differs from the installed build");
                        }
                    }
                }
            }
            catch (Exception error) { uiError = error; }
        });
        ui.SetApartmentState(ApartmentState.STA); ui.Start(); ui.Join();
        if (uiError != null) throw new InvalidOperationException("Launcher build compatibility failed", uiError);
        check(true, "Launcher offers legacy Quick race, requires current game boot and disables missing course assets");
    }

    private static void ControllerChecks(string directory, Action<bool, string> check)
    {
        var file = ControllerProfile.FilePath(Path.Combine(directory, "controller settings č"));
        var defaults = ControllerProfile.Load(file);
        check(defaults["accelerate"] == "south" && defaults["deadzone"] == "8000" && defaults["steering_mode"] == "digital" && !File.Exists(file),
            "Missing controller profile uses defaults without creating a file");
        var profile = ControllerProfile.Parse("# comment\r\n; comment\r\n accelerate = A \r\nboost=none\nsteering_axis=RIGHTX\ndeadzone=1234\ntrigger_threshold=5678\n");
        check(profile["accelerate"] == "south" && profile["boost"] == "none" && profile["steering_axis"] == "rightx" && profile["brake"] == "east" && profile["steering_mode"] == "digital",
            "Controller aliases, comments, remapping and partial defaults round-trip");
        profile.Set("steering_mode", "ANALOG");
        profile.Save(file);
        var saved = File.ReadAllBytes(file);
        check(!saved.Take(3).SequenceEqual(new byte[] { 0xef, 0xbb, 0xbf }) && ControllerProfile.Load(file).Serialize() == profile.Serialize(),
            "Controller settings save atomically as UTF-8 without BOM in paths with spaces and Unicode");
        foreach (var invalid in new[] { "unknown=south", "accelerate=south\naccelerate=east", "Accelerate=south",
            "accelerate=bad-button", "steering_axis=south", "steering_mode=unknown", "deadzone=0", "deadzone=30001", "trigger_threshold=-1", "trigger_threshold=30001" })
        {
            bool rejected = false;
            try { ControllerProfile.Parse(invalid); } catch (InvalidDataException) { rejected = true; }
            check(rejected, "Invalid controller profile rejected: " + invalid.Replace('\n', ';'));
        }
        check(File.ReadAllBytes(file).SequenceEqual(saved), "Rejected controller settings leave existing profile unchanged");
        defaults.Set("deadzone", "1"); defaults.Set("trigger_threshold", "30000"); defaults.Save(file);
        check(ControllerProfile.Load(file).Serialize() == defaults.Serialize(), "Controller profile replacement and threshold boundaries work");
        // WinForms installs a synchronization context; keep it off the async CLI thread.
        Exception? dialogError = null;
        var uiThread = new Thread(() =>
        {
            try
            {
                void CancelDialog()
                {
                    using var dialog = new ControllerDialog(file)
                    {
                        ShowInTaskbar = false, StartPosition = FormStartPosition.Manual,
                        Location = new Point(-32000, -32000)
                    };
                    dialog.Shown += (_, _) => dialog.BeginInvoke(() => dialog.CancelButton!.PerformClick());
                    if (dialog.ShowDialog() != DialogResult.Cancel || dialog.Visible)
                        throw new InvalidOperationException("Controller Cancel did not close the dialog");
                }
                CancelDialog();
                if (File.ReadAllText(file) != defaults.Serialize()) throw new InvalidOperationException("Cancel changed valid profile");
                using (var dialog = new ControllerDialog(file)
                {
                    ShowInTaskbar = false, StartPosition = FormStartPosition.Manual,
                    Location = new Point(-32000, -32000)
                })
                {
                    dialog.Shown += (_, _) => dialog.BeginInvoke(() =>
                    {
                        ((ComboBox)dialog.Controls.Find("steering_mode", true).Single()).SelectedIndex = 1;
                        dialog.AcceptButton!.PerformClick();
                    });
                    if (dialog.ShowDialog() != DialogResult.OK || ControllerProfile.Load(file)["steering_mode"] != "analog")
                        throw new InvalidOperationException("Controller Save did not write proportional analog mode");
                }
                File.WriteAllText(file, "deadzone=broken\n");
                CancelDialog();
                if (File.ReadAllText(file) != "deadzone=broken\n") throw new InvalidOperationException("Cancel changed malformed profile");
            }
            catch (Exception error) { dialogError = error; }
        }) { IsBackground = true };
        uiThread.SetApartmentState(ApartmentState.STA);
        uiThread.Start();
        if (!uiThread.Join(TimeSpan.FromSeconds(10))) throw new TimeoutException("Controller dialog check timed out");
        if (dialogError != null) throw new InvalidOperationException("Controller dialog check failed", dialogError);
        check(true, "Cancelling controller dialog preserves both valid and malformed saved profiles");
        check(true, "Controller dialog selection saves proportional analog mode");
    }
}
