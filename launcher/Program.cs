using System.Diagnostics;
using System.Text.Json;

namespace StuntRaceFX.Launcher;

internal static class Program
{
    internal static void SwitchInterface(string path, LauncherSettings current, bool classic)
    {
        // The apphost has the same name in framework-dependent and single-file builds.
        var executable = Path.Combine(AppContext.BaseDirectory, "StuntRaceFX.Launcher.exe");
        var start = new ProcessStartInfo(executable) { UseShellExecute = false };
        start.ArgumentList.Add(classic ? "--classic" : "--modern");
        start.ArgumentList.Add("--settings");
        start.ArgumentList.Add(Path.GetFullPath(path));
        void Store(LauncherSettings value) => BuildPipeline.AtomicWrite(path, JsonSerializer.Serialize(value, BuildPipeline.JsonOptions));
        Store(current with { ClassicLauncher = classic });
        try { using var child = Process.Start(start) ?? throw new InvalidOperationException("Could not restart the launcher."); }
        catch { Store(current); throw; }
    }

    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length != 0 && args[0] is not ("--classic" or "--modern"))
            return CommandLine.RunAsync(args).GetAwaiter().GetResult();
        if (args.Length > 1 && (args.Length != 3 || args[1] != "--settings"))
            return 2;

        string path = args.Length == 3 ? Path.GetFullPath(args[2]) : Path.Combine(AppContext.BaseDirectory, "launcher-settings.json");
        LauncherSettings settings;
        string? error = null;
        try { settings = RaceLaunch.Read(path); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
        {
            settings = new LauncherSettings(OutputPath: Path.Combine(AppContext.BaseDirectory, "user-builds"));
            error = $"Could not load launcher settings. {e.Message}";
        }
        if (args.FirstOrDefault() == "--classic" || (args.Length == 0 && settings.ClassicLauncher))
        {
            ApplicationConfiguration.Initialize();
            Application.SetColorMode(SystemColorMode.System);
            Application.Run(new MainForm(path));
            return 0;
        }
        return new System.Windows.Application().Run(new Modern.GridWindow(path, settings, error));
    }
}
