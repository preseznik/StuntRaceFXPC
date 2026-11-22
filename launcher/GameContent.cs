namespace StuntRaceFX.Launcher;

internal sealed record GameCourse(string Id, string Name, int ReplayPhases, int MinimumContentVersion)
{
    public override string ToString() => Name;
}

internal static class GameContent
{
    internal const string DefaultCourse = "easy-ride";
    internal const int OriginalBootVersion = 22;
    internal const int CurrentVersion = 29;
    internal static readonly GameCourse[] Courses = [
        new("easy-ride", "Easy Ride", 1637, 0),
        new("aqua-tunnel", "Aqua Tunnel", 2206, 2),
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
