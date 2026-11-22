using System.Diagnostics;
using System.Text.Json;

namespace StuntRaceFX.Launcher;

internal static class CommandLine
{
    internal static async Task<int> RunAsync(string[] args)
    {
        try
        {
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
                        ((ComboBox)form.Controls.Find("DesktopFps", true).Single()).SelectedIndex = 2;
                        ((ComboBox)form.Controls.Find("VrRefresh", true).Single()).SelectedIndex = 4;
                        ((ComboBox)form.Controls.Find("VrScale", true).Single()).SelectedIndex = 2;
                        resolution.SelectedIndex = 3; mode.SelectedIndex = 1; form.Close();
                    });
                    form.ShowDialog();
                }
                using var restored = new MainForm(settings);
                if (((ComboBox)restored.Controls.Find("DesktopFps", true).Single()).Text != "60 FPS" ||
                    ((ComboBox)restored.Controls.Find("VrRefresh", true).Single()).Text != "120 Hz" ||
                    ((ComboBox)restored.Controls.Find("VrScale", true).Single()).Text != "75%")
                    throw new InvalidOperationException("Frame settings did not persist");
                if (((ComboBox)restored.Controls.Find("LaunchMode", true).Single()).Text != "Quick Race" ||
                    ((ComboBox)restored.Controls.Find("PlayerCar", true).Single()).Text != "F-TYPE" ||
                    ((ComboBox)restored.Controls.Find("Course", true).Single()).Text != GameContent.Courses[^1].Name ||
                    ((ComboBox)restored.Controls.Find("Resolution", true).Single()).Text != "3840 x 2160" ||
                    ((ComboBox)restored.Controls.Find("WindowMode", true).Single()).SelectedIndex != 1)
                    throw new InvalidOperationException("Saved selections did not restore");
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
                    File.WriteAllText(settings, JsonSerializer.Serialize(new LauncherSettings(OutputPath: output)));
                    using var form = new MainForm(settings);
                    var launch = (Button)form.Controls.Find("LaunchGame", true).Single();
                    var mode = (ComboBox)form.Controls.Find("LaunchMode", true).Single();
                    mode.SelectedItem = "Easy Ride VR";
                    if (launch.Text != "Launch VR" || launch.Enabled != (version >= 28))
                        throw new InvalidOperationException($"Wrong Easy Ride VR availability for content version {version}");
                    mode.SelectedItem = "Normal";
                    if (launch.Text != "Launch Game" || launch.Enabled != (version >= 2))
                        throw new InvalidOperationException($"Wrong Normal availability for content version {version}");
                    mode.SelectedItem = "Quick Race";
                    if (launch.Text != "Launch Quick Race" || !launch.Enabled)
                        throw new InvalidOperationException($"Wrong Quick Race availability for content version {version}");
                    if (version >= 2)
                    {
                        var courses = (ComboBox)form.Controls.Find("Course", true).Single();
                        courses.SelectedIndex = 1;
                        if (launch.Enabled) throw new InvalidOperationException("Quick Race enabled for a missing course");
                        mode.SelectedItem = "Normal";
                        if (!launch.Enabled) throw new InvalidOperationException("Missing Quick Race course disabled Normal");
                        mode.SelectedItem = "Quick Race";
                        if (launch.Enabled || courses.SelectedIndex != 1)
                            throw new InvalidOperationException("Switching modes lost the Quick Race selection");
                        foreach (var entry in GameContent.Courses.Skip(2))
                        {
                            var assets = GameContent.AssetDirectory(stage, entry.Id);
                            Directory.CreateDirectory(assets);
                            File.WriteAllText(Path.Combine(assets, "world.srfworld"), "fixture");
                            courses.SelectedItem = entry;
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
