namespace StuntRaceFX.Launcher;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length != 0)
            return CommandLine.RunAsync(args).GetAwaiter().GetResult();
        ApplicationConfiguration.Initialize();
        Application.SetColorMode(SystemColorMode.System);
        Application.Run(new MainForm());
        return 0;
    }
}
