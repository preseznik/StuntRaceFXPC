using Microsoft.Win32;
using System.Security;
using System.Text.Json;

namespace StuntRaceFX.Launcher;

internal sealed record OpenXrRuntime(string Id, string Name, string? Manifest)
{
    public override string ToString() => Name;
    private const string RegistryPath = @"SOFTWARE\Khronos\OpenXR\1";

    internal static string SavedChoice(JsonElement settings, string? choice) =>
        !settings.TryGetProperty("VrRuntime", out _) && settings.TryGetProperty("UseSteamVr", out var old) && old.ValueKind == JsonValueKind.True
            ? "SteamVR" : choice ?? "";

    // Read a manifest as data. Only the game's OpenXR loader loads the runtime.
    internal static OpenXrRuntime ReadManifest(string path)
    {
        if (!Path.IsPathFullyQualified(path) || !Path.GetExtension(path).Equals(".json", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Choose an OpenXR runtime JSON file.");
        using var doc = JsonDocument.Parse(File.ReadAllText(path));
        var root = doc.RootElement;
        if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty("file_format_version", out var version) ||
            version.ValueKind != JsonValueKind.String || !Version.TryParse(version.GetString(), out var format) || format.Major != 1 ||
            !root.TryGetProperty("runtime", out var runtime) || runtime.ValueKind != JsonValueKind.Object ||
            !runtime.TryGetProperty("library_path", out var library) || library.ValueKind != JsonValueKind.String || string.IsNullOrWhiteSpace(library.GetString()))
            throw new InvalidDataException("This file does not describe an OpenXR runtime.");
        var dll = library.GetString()!;
        if (dll != Path.GetFileName(dll) && !File.Exists(Path.GetFullPath(dll, Path.GetDirectoryName(path)!)))
            throw new FileNotFoundException($"The OpenXR runtime library referenced by {Path.GetFileName(path)} is missing.");
        string name = runtime.TryGetProperty("name", out var label) && label.ValueKind == JsonValueKind.String && !string.IsNullOrWhiteSpace(label.GetString())
            ? label.GetString()! : Path.GetFileNameWithoutExtension(path);
        return Path.GetFileName(path).ToLowerInvariant() switch
        {
            "steamxr_win64.json" => new("SteamVR", "SteamVR", path),
            "virtualdesktop-openxr.json" => new("VDXR", "Virtual Desktop (VDXR)", path),
            _ => new(path, name, path)
        };
    }

    internal static bool DiscoveryError(Exception e) => e is IOException or UnauthorizedAccessException or SecurityException or JsonException or ArgumentException or InvalidDataException;

    internal static List<OpenXrRuntime> Choices(string selected)
    {
        var installed = new List<OpenXrRuntime>();
        OpenXrRuntime? Add(string? path)
        {
            if (string.IsNullOrEmpty(path) || !File.Exists(path)) return null;
            try
            {
                var item = ReadManifest(path);
                if (!installed.Any(r => string.Equals(r.Id, item.Id, StringComparison.OrdinalIgnoreCase))) installed.Add(item);
                return item;
            }
            catch (Exception e) when (DiscoveryError(e)) { return null; }
        }
        string? active = Environment.GetEnvironmentVariable("XR_RUNTIME_JSON");
        var hidden = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        try
        {
            using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
            using var xr = machine.OpenSubKey(RegistryPath);
            var system = xr?.GetValue("ActiveRuntime") as string;
            if (string.IsNullOrWhiteSpace(active)) active = system;
            Add(system);
            using var available = xr?.OpenSubKey("AvailableRuntimes");
            if (available != null)
            {
                foreach (var path in available.GetValueNames())
                {
                    if (available.GetValue(path) is 0) Add(path);
                    else hidden.Add(path);
                }
            }
        }
        catch (Exception e) when (DiscoveryError(e)) { }
        if (!installed.Any(r => r.Id == "SteamVR"))
        {
            var registration = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "openvr", "openvrpaths.vrpath");
            try
            {
                if (File.Exists(registration))
                {
                    using var doc = JsonDocument.Parse(File.ReadAllText(registration));
                    if (doc.RootElement.ValueKind == JsonValueKind.Object && doc.RootElement.TryGetProperty("runtime", out var dirs) && dirs.ValueKind == JsonValueKind.Array)
                        foreach (var dir in dirs.EnumerateArray())
                        {
                            if (dir.ValueKind != JsonValueKind.String || dir.GetString() is not string p || !Path.IsPathFullyQualified(p)) continue;
                            var path = Path.Combine(p, "steamxr_win64.json");
                            if (!hidden.Contains(path) && Add(path) != null) break;
                        }
                }
            }
            catch (Exception e) when (DiscoveryError(e)) { }
        }
        if (!installed.Any(r => r.Id == "VDXR"))
        {
            var path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Virtual Desktop Streamer", "OpenXR", "virtualdesktop-openxr.json");
            if (!hidden.Contains(path)) Add(path);
        }
        var effective = Add(active);
        installed.Sort((a,b) => StringComparer.OrdinalIgnoreCase.Compare(a.Name, b.Name));
        installed.Insert(0, new("", effective == null ? "Automatic (OpenXR default)" : $"Automatic — {effective.Name}", null));
        if (!installed.Any(r => string.Equals(r.Id, selected, StringComparison.OrdinalIgnoreCase)))
        {
            // Keep an explicit file pinned even if its runtime is also registered under a named preset.
            var custom = Add(selected);
            if (custom == null) installed.Add(new(selected, $"{selected} (unavailable)", null));
            else if (!installed.Any(r => string.Equals(r.Id, selected, StringComparison.OrdinalIgnoreCase)))
                installed.Add(new(selected, $"{custom.Name} ({Path.GetFileName(selected)})", selected));
        }
        return installed;
    }

    internal static string Resolve(string selected)
    {
        if (Path.IsPathFullyQualified(selected)) return ReadManifest(selected).Manifest!;
        return Choices(selected).First(r => r.Id.Equals(selected, StringComparison.OrdinalIgnoreCase)).Manifest
            ?? throw new FileNotFoundException($"The selected OpenXR runtime ({selected}) is unavailable. Check Settings → Graphics → VR.");
    }

    internal static string LastSession(string? stage)
    {
        if (stage != null)
        {
            try
            {
                using var file = new FileStream(Path.Combine(stage, "native", "openxr-last-drive.log"), FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                using var reader = new StreamReader(file);
                string? runtime = null;
                for (int i = 0; i < 8 && reader.ReadLine() is string line; ++i)
                {
                    if (line.StartsWith("Runtime ", StringComparison.Ordinal) && !line.StartsWith("Runtime controls ", StringComparison.Ordinal)) runtime = line[8..];
                    if (runtime != null && line.StartsWith("Headset ", StringComparison.Ordinal)) return $"Last VR run used {runtime} with {line[8..]}.";
                }
                if (runtime != null) return $"Last VR run used {runtime}.";
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
        return "The game reports the runtime and headset here after a VR launch.";
    }
}
