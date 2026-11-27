using System.Windows.Media.Imaging;

namespace StuntRaceFX.Launcher.Modern;

internal sealed record InstalledContent(BuildRecord? Record, string? Stage, HashSet<string> Worlds)
{
    internal int Version => Record?.ContentVersion ?? -1;

    internal static InstalledContent Read(string output)
    {
        var record = BuildPipeline.LastSuccess(output);
        string? stage = record == null ? null : Path.Combine(Path.GetFullPath(output), "builds", record.Directory);
        var worlds = GameContent.Courses.Concat(GameContent.CustomCourses).Where(course => stage != null &&
            File.Exists(Path.Combine(GameContent.AssetDirectory(stage, course.Id), "world.srfworld"))).Select(c => c.Id).ToHashSet();
        return new(record, stage, worlds);
    }

    internal string? BlockReason(LauncherSettings s)
    {
        if (Stage == null) return "Choose your ROM and build the game in Game files.";
        bool vr = RaceLaunch.Vr(s);
        if (s.QuickRace && s.EnhancedPhysics && Version < GameContent.EnhancedPhysicsVersion)
            return "Rebuild the game or turn off Enhanced physics in Settings → Gameplay.";
        var style = RaceLaunch.Style(s);
        var course = GameContent.Course(s.QuickRace ? RaceLaunch.Course(s) : GameContent.DefaultCourse);
        var car = RaceLaunch.Car(s);
        if ((s.QuickRace || vr) && !Worlds.Contains(course.Id)) return $"{course.Name} is missing from this build.";
        bool supported = s.QuickRace ? Version >= course.MinimumContentVersion : Version >= (vr ? GameContent.SeamlessVersion : 2);
        if (vr && s.QuickRace) supported &= GameContent.SupportsVr(course, car, Version);
        if (s.QuickRace && GameContent.IsCustom(course.Id, car)) supported &= s.CustomContent && Version >= GameContent.RequiredCustomVersion(course.Id, car);
        if (s.QuickRace && Version == 0) supported &= car == "4wd" && !RaceLaunch.Display(s, RaceLaunch.Profile(s)).Borderless;
        if (style != RaceStyle.Original) supported &= Version >= (s.QuickRace ? GameContent.StyleVersion(course.Id, car, style) : GameContent.ChampionshipStyleVersion)
            && (!vr || Version >= GameContent.VrStyleVersion);
        if (style == RaceStyle.Remastered && s.RemasteredCamera == "isometric") supported &= Version >= GameContent.IsometricVersion;
        return supported ? null : "Rebuild the game in Game files to use this setup.";
    }
}

internal sealed class CaptureLibrary
{
    private readonly Dictionary<(string, int), Task<BitmapImage?>> cache = [];
    private readonly SemaphoreSlim decoders = new(3);
    internal Task<BitmapImage?> Load(string? stage, string file, int width)
    {
        if (stage == null) return Task.FromResult<BitmapImage?>(null);
        var key = (Path.Combine(stage, file), width);
        if (cache.TryGetValue(key, out var pending)) return pending;
        if (cache.Count >= 48) cache.Remove(cache.Keys.First());
        return cache[key] = Decode(key.Item1, width);
    }
    private async Task<BitmapImage?> Decode(string path, int width)
    {
        await decoders.WaitAsync().ConfigureAwait(false);
        try
        {
            return await Task.Run(() =>
            {
                using var stream = File.OpenRead(path);
                var image = new BitmapImage();
                image.BeginInit(); image.CacheOption = BitmapCacheOption.OnLoad;
                image.DecodePixelWidth = width; image.StreamSource = stream; image.EndInit(); image.Freeze();
                return image;
            }).ConfigureAwait(false);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or NotSupportedException or FormatException) { return null; }
        finally { decoders.Release(); }
    }
    internal void Clear() => cache.Clear();
}

