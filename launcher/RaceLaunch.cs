using System.Diagnostics;
using System.Text.Json;

namespace StuntRaceFX.Launcher;

internal static class RaceLaunch
{
    internal static bool Vr(LauncherSettings s) => s.NormalVr || s.EasyRideVr;
    internal static RaceStyle Style(LauncherSettings s) => (Vr(s) ? s.VrStyle : s.QuickRace ? s.QuickRaceStyle : s.NormalStyle) switch
    {
        "N64" => RaceStyle.N64,
        "Remastered" => RaceStyle.Remastered,
        _ => RaceStyle.Original
    };
    internal static string StyleName(RaceStyle style) => style switch { RaceStyle.N64 => "N64 Mode", RaceStyle.Remastered => "Remastered", _ => "Classic" };
    internal static string Profile(LauncherSettings s) => Vr(s) ? "VR" : !s.QuickRace && Style(s) == RaceStyle.Original ? "Normal" : StyleName(Style(s));
    internal static DisplaySettings Display(LauncherSettings s, string profile) => s.GraphicsProfiles?.GetValueOrDefault(profile) ??
        (s.GraphicsProfiles == null ? new(s.Resolution, s.Borderless, s.DesktopFps) : new());
    internal static string Course(LauncherSettings s) => Vr(s) ? s.VrCourse : s.Course;
    internal static string Car(LauncherSettings s) => Vr(s) ? s.VrCar : s.Car;
    internal static Process Start(LauncherSettings s, LanGrid? lan = null, RaceStyle? lanStyle = null) =>
        BuildPipeline.Start(Prepare(s, lan, lanStyle));

    internal static Process StartPractice(LauncherSettings s) => BuildPipeline.Start(Prepare(s, practice: true));

    internal static ProcessStartInfo Prepare(LauncherSettings s, LanGrid? lan = null, RaceStyle? lanStyle = null, bool practice = false)
    {
        if (practice)
            s = s with { QuickRace = true, NormalVr = false, EasyRideVr = Vr(s), Car = "4wd", VrCar = "4wd",
                Course = GameContent.DefaultCourse, VrCourse = GameContent.DefaultCourse, QuickRaceStyle = "Original", VrStyle = "Original" };
        var vr = lan == null && Vr(s);
        var quality = RemasteredQuality.Read(s, vr);
        var style = lanStyle ?? Style(s);
        var display = Display(s, lanStyle.HasValue ? StyleName(style) : Profile(s));
        var dimensions = display.Resolution.Split('x', StringSplitOptions.TrimEntries);
        var version = BuildPipeline.LastSuccess(s.OutputPath)?.ContentVersion ?? 0;
        var launch = BuildPipeline.PrepareLaunch(s.OutputPath, width: int.Parse(dimensions[0]), height: int.Parse(dimensions[1]),
            car: Car(s), borderless: display.Borderless, course: lan?.Course ?? (s.QuickRace ? Course(s) : GameContent.DefaultCourse),
            game: lan == null && !s.QuickRace, vr: vr, style: style, desktopFps: display.DesktopFps,
            vrRefresh: s.VrRefresh, vrScale: s.VrScale, smoothVrMotion: s.SmoothVrMotion, n64LowRaster: s.N64LowRaster && version >= GameContent.N64RasterVersion,
            remasteredHour: s.RemasteredHour, remasteredMsaa: quality.RemasteredMsaa, remasteredWeather: s.RemasteredWeather, remasteredCamera: s.RemasteredCamera,
            cameraLook: s.CameraLook, maxCameraDistance: s.MaxCameraDistance, classicSnes: s.ClassicSnes, hideRaceHud: s.HideRaceHud,
            rayTracedShadows: quality.RayTracedShadows, rayTracedReflections: quality.RayTracedReflections, rayTracedIndirect: quality.RayTracedIndirect,
            materialRoughness: quality.MaterialRoughness, carClearCoat: quality.CarClearCoat, highDetailRoad: quality.HighDetailRoad, highDetailScenery: quality.HighDetailScenery,
            shadowRays: quality.ShadowRays, wetTireSpray: quality.WetTireSpray, volumetricLighting: quality.VolumetricLighting, lan: lan, customContent: s.CustomContent,
            enhancedPhysics: s.EnhancedPhysics, practice: practice);
        if (vr && !string.IsNullOrEmpty(s.VrRuntime))
            launch.Environment["XR_RUNTIME_JSON"] = OpenXrRuntime.Resolve(s.VrRuntime);
        return launch;
    }

    internal static LauncherSettings Read(string path)
    {
        var s = new LauncherSettings();
        bool legacyN64 = false;
        if (File.Exists(path))
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(path));
            if (doc.RootElement.ValueKind != JsonValueKind.Object) throw new JsonException("Expected a launcher settings object.");
            s = doc.RootElement.Deserialize<LauncherSettings>(BuildPipeline.JsonOptions) ?? s;
            s = s with { VrRuntime = OpenXrRuntime.SavedChoice(doc.RootElement, s.VrRuntime) };
            legacyN64 = !doc.RootElement.TryGetProperty("N64Mode", out _) && doc.RootElement.TryGetProperty("Remastered", out var old) && old.ValueKind == JsonValueKind.True;
        }
        bool legacy = s.QuickRaceStyle == null && (s.N64Mode || legacyN64 || s.RemasteredMode);
        var profiles = new Dictionary<string, DisplaySettings>();
        foreach (var (name, old) in new[] { ("Normal", "Normal"), ("VR", "4WD VR"), ("Classic", "Original"), ("N64 Mode", "N64 mode"), ("Remastered", "Remastered") })
            profiles[name] = s.GraphicsProfiles?.GetValueOrDefault(name) ?? s.GraphicsProfiles?.GetValueOrDefault(old)
                ?? (name == "Classic" ? s.GraphicsProfiles?.GetValueOrDefault("Quick Race") : null) ?? Display(s, name);
        string ValidCar(string id) => LanLobby.Cars.Contains(id) || s.CustomContent && GameContent.CustomCars.Contains(id) ? id : "4wd";
        return s with
        {
            OutputPath = string.IsNullOrWhiteSpace(s.OutputPath) ? Path.Combine(LauncherUpdate.Home, "user-builds") : s.OutputPath,
            QuickRace = !s.NormalVr && (s.QuickRace || s.EasyRideVr || legacy),
            EasyRideVr = !legacy && s.EasyRideVr,
            QuickRaceStyle = legacy ? s.RemasteredMode ? "Remastered" : "N64" : s.QuickRaceStyle ?? "Original",
            Course = !legacy && GameContent.AllowsCourse(s.Course, s.CustomContent) ? s.Course : GameContent.DefaultCourse,
            Car = legacy ? "4wd" : ValidCar(s.Car),
            VrCar = ValidCar(s.VrCar),
            VrCourse = GameContent.AllowsCourse(s.VrCourse, s.CustomContent) ? s.VrCourse : GameContent.DefaultCourse,
            VrQuality = RemasteredQuality.Read(s, true),
            GraphicsProfiles = profiles
        };
    }
}

