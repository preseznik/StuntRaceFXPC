using System.Globalization;

namespace StuntRaceFX.Launcher;

internal sealed record ControllerBinding(string Key, string Label, string Default, bool Axis = false);

internal sealed class ControllerProfile
{
    internal static readonly ControllerBinding[] Bindings =
    [
        new("steer_left", "Steer left", "dpleft"),
        new("steer_right", "Steer right", "dpright"),
        new("accelerate", "Accelerate", "south"),
        new("brake", "Brake / reverse", "east"),
        new("boost", "Boost", "west"),
        new("drift", "Drift (Enhanced)", "rightshoulder"),
        new("pause", "Pause / resume", "start"),
        new("restart", "Restart race", "back"),
        new("recenter", "Recenter VR seat", "rightstick"),
        new("steering_axis", "Steering axis", "leftx", true),
        new("accelerate_axis", "Accelerator axis", "righttrigger", true),
        new("brake_axis", "Brake axis", "lefttrigger", true),
    ];
    internal static readonly string[] Buttons = ["none", "south", "east", "west", "north", "back", "guide", "start",
        "leftstick", "rightstick", "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright",
        "misc1", "paddle1", "paddle2", "paddle3", "paddle4", "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6"];
    internal static readonly string[] Axes = ["none", "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"];
    private readonly Dictionary<string, string> values = Bindings.ToDictionary(binding => binding.Key, binding => binding.Default);

    internal ControllerProfile() { values.Add("steering_mode", "digital"); values.Add("deadzone", "8000"); values.Add("trigger_threshold", "4000"); }
    internal string this[string key] => values[key];
    internal static string FilePath(string output) => Path.Combine(Path.GetFullPath(output), "controller.ini");
    internal static ControllerProfile Load(string path) => File.Exists(path) ? Parse(File.ReadAllText(path)) : new();

    internal void Set(string key, string value)
    {
        value = value.Trim().ToLowerInvariant();
        var binding = Bindings.FirstOrDefault(item => item.Key == key);
        if (binding != null)
        {
            if (!binding.Axis) value = value switch { "a" => "south", "b" => "east", "x" => "west", "y" => "north", _ => value };
            if (!(binding.Axis ? Axes : Buttons).Contains(value))
                throw new InvalidDataException($"Unsupported {binding.Label.ToLowerInvariant()} binding: {value}");
        }
        else if (key == "steering_mode")
        {
            if (value is not ("digital" or "analog"))
                throw new InvalidDataException("steering_mode must be digital or analog.");
        }
        else if (key is "deadzone" or "trigger_threshold")
        {
            if (!int.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out var number) || number < 1 || number > 30000)
                throw new InvalidDataException($"{key} must be between 1 and 30000.");
            value = number.ToString(CultureInfo.InvariantCulture);
        }
        else throw new InvalidDataException($"Unknown controller setting: {key}");
        values[key] = value;
    }

    internal static ControllerProfile Parse(string content)
    {
        var profile = new ControllerProfile();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var source in content.Split('\n'))
        {
            var line = source.Trim();
            if (line.StartsWith("# drift=", StringComparison.Ordinal) || line.StartsWith("# handbrake=", StringComparison.Ordinal)) line = line[2..];
            if (line.Length == 0 || line.StartsWith('#') || line.StartsWith(';')) continue;
            var separator = line.IndexOf('=');
            if (separator <= 0) throw new InvalidDataException("Controller settings require key=value lines.");
            var key = line[..separator].Trim();
            if (key == "handbrake") key = "drift";
            if (!seen.Add(key)) throw new InvalidDataException($"Duplicate controller setting: {key}");
            profile.Set(key, line[(separator + 1)..]);
        }
        return profile;
    }

    internal string Serialize() => "# Stunt Race FX native controller profile\n" +
        string.Join('\n', Bindings.Select(binding => binding.Key).Concat(["steering_mode", "deadzone", "trigger_threshold"])
            // Version 127 reads this spelling. Earlier builds ignore the comment.
            .Select(key => (key == "drift" ? "# handbrake" : key) + "=" + values[key])) + "\n";

    internal void Save(string path)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        BuildPipeline.AtomicWrite(path, Serialize());
    }

    internal static string LabelFor(string value) => value switch
    {
        "none" => "Unassigned",
        "south" => "South face button — Xbox A / PlayStation Cross",
        "east" => "East face button — Xbox B / PlayStation Circle",
        "west" => "West face button — Xbox X / PlayStation Square",
        "north" => "North face button — Xbox Y / PlayStation Triangle",
        "back" => "Back / Select / View", "start" => "Start / Menu", "guide" => "Guide / Home",
        "leftstick" => "Left stick click", "rightstick" => "Right stick click",
        "leftshoulder" => "Left shoulder / LB / L1", "rightshoulder" => "Right shoulder / RB / R1",
        "dpleft" => "D-pad left", "dpright" => "D-pad right", "dpup" => "D-pad up", "dpdown" => "D-pad down",
        "leftx" => "Left stick horizontal", "lefty" => "Left stick vertical",
        "rightx" => "Right stick horizontal", "righty" => "Right stick vertical",
        "lefttrigger" => "Left trigger / LT / L2", "righttrigger" => "Right trigger / RT / R2",
        _ => value,
    };
}
