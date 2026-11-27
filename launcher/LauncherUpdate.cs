using System.Diagnostics;
using System.Globalization;
using System.IO.Compression;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text.Json;

namespace StuntRaceFX.Launcher;

internal sealed record LauncherRelease(int Build, long Size, string Digest)
{
    private const string Repository = "preseznik/StuntRaceFXPC";
    internal string FileName => $"StuntRaceFXPC-Portable-Launcher-Build-{Build}.zip";
    internal Uri Download => new($"https://github.com/{Repository}/releases/download/build-{Build}/{FileName}");
    private static readonly HttpClient Client = new() { Timeout = TimeSpan.FromMinutes(15) };

    static LauncherRelease() => Client.DefaultRequestHeaders.UserAgent.ParseAdd($"StuntRaceFXPC/{GameContent.CurrentVersion}");

    internal static async Task<LauncherRelease?> LatestAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        using var json = JsonDocument.Parse(await Client.GetByteArrayAsync(
            $"https://api.github.com/repos/{Repository}/releases/latest", timeout.Token));
        return Parse(json.RootElement, GameContent.CurrentVersion);
    }

    internal static LauncherRelease? Parse(JsonElement release, int current)
    {
        if (release.GetProperty("draft").GetBoolean() || release.GetProperty("prerelease").GetBoolean()) return null;
        var tag = release.GetProperty("tag_name").GetString() ?? "";
        if (!tag.StartsWith("build-", StringComparison.Ordinal) ||
            !int.TryParse(tag.AsSpan(6), NumberStyles.None, CultureInfo.InvariantCulture, out int build) || tag != $"build-{build}")
            throw new InvalidDataException("The latest GitHub release has an unrecognized build number.");
        if (build <= current) return null;
        foreach (var asset in release.GetProperty("assets").EnumerateArray())
        {
            if (asset.GetProperty("name").GetString() != $"StuntRaceFXPC-Portable-Launcher-Build-{build}.zip") continue;
            var digest = asset.GetProperty("digest").GetString() ?? "";
            var size = asset.GetProperty("size").GetInt64();
            if (size > 0 && digest.StartsWith("sha256:", StringComparison.Ordinal) && digest.Length == 71 && digest[7..].All(Uri.IsHexDigit))
                return new(build, size, digest[7..]);
        }
        throw new InvalidDataException($"GitHub has no verified portable ZIP for Build {build}.");
    }

    internal async Task DownloadAsync(string root, IProgress<string> progress, CancellationToken cancel)
    {
        using var response = await Client.GetAsync(Download, HttpCompletionOption.ResponseHeadersRead, cancel);
        response.EnsureSuccessStatusCode();
        using var stream = await response.Content.ReadAsStreamAsync(cancel);
        await InstallAsync(root, stream, progress, cancel);
    }

    internal async Task InstallAsync(string root, Stream download, IProgress<string> progress, CancellationToken cancel)
    {
        var updates = Directory.CreateDirectory(Path.Combine(root, LauncherUpdate.Folder)).FullName;
        using var updateLock = new FileStream(Path.Combine(updates, "install.lock"), FileMode.OpenOrCreate, FileAccess.Write, FileShare.None);
        if (LauncherUpdate.ActiveBuild(root) >= Build) return;
        var stage = Directory.CreateDirectory(Path.Combine(updates, $"pending-{Guid.NewGuid():N}")).FullName;
        try
        {
            var zip = Path.Combine(stage, "download.zip");
            using (var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256))
            using (var output = new FileStream(zip, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                long total = 0;
                var buffer = new byte[128 * 1024];
                int count, previous = -1;
                while ((count = await download.ReadAsync(buffer, cancel)) > 0)
                {
                    total += count;
                    if (total > Size) throw new InvalidDataException("The update exceeds its published size.");
                    await output.WriteAsync(buffer.AsMemory(0, count), cancel);
                    hash.AppendData(buffer, 0, count);
                    int percent = (int)(100.0 * total / Size);
                    if (percent != previous) { progress.Report($"Downloading Build {Build} · {percent}%"); previous = percent; }
                }
                if (total != Size || !Convert.ToHexString(hash.GetHashAndReset()).Equals(Digest, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("The update failed SHA-256 or size verification. Please retry.");
            }
            progress.Report($"Unpacking Build {Build}…");
            var package = Path.Combine(stage, "package");
            await ZipFile.ExtractToDirectoryAsync(zip, package, cancel);
            foreach (var required in new[] { LauncherUpdate.Executable, "source/CMakeLists.txt", "source/.tools/toolchain.json" })
                if (!File.Exists(Path.Combine(package, required))) throw new InvalidDataException($"The release is missing {required}.");
            cancel.ThrowIfCancellationRequested();
            // A unique destination also allows retry after interruption between the move and pointer write.
            var installed = $"build-{Build}-{Guid.NewGuid():N}";
            Directory.Move(package, Path.Combine(updates, installed));
            BuildPipeline.AtomicWrite(Path.Combine(updates, "current"), installed);
        }
        finally
        {
            try { Directory.Delete(stage, true); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
    }
}

internal static class LauncherUpdate
{
    internal const string Folder = ".launcher-updates";
    internal const string Executable = "StuntRaceFX.Launcher.exe";
    private static readonly DirectoryInfo Running = new(AppContext.BaseDirectory);
    internal static string Home => Running.Parent?.Name == Folder && BuildNumber(Running.Name) > 0
        ? Running.Parent.Parent!.FullName : Running.FullName;
    internal static bool Portable => File.Exists(Path.Combine(Running.FullName, "source", ".tools", "toolchain.json"))
        && string.Equals(Path.GetFileName(Environment.ProcessPath), Executable, StringComparison.OrdinalIgnoreCase)
        && !File.Exists(Path.Combine(Running.FullName, "StuntRaceFX.Launcher.dll"));

    private static int BuildNumber(string name)
    {
        var fields = name.Split('-');
        return fields.Length == 3 && fields[0] == "build" && Guid.TryParseExact(fields[2], "N", out _) &&
            int.TryParse(fields[1], NumberStyles.None, CultureInfo.InvariantCulture, out int n) ? n : 0;
    }

    internal static string? ActiveDirectory(string root)
    {
        var pointer = Path.Combine(root, Folder, "current");
        if (!File.Exists(pointer)) return null;
        var name = File.ReadAllText(pointer);
        if (BuildNumber(name) <= 0) return null;
        var directory = Path.Combine(root, Folder, name);
        return File.Exists(Path.Combine(directory, Executable)) ? directory : null;
    }

    internal static int ActiveBuild(string root) => BuildNumber(Path.GetFileName(ActiveDirectory(root)) ?? "");

    internal static int? Forward(string[] args)
    {
        var directory = ActiveDirectory(Home);
        if (directory == null || BuildNumber(Path.GetFileName(directory)) <= GameContent.CurrentVersion ||
            Path.TrimEndingDirectorySeparator(Running.FullName).Equals(directory, StringComparison.OrdinalIgnoreCase)) return null;
        var info = new ProcessStartInfo(Path.Combine(directory, Executable)) { UseShellExecute = false };
        foreach (var arg in args) info.ArgumentList.Add(arg);
        using var child = Process.Start(info) ?? throw new IOException("Could not start the updated launcher.");
        if (args.Length == 0 || args[0] is "--classic" or "--modern") return 0;
        child.WaitForExit();
        return child.ExitCode;
    }
}
