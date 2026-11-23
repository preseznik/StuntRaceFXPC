namespace StuntRaceFX.Launcher;

internal enum RaceStyle { Original, N64, Remastered }

internal sealed record GameCourse(string Id, string Name, int ReplayPhases, int MinimumContentVersion, int? VrVersion = null)
{
    public override string ToString() => Name;
}

internal static class GameContent
{
    internal const string DefaultCourse = "easy-ride";
    internal static bool SupportsEnhancedStyles(string course, string car) => car == "4wd" && Courses.Any(entry => entry.Id == course);
    internal static int StyleVersion(string course, RaceStyle style) => course == "harbor-city" ? HarborCityStyleVersion
        : course == "sky-ramp" ? SkyRampStyleVersion
        : course == "big-ravine" ? BigRavineStyleVersion
        : course == "lake-side" ? LakeSideStyleVersion
        : course == "night-cruise" ? NightCruiseStyleVersion
        : course == "white-land" ? WhiteLandStyleVersion
        : course == "sea-breeze" ? SeaBreezeStyleVersion
        : course == "kings-forest" ? ForestStyleVersion
        : course == "night-owl" ? NightOwlStyleVersion
        : course == "sunset-valley" ? SunsetStyleVersion
        : course == "aqua-tunnel" ? AquaStyleVersion
        : style == RaceStyle.Remastered ? RemasteredVersion : N64ModeVersion;
    internal static string StylePreview(string course, RaceStyle style, int version)
    {
        var look = style == RaceStyle.N64 && version >= N64CommandVersion ? "n64" : "remastered";
        return course == DefaultCourse ? $"preview-{look}.png" : $"preview-{course}-{look}.png";
    }
    internal const int OriginalBootVersion = 22;
    internal const int N64ModeVersion = 31;
    internal const int N64CommandVersion = 32;
    internal const int N64RasterVersion = 33;
    internal const int RemasteredVersion = 34;
    internal const int SunLightingVersion = 35;
    internal const int AtmosphereVersion = 37;
    internal const int CameraVersion = 38;
    internal const int IsometricVersion = 40;
    internal const int AquaStyleVersion = 42;
    internal const int SunsetStyleVersion = 44;
    internal const int NightOwlStyleVersion = 45;
    internal const int ForestStyleVersion = 46;
    internal const int SeaBreezeStyleVersion = 48;
    internal const int WhiteLandStyleVersion = 49;
    internal const int NightCruiseStyleVersion = 50;
    internal const int LakeSideStyleVersion = 51;
    internal const int BigRavineStyleVersion = 52;
    internal const int SkyRampStyleVersion = 53;
    internal const int HarborCityStyleVersion = 54;
    internal const int CurrentVersion = HarborCityStyleVersion;
    internal const int DefaultCameraDistance = 80, MinCameraDistance = 40, MaxCameraDistance = 160;
    internal static readonly string[] CameraIds = ["chase", "far-chase", "cockpit", "bonnet", "isometric"];
    internal static readonly string[] WeatherIds = ["clear", "partly-cloudy", "overcast", "fog", "rain"];
    internal static readonly GameCourse[] Courses = [
        new("easy-ride", "Easy Ride", 1637, 0, 28),
        new("aqua-tunnel", "Aqua Tunnel", 2206, 2, 30),
        new("sunset-valley", "Sunset Valley", 2117, 3),
        new("night-owl", "Night Owl", 2722, 4),
        new("kings-forest", "King's Forest", 2245, 5),
        new("sea-breeze", "Sea Breeze", 1958, 6),
        new("white-land", "White Land", 2560, 7),
        new("night-cruise", "Night Cruise", 2364, 8),
        new("lake-side", "Lake Side", 2260, 9),
        new("big-ravine", "Big Ravine", 3034, 10),
        new("sky-ramp", "Sky Ramp", 2811, 11),
        new("harbor-city", "Harbor City", 2613, 12)
    ];

    internal static GameCourse Course(string id) => Courses.FirstOrDefault(course => course.Id == id)
        ?? throw new ArgumentException("Unsupported course.");

    internal static string AssetDirectory(string stage, string course)
    {
        Course(course);
        return course == DefaultCourse ? Path.Combine(stage, "generated") : Path.Combine(stage, "generated", course);
    }
}
