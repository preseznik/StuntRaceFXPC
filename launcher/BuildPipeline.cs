using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace StuntRaceFX.Launcher;

internal sealed record Toolchain
{
    public int SchemaVersion { get; init; }
    public string Name { get; init; } = "";
    public bool DevelopmentOnly { get; init; }
    public string Compiler { get; init; } = "";
    public string CMake { get; init; } = "";
    public string Ninja { get; init; } = "";
    public Dictionary<string, string> FileDigests { get; init; } = [];

    public static Toolchain Load(string source)
    {
        var tools = Path.Combine(source, ".tools");
        var file = Path.Combine(tools, "toolchain.json");
        if (!File.Exists(file))
            throw new InvalidOperationException("Compiler bundle is missing. Run tools/Bootstrap-Toolchain.ps1, or use a launcher package with its compiler bundle included.");
        var config = JsonSerializer.Deserialize<Toolchain>(File.ReadAllText(file), BuildPipeline.JsonOptions)
            ?? throw new InvalidDataException("Invalid toolchain.json.");
        if (config.SchemaVersion != 1 || config.FileDigests.Count != 3)
            throw new InvalidDataException("Unsupported or incomplete toolchain manifest.");
        string Resolve(string path) => Path.GetFullPath(path, tools);
        foreach (var executable in new[] { config.Compiler, config.CMake, config.Ninja })
        {
            if (!config.FileDigests.TryGetValue(executable, out var expected))
                throw new InvalidDataException($"Missing tool hash: {executable}");
            var resolved = Resolve(executable);
            if (!config.DevelopmentOnly && !BuildPipeline.IsWithin(resolved, tools))
                throw new InvalidDataException("A portable toolchain must be inside its .tools directory.");
            using var stream = File.OpenRead(resolved);
            var actual = Convert.ToHexString(SHA256.HashData(stream));
            if (!string.Equals(expected, actual, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException($"Tool checksum mismatch: {executable}");
        }
        using var dependencies = JsonDocument.Parse(File.ReadAllText(Path.Combine(tools, "dependencies.json")));
        var dependencyRoot = dependencies.RootElement;
        var sdl = dependencyRoot.GetProperty("sdl");
        if (dependencyRoot.GetProperty("schemaVersion").GetInt32() != 1 || sdl.GetProperty("version").GetString() != "3.4.16")
            throw new InvalidDataException("Unsupported SDL dependency manifest.");
        var runtime = Path.GetFullPath(Path.Combine(sdl.GetProperty("sdk").GetString()!, "bin", "SDL3.dll"), tools);
        if (!BuildPipeline.IsWithin(runtime, tools)) throw new InvalidDataException("SDL runtime must be inside the local compiler bundle.");
        using (var runtimeFile = File.OpenRead(runtime))
        {
            var digest = Convert.ToHexString(SHA256.HashData(runtimeFile));
            if (!string.Equals(digest, sdl.GetProperty("runtimeSha256").GetString(), StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("SDL runtime checksum mismatch.");
        }
        if (!dependencyRoot.TryGetProperty("openxr", out var xr) || xr.GetProperty("version").GetString() != "1.1.53")
            throw new InvalidDataException("Missing OpenXR SDK. Restore the portable compiler bundle.");
        using var loader = File.OpenRead(Path.Combine(tools, "openxr-1.1.53", "lib", "libopenxr_loader.a"));
        if (!string.Equals(Convert.ToHexString(SHA256.HashData(loader)), xr.GetProperty("librarySha256").GetString(), StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("OpenXR loader checksum mismatch.");
        return config with { Compiler = Resolve(config.Compiler), CMake = Resolve(config.CMake), Ninja = Resolve(config.Ninja) };
    }
}

internal sealed record BuildRecord(string Directory, string RomManifest, string SourceSha256,
    string Toolchain, bool DevelopmentOnly, DateTimeOffset BuiltAt)
{
    public string RomPath { get; init; } = "";
    public int ContentVersion { get; init; }
}

internal sealed class BuildPipeline(string source, Action<string> report)
{
    internal static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true, PropertyNameCaseInsensitive = true };
    private readonly string source = Path.GetFullPath(source);

    public static string FindSource()
    {
        foreach (var start in new[] { AppContext.BaseDirectory, Environment.CurrentDirectory })
        {
            for (var directory = new DirectoryInfo(start); directory != null; directory = directory.Parent)
            {
                if (File.Exists(Path.Combine(directory.FullName, "source", "CMakeLists.txt")))
                    return Path.Combine(directory.FullName, "source");
                if (File.Exists(Path.Combine(directory.FullName, "CMakeLists.txt")) &&
                    File.Exists(Path.Combine(directory.FullName, "src", "romtool.cpp")))
                    return directory.FullName;
            }
        }
        throw new DirectoryNotFoundException("Cannot locate the native sources. Keep the launcher beside its source folder, or run it from this checkout.");
    }

    public async Task<string> VerifyAsync(string rom, string output, CancellationToken cancel)
    {
        ValidateInputs(rom, output);
        output = Path.GetFullPath(output);
        Directory.CreateDirectory(output);
        using var buildLock = new FileStream(Path.Combine(output, ".build.lock"), FileMode.OpenOrCreate,
            FileAccess.ReadWrite, FileShare.None);
        var toolchain = Toolchain.Load(source);
        LogToolchain(toolchain);
        var romTool = await EnsureRomToolAsync(toolchain, output, cancel);
        return await RunAsync(romTool, ["verify", "--rom", Path.GetFullPath(rom)], source, report, cancel);
    }

    public async Task<BuildRecord> BuildAsync(string rom, string output, CancellationToken cancel)
    {
        ValidateInputs(rom, output);
        output = Path.GetFullPath(output);
        Directory.CreateDirectory(output);
        // One writer per output directory. Existing successful builds remain launchable.
        using var buildLock = new FileStream(Path.Combine(output, ".build.lock"), FileMode.OpenOrCreate,
            FileAccess.ReadWrite, FileShare.None);
        var toolchain = Toolchain.Load(source);
        LogToolchain(toolchain);
        var id = DateTime.UtcNow.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N")[..8];
        var stage = Path.Combine(output, "builds", id);
        Directory.CreateDirectory(stage);
        using var log = new StreamWriter(Path.Combine(stage, "build.log"), false, Encoding.UTF8) { AutoFlush = true };
        var logGate = new object();
        void Log(string line)
        {
            lock (logGate) log.WriteLine(line);
            report(line);
        }
        try
        {
            Log("Native Stunt Race FX development build. Original courses, cars and opponents; the full game and VR are still in development.");
            Log($"Staging: {stage}");
            var romTool = await EnsureRomToolAsync(toolchain, output, cancel, Log);
            var manifest = await RunAsync(romTool, ["verify", "--rom", Path.GetFullPath(rom)], source, Log, cancel);
            using var verified = JsonDocument.Parse(manifest);
            var generated = Path.Combine(stage, "generated");
            await RunAsync(romTool, ["generate", "--rom", Path.GetFullPath(rom), "--output", generated], source, Log, cancel);
            var native = Path.Combine(stage, "native");
            await ConfigureAsync(toolchain, native, generated, Log, cancel);
            await RunAsync(toolchain.CMake, ["--build", native, "--target", "srf_preview", "--parallel", "4"], source, Log, cancel, toolchain);
            var executable = Path.Combine(native, "srf_preview.exe");
            foreach (var course in GameContent.Courses)
            {
                var assets = GameContent.AssetDirectory(stage, course.Id);
                var world = Path.Combine(assets, "world.srfworld");
                await RunAsync(executable, ["--assets", Path.Combine(assets, "models.srfmesh"), "--world", world, "--course", course.Id,
                    "--validate-assets"], native, Log, cancel, toolchain);
                foreach (var car in new[] { "4wd", "coupe", "f-type" })
                    await RunAsync(executable, ["--world", world, "--race", "--rom", Path.GetFullPath(rom),
                        "--course", course.Id, "--car", car, "--steps", "50", "--width", "960", "--height", "540",
                        "--restart-check", "--screenshot-no-hud", "--screenshot", Path.Combine(stage, $"preview-{course.Id}-{car}.png")], native, Log, cancel, toolchain);
                var replay = Path.Combine(source, "tests", "fixtures", course.Id.Replace('-', '_') + "_clean_inputs.csv");
                if (GameContent.SupportsEnhancedStyles(course.Id, "4wd"))
                    foreach (var style in new[] { RaceStyle.N64, RaceStyle.Remastered })
                        await RunAsync(executable, ["--world", world, "--course", course.Id, "--race", style == RaceStyle.N64 ? "--n64" : "--remastered", "--rom", Path.GetFullPath(rom),
                        "--replay-input", replay, "--steps", course.Id switch { "aqua-tunnel" => "380", "sunset-valley" => "1391", "night-owl" => "388", "kings-forest" => "1853", "sea-breeze" => "505", "white-land" => "1994", "night-cruise" => "1609", "lake-side" => "458", "big-ravine" => "605", "sky-ramp" => "1165", "harbor-city" => "2200", _ => "245" }, "--width", "960", "--height", "540", "--screenshot-no-hud",
                        "--screenshot", Path.Combine(stage, GameContent.StylePreview(course.Id, style, GameContent.CurrentVersion))], native, Log, cancel, toolchain);
                var result = await RunAsync(executable, ["--world", world, "--race", "--rom", Path.GetFullPath(rom),
                    "--course", course.Id, "--car", "4wd", "--replay-input", replay, "--restart-check",
                    "--screenshot", Path.Combine(stage, $"race-check-{course.Id}.png")], native, Log, cancel, toolchain);
                ValidateRaceReplay(result, course);
                Log($"{course.Name}: complete 3-lap race and deterministic restart validated ({course.ReplayPhases} native phases).");
            }
            await RunAsync(executable, ["--world", Path.Combine(generated, "world.srfworld"), "--game", "--rom", Path.GetFullPath(rom),
                "--course", GameContent.DefaultCourse, "--screenshot", Path.Combine(stage, "native-smoke.png")], native, Log, cancel, toolchain);
            cancel.ThrowIfCancellationRequested();
            var record = new BuildRecord(id, verified.RootElement.GetRawText(), SourceDigest(),
                toolchain.Name, toolchain.DevelopmentOnly, DateTimeOffset.UtcNow) { RomPath = Path.GetFullPath(rom), ContentVersion = GameContent.CurrentVersion };
            AtomicWrite(Path.Combine(stage, "build-record.json"), JsonSerializer.Serialize(record, JsonOptions));
            // Promotion is the final operation: no failed or cancelled build replaces the pointer.
            cancel.ThrowIfCancellationRequested();
            AtomicWrite(Path.Combine(output, "last-success.json"), JsonSerializer.Serialize(record, JsonOptions));
            Log("Build validated. Launch Game is ready.");
            return record;
        }
        catch (OperationCanceledException)
        {
            Log("Build cancelled. The previous successful build is preserved.");
            throw;
        }
        catch (Exception error)
        {
            Log($"Build failed: {error.Message}\nThe previous successful build is preserved.");
            throw;
        }
    }

    internal static void ValidateRaceReplay(string output, GameCourse course)
    {
        var found = false;
        foreach (var line in output.Split('\n', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries))
        {
            using var document = JsonDocument.Parse(line);
            var result = document.RootElement;
            if (!result.TryGetProperty("native_phases", out var phases)) continue;
            if (found || phases.GetInt32() != course.ReplayPhases || !result.GetProperty("finished").GetBoolean() ||
                result.GetProperty("failure").GetInt32() != 0 || result.GetProperty("laps").GetInt32() != 3 ||
                !result.GetProperty("restart_deterministic").GetBoolean())
                throw new InvalidDataException($"{course.Name} failed full-race and restart validation. The previous build is preserved.");
            found = true;
        }
        if (!found) throw new InvalidDataException($"{course.Name} did not report a completed native race.");
    }

    private void LogToolchain(Toolchain tools) => report(tools.Name +
        (tools.DevelopmentOnly ? " — DEVELOPMENT TOOLS: this package is not portable." : " — pinned portable compiler bundle."));

    private async Task<string> EnsureRomToolAsync(Toolchain tools, string output, CancellationToken cancel, Action<string>? log = null)
    {
        log ??= report;
        // Always run CMake/build so source changes cannot leave a stale extractor in use.
        // CMake caches absolute paths: relocation needs a fresh cache even if source bytes match.
        var cacheKey = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(
            source + "\0" + Path.GetFullPath(output) + "\0" + SourceDigest())))[..16].ToLowerInvariant();
        var native = Path.Combine(Path.GetFullPath(output), "extractor", cacheKey);
        await ConfigureAsync(tools, native, null, log, cancel);
        await RunAsync(tools.CMake, ["--build", native, "--target", "srf_romtool", "--parallel", "4"], source, log, cancel, tools);
        return Path.Combine(native, "srf_romtool.exe");
    }

    private Task<string> ConfigureAsync(Toolchain tools, string build, string? generated, Action<string> log, CancellationToken cancel)
    {
        List<string> args = ["-S", source, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF",
            "-DCMAKE_CXX_COMPILER=" + tools.Compiler, "-DCMAKE_MAKE_PROGRAM=" + tools.Ninja];
        if (generated != null) args.Add("-DSRF_GENERATED_DATA=" + generated);
        return RunAsync(tools.CMake, args, source, log, cancel, tools);
    }

    private string SourceDigest()
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var files = new[] { Path.Combine(source, "CMakeLists.txt"), Path.Combine(source, ".tools", "toolchain.json"), Path.Combine(source, ".tools", "dependencies.json") }
            .Concat(new[] { "src", "include" }.SelectMany(directory => Directory.EnumerateFiles(Path.Combine(source, directory), "*", SearchOption.AllDirectories)))
            .OrderBy(file => file, StringComparer.Ordinal);
        foreach (var file in files)
        {
            hash.AppendData(Encoding.UTF8.GetBytes(Path.GetRelativePath(source, file).Replace('\\', '/')));
            hash.AppendData([0]);
            hash.AppendData(File.ReadAllBytes(file));
            hash.AppendData([0]);
        }
        return Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant();
    }

    private static void ValidateInputs(string rom, string output)
    {
        if (string.IsNullOrWhiteSpace(rom) || !File.Exists(rom)) throw new FileNotFoundException("Choose an existing Stunt Race FX ROM.");
        if (string.IsNullOrWhiteSpace(output)) throw new ArgumentException("Choose a build output folder.");
        if (File.Exists(output)) throw new ArgumentException("The output must be a folder, not a file.");
    }

    public static BuildRecord? LastSuccess(string output)
    {
        var pointer = Path.Combine(output, "last-success.json");
        if (!File.Exists(pointer)) return null;
        var record = JsonSerializer.Deserialize<BuildRecord>(File.ReadAllText(pointer), JsonOptions);
        if (record == null || record.Directory != Path.GetFileName(record.Directory))
            throw new InvalidDataException("Invalid last-success.json build directory.");
        var stage = Path.GetFullPath(Path.Combine(output, "builds", record.Directory));
        if (!IsWithin(stage, Path.Combine(output, "builds"))) throw new InvalidDataException("Invalid build directory.");
        if (!File.Exists(Path.Combine(stage, "native", "srf_preview.exe")) ||
            !File.Exists(Path.Combine(stage, "native", "SDL3.dll")) ||
            !File.Exists(Path.Combine(stage, "generated", "world.srfworld")) ||
            !File.Exists(Path.Combine(stage, "generated", "sprites.srfsprites"))) return null;
        return record;
    }

    internal static string[] ContentArguments(BuildRecord record, string car, bool borderless, string course = GameContent.DefaultCourse)
    {
        if (car is not ("4wd" or "coupe" or "f-type")) throw new ArgumentException("Unsupported car.");
        var selected = GameContent.Course(course);
        if (record.ContentVersion < selected.MinimumContentVersion)
            throw new InvalidOperationException($"Rebuild from Game files to enable {selected.Name}.");
        if (record.ContentVersion < 1)
        {
            if (car != "4wd" || borderless)
                throw new InvalidOperationException("Rebuild from Game files to enable car selection and borderless mode. Your previous 4WD build is still available.");
            return [];
        }
        List<string> arguments = ["--car", car];
        if (borderless) arguments.Add("--borderless");
        if (record.ContentVersion >= 2) arguments.AddRange(["--course", course]);
        return [.. arguments];
    }

    internal static string EntryPointArgument(BuildRecord record, bool game)
    {
        if (game && record.ContentVersion < 2)
            throw new InvalidOperationException("Choose Quick Race or rebuild from Game files to enable Normal mode.");
        return game ? "--game" : "--race";
    }

    internal static string[] FrameArguments(int version, bool vr, int desktopFps, int vrRefresh, int vrScale)
    {
        if ((vr && vrRefresh == 0 && vrScale == 100) || (!vr && desktopFps == 0)) return [];
        if (version < 29) throw new InvalidOperationException("Rebuild from Game files to use custom frame settings.");
        return vr ? ["--vr-hz", vrRefresh.ToString(), "--vr-scale", vrScale.ToString()] : ["--desktop-fps", desktopFps.ToString()];
    }

    public static Process Launch(string output, bool inspectModels = false, int width = 1280, int height = 720,
        string car = "4wd", bool borderless = false, string course = GameContent.DefaultCourse, bool game = false, bool vr = false,
        int desktopFps = 0, int vrRefresh = 0, int vrScale = 100, RaceStyle style = RaceStyle.Original, bool n64LowRaster = false,
        int remasteredHour = 12, int remasteredMsaa = 0, string remasteredWeather = "clear",
        string remasteredCamera = "chase", bool cameraLook = false, int maxCameraDistance = GameContent.DefaultCameraDistance)
    {
        var record = LastSuccess(output) ?? throw new InvalidOperationException("Build the game from Game files first.");
        var stage = Path.Combine(Path.GetFullPath(output), "builds", record.Directory);
        var info = new ProcessStartInfo(Path.Combine(stage, "native", "srf_preview.exe"))
        { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = Path.Combine(stage, "native") };
        if (inspectModels)
        {
            info.ArgumentList.Add("--assets");
            info.ArgumentList.Add(Path.Combine(stage, "generated", "models.srfmesh"));
        }
        else
        {
            if (vr)
            {
                var selected = GameContent.Course(course);
                if (game || selected.VrVersion is not int minimum || record.ContentVersion < minimum)
                    throw new InvalidOperationException($"This build does not support {selected.Name} in VR.");
                info.ArgumentList.Add("--vr");
            }
            foreach (var arg in FrameArguments(record.ContentVersion, vr, desktopFps, vrRefresh, vrScale)) info.ArgumentList.Add(arg);
            if (style != RaceStyle.Original && (game || vr || !GameContent.SupportsEnhancedStyles(course, car)))
                throw new InvalidOperationException("Choose a desktop 4WD Quick Race for an enhanced visual style.");
            if (style != RaceStyle.Original && record.ContentVersion < GameContent.StyleVersion(course, style))
                throw new InvalidOperationException($"Rebuild from Game files to enable this visual style on {GameContent.Course(course).Name}.");
            if (style == RaceStyle.N64)
            {
                info.ArgumentList.Add(record.ContentVersion >= GameContent.N64CommandVersion ? "--n64" : "--remastered");
                if (n64LowRaster)
                {
                    if (record.ContentVersion < GameContent.N64RasterVersion)
                        throw new InvalidOperationException("Rebuild from Game files to enable the 320 × 240 N64 raster.");
                    info.ArgumentList.Add("--n64-240p");
                }
            }
            if (style == RaceStyle.Remastered)
            {
                info.ArgumentList.Add("--remastered");
                if (remasteredCamera == "isometric" && record.ContentVersion < GameContent.IsometricVersion)
                    throw new InvalidOperationException("Rebuild from Game files to use the Isometric camera.");
                if (record.ContentVersion >= GameContent.IsometricVersion)
                {
                    if (maxCameraDistance is < GameContent.MinCameraDistance or > GameContent.MaxCameraDistance)
                        throw new ArgumentOutOfRangeException(nameof(maxCameraDistance));
                    info.ArgumentList.Add("--camera-max-distance");
                    info.ArgumentList.Add(maxCameraDistance.ToString(System.Globalization.CultureInfo.InvariantCulture));
                }
                if (record.ContentVersion >= GameContent.CameraVersion)
                {
                    if (!GameContent.CameraIds.Contains(remasteredCamera)) throw new ArgumentOutOfRangeException(nameof(remasteredCamera));
                    info.ArgumentList.Add("--camera");
                    info.ArgumentList.Add(remasteredCamera);
                    if (cameraLook) info.ArgumentList.Add("--camera-look");
                }
                if (record.ContentVersion >= GameContent.AtmosphereVersion)
                {
                    if (remasteredMsaa is not (0 or 2 or 4 or 8)) throw new ArgumentOutOfRangeException(nameof(remasteredMsaa));
                    if (!GameContent.WeatherIds.Contains(remasteredWeather)) throw new ArgumentOutOfRangeException(nameof(remasteredWeather));
                    info.ArgumentList.Add("--msaa");
                    info.ArgumentList.Add(remasteredMsaa.ToString(System.Globalization.CultureInfo.InvariantCulture));
                    info.ArgumentList.Add("--weather");
                    info.ArgumentList.Add(remasteredWeather);
                }
                if (record.ContentVersion >= GameContent.SunLightingVersion)
                {
                    if (remasteredHour is < 0 or > 23) throw new ArgumentOutOfRangeException(nameof(remasteredHour));
                    info.ArgumentList.Add("--time-of-day");
                    info.ArgumentList.Add(remasteredHour.ToString(System.Globalization.CultureInfo.InvariantCulture));
                }
            }
            if (game) course = GameContent.DefaultCourse;
            if (game || vr) car = "4wd";
            var contentArguments = ContentArguments(record, car, borderless, course);
            var entryPoint = EntryPointArgument(record, game);
            var world = Path.Combine(GameContent.AssetDirectory(stage, course), "world.srfworld");
            if (!File.Exists(world)) throw new FileNotFoundException("The selected course is missing from this build. Rebuild from Game files.", world);
            if (string.IsNullOrWhiteSpace(record.RomPath) || !File.Exists(record.RomPath))
                throw new FileNotFoundException("The ROM used to build this game is missing. Restore it to its original location or rebuild with the new path.");
            var size = new FileInfo(record.RomPath).Length;
            if (size != 1048576 && size != 1049088) throw new InvalidDataException("The build's ROM has changed size. Verify and rebuild it.");
            var romBytes = File.ReadAllBytes(record.RomPath);
            var normalized = romBytes.AsSpan(romBytes.Length == 1049088 ? 512 : 0);
            using var identity = JsonDocument.Parse(record.RomManifest);
            var expected = identity.RootElement.GetProperty("sha256").GetString();
            if (!string.Equals(Convert.ToHexString(SHA256.HashData(normalized)), expected, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("The build's ROM checksum has changed. Verify and rebuild it.");
            foreach (var argument in new[] { "--world", world,
                entryPoint, "--rom", record.RomPath, "--width", width.ToString(), "--height", height.ToString(),
                "--controller-config", ControllerProfile.FilePath(output) })
                info.ArgumentList.Add(argument);
            foreach (var argument in contentArguments) info.ArgumentList.Add(argument);
            if (game && record.ContentVersion >= 13)
            {
                info.ArgumentList.Add("--progress-file");
                info.ArgumentList.Add(Path.Combine(Path.GetFullPath(output), "speed-trax.sav"));
            }
        }
        return Process.Start(info) ?? throw new InvalidOperationException("Could not start the native game.");
    }

    internal static bool IsWithin(string path, string root) => Path.GetFullPath(path).StartsWith(
        Path.TrimEndingDirectorySeparator(Path.GetFullPath(root)) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);

    internal static void AtomicWrite(string destination, string content)
    {
        var temporary = destination + "." + Guid.NewGuid().ToString("N") + ".tmp";
        File.WriteAllText(temporary, content, new UTF8Encoding(false));
        File.Move(temporary, destination, true);
    }

    internal static async Task<string> RunAsync(string file, IEnumerable<string> args, string workingDirectory,
        Action<string> log, CancellationToken cancel, Toolchain? tools = null)
    {
        cancel.ThrowIfCancellationRequested();
        var info = new ProcessStartInfo(file)
        { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true, WorkingDirectory = workingDirectory };
        foreach (var argument in args) info.ArgumentList.Add(argument);
        if (tools != null)
            info.Environment["PATH"] = Path.GetDirectoryName(tools.Compiler) + ";" + Path.GetDirectoryName(tools.Ninja) + ";" + Environment.GetEnvironmentVariable("PATH");
        log("> " + Path.GetFileName(file) + " " + string.Join(" ", info.ArgumentList.Select(argument => JsonSerializer.Serialize(argument))));
        using var process = new Process { StartInfo = info };
        process.Start();
        using var registration = cancel.Register(() =>
        {
            try { if (!process.HasExited) process.Kill(entireProcessTree: true); }
            catch (InvalidOperationException) { }
            catch (System.ComponentModel.Win32Exception) { }
        });
        var output = new StringBuilder();
        async Task ReadAsync(StreamReader reader, bool stdout)
        {
            while (await reader.ReadLineAsync() is { } line)
            {
                if (stdout) output.AppendLine(line);
                log(line);
            }
        }
        await Task.WhenAll(ReadAsync(process.StandardOutput, true), ReadAsync(process.StandardError, false), process.WaitForExitAsync());
        cancel.ThrowIfCancellationRequested();
        if (process.ExitCode != 0) throw new InvalidOperationException($"{Path.GetFileName(file)} failed (exit {process.ExitCode}). See the build log.");
        return output.ToString();
    }
}
