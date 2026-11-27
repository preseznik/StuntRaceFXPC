namespace StuntRaceFX.Launcher;

internal enum RaceStyle { Original, N64, Remastered }

internal sealed record GameCourse(string Id, string Name, int ReplayPhases, int MinimumContentVersion, int VrVersion = GameContent.AllCoursesVrVersion)
{
    public override string ToString() => Name;
}

internal static class GameContent
{
    internal const int CustomContentVersion = 120;
    internal const string CustomCar = "kestrel"; // Stable save and network ID.
    internal const string CustomCarName = "HILLY";
    internal const string CargoCar = "cargo";
    internal static string DisplayCar(string id) => id == CustomCar ? CustomCarName : id == CargoCar ? "CARGO" : id;
    internal static readonly GameCourse CustomCourse = new("ember-mesa", "Ember Mesa", 1655, CustomContentVersion, CustomContentVersion);
    internal static readonly GameCourse CityCourse = new("kasumi-loop", "Kasumi Inner Loop", 2243, 123, 123);
    internal static readonly GameCourse[] CustomCourses = [CustomCourse, CityCourse];
    internal static readonly string[] CustomCars = [CustomCar, CargoCar];
    internal static bool IsCustomCourse(string id) => CustomCourses.Any(c => c.Id == id);
    internal static bool IsCustom(string course, string car) => IsCustomCourse(course) || CustomCars.Contains(car);
    internal static int RequiredCustomVersion(string course, string car) => course == CityCourse.Id ? CityCourse.MinimumContentVersion : car == CargoCar ? 121 : CustomContentVersion;
    internal static bool AllowsCourse(string id, bool custom) => Courses.Any(c => c.Id == id) || custom && IsCustomCourse(id);
    internal const string DefaultCourse = "easy-ride";
    internal static bool SupportsEnhancedStyles(string course, string car) => (car is "4wd" or "coupe" or "f-type" or CustomCar or CargoCar) && AllowsCourse(course, true);
    internal static int StyleVersion(string course, string car, RaceStyle style) => IsCustom(course, car) ? RequiredCustomVersion(course, car) : car == "f-type" ? FTypeStyleVersion
        : car == "coupe" ? CoupeStyleVersion
        : course == "harbor-city" ? HarborCityStyleVersion
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
    internal static string StylePreview(string course, string car, RaceStyle style, int version)
    {
        var look = style == RaceStyle.N64 && version >= N64CommandVersion ? "n64" : "remastered";
        if (car != "4wd") return $"preview-{course}-{car}-{look}.png";
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
    internal const int CoupeStyleVersion = 55;
    internal const int FTypeStyleVersion = 56;
    internal const int ChampionshipStyleVersion = 57;
    internal const int StuntStyleVersion = 58;
    internal const int SeamlessVersion = 60;
    internal const int AllCarsVrVersion = 64;
    internal const int NightOwlVrVersion = 65;
    internal const int ClassicRasterVersion = 66;
    internal const int RaceHudVersion = 67;
    internal const int RayShadowsVersion = 70;
    internal const int RayReflectionsVersion = 71;
    internal const int IndirectLightVersion = 72;
    internal const int MaterialFinishVersion = 73;
    internal const int RoadDetailVersion = 74;
    internal const int FullRoadDetailVersion = 75;
    internal const int AquaRoadDetailVersion = 76;
    internal const int SunsetRoadDetailVersion = 77;
    internal const int NightOwlRoadDetailVersion = 78;
    internal const int ForestRoadDetailVersion = 79;
    internal const int SeaBreezeRoadDetailVersion = 80;
    internal const int WhiteLandRoadDetailVersion = 81;
    internal const int NightCruiseRoadDetailVersion = 82;
    internal const int LakeSideRoadDetailVersion = 83;
    internal const int BigRavineRoadDetailVersion = 84;
    internal const int SkyRampRoadDetailVersion = 85;
    internal const int HarborCityRoadDetailVersion = 86;
    internal const int ForestSceneryVersion = 87;
    internal const int ForestCourseSceneryVersion = 88;
    internal const int EasyRideSceneryVersion = 89;
    internal const int SunsetSceneryVersion = 90;
    internal const int AquaSceneryVersion = 91;
    internal const int OwlSceneryVersion = 92;
    internal const int SeaBreezeSceneryVersion = 93;
    internal const int WhiteLandSceneryVersion = 94;
    internal const int NightCruiseSceneryVersion = 95;
    internal const int LakeSideSceneryVersion = 96;
    internal const int BigRavineSceneryVersion = 97;
    internal const int SkyRampSceneryVersion = 98;
    internal const int HarborCitySceneryVersion = 99;
    internal const int WetTireSprayVersion = 100;
    internal const int AllTrackSprayVersion = 101;
    internal const int VolumetricLightingVersion = 102;
    internal const int VrStyleVersion = 103;
    internal const int AllCoursesVrVersion = 104;
    internal const int LanVersion = 107;
    internal const int AquaLanVersion = 108;
    internal const int SunsetLanVersion = 109;
    internal const int NightOwlLanVersion = 110;
    internal const int ForestLanVersion = 111;
    internal const int SeaBreezeLanVersion = 112;
    internal const int WhiteLandLanVersion = 113;
    internal const int NightCruiseLanVersion = 114;
    internal const int LakeSideLanVersion = 115;
    internal const int BigRavineLanVersion = 116;
    internal const int AllSpeedTraxLanVersion = 117;
    internal const int FourDriverLanVersion = 119;
    internal const int EnhancedPhysicsVersion = 124;
    internal const int RacerPhysicsVersion = 129;
    internal const int CurrentVersion = 131;
    internal static string DrivingPhysicsHelp =>
        "Applies to practice, Quick Race and LAN races you host. Hold Shift or the right bumper and steer at speed. " +
        "Blue sparks mean you can release for a boost (0.7 seconds). Gold sparks mean full boost (2 seconds). " +
        $"Enhanced LAN races require game content {RacerPhysicsVersion} or later on every PC.";
    internal static readonly IReadOnlyDictionary<string, int> LanCourseVersions = new Dictionary<string, int>
    {
        [DefaultCourse] = LanVersion,
        ["aqua-tunnel"] = AquaLanVersion,
        ["sunset-valley"] = SunsetLanVersion,
        ["night-owl"] = NightOwlLanVersion,
        ["kings-forest"] = ForestLanVersion,
        ["sea-breeze"] = SeaBreezeLanVersion,
        ["white-land"] = WhiteLandLanVersion,
        ["night-cruise"] = NightCruiseLanVersion,
        ["lake-side"] = LakeSideLanVersion,
        ["big-ravine"] = BigRavineLanVersion,
        ["sky-ramp"] = AllSpeedTraxLanVersion,
        [CustomCourse.Id] = CustomContentVersion,
        [CityCourse.Id] = CityCourse.MinimumContentVersion,
        ["harbor-city"] = AllSpeedTraxLanVersion
    };
    internal static bool SupportsVr(GameCourse course, string car, int version) =>
        version >= course.VrVersion &&
        (CustomCars.Contains(car) && version >= RequiredCustomVersion(course.Id, car) || car == "4wd" || (car is "coupe" or "f-type") && version >= AllCarsVrVersion);
    internal const int DefaultCameraDistance = 80, MinCameraDistance = 40, MaxCameraDistance = 160;
    internal static readonly string[] CameraIds = ["chase", "far-chase", "cockpit", "bonnet", "isometric"];
    internal static readonly string[] WeatherIds = ["clear", "partly-cloudy", "overcast", "fog", "rain"];
    internal static readonly GameCourse[] Courses = [
        new("easy-ride", "Easy Ride", 1637, 0, 28),
        new("aqua-tunnel", "Aqua Tunnel", 2206, 2, 30),
        new("sunset-valley", "Sunset Valley", 2117, 3, AllCarsVrVersion),
        new("night-owl", "Night Owl", 2722, 4, NightOwlVrVersion),
        new("kings-forest", "King's Forest", 2245, 5),
        new("sea-breeze", "Sea Breeze", 1958, 6),
        new("white-land", "White Land", 2560, 7),
        new("night-cruise", "Night Cruise", 2364, 8),
        new("lake-side", "Lake Side", 2260, 9),
        new("big-ravine", "Big Ravine", 3034, 10),
        new("sky-ramp", "Sky Ramp", 2811, 11),
        new("harbor-city", "Harbor City", 2613, 12)
    ];

    internal static GameCourse Course(string id) => CustomCourses.Concat(Courses).FirstOrDefault(course => course.Id == id)
        ?? throw new ArgumentException("Unsupported course.");

    internal static string AssetDirectory(string stage, string course)
    {
        Course(course);
        return course == DefaultCourse || IsCustomCourse(course) ? Path.Combine(stage, "generated") : Path.Combine(stage, "generated", course);
    }

    internal static string CaptureName(string course, string car, RaceStyle style, int version) =>
        style == RaceStyle.Original ? version >= 2 ? $"preview-{course}-{car}.png" : $"preview-{car}.png"
        : GameContent.StylePreview(course, car, style, version);

}
