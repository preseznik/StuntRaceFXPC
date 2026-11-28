namespace StuntRaceFX.Launcher;

internal sealed record GameRebuild(string? Rom)
{
    // Portable updates start a new process with a new source directory.
    private static readonly Lazy<string?> packageDigest = new(() =>
    {
        try { return BuildPipeline.SourceDigest(BuildPipeline.FindSource()); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    });

    internal string ActionText => Rom == null ? "Choose ROM to rebuild…" : "Rebuild game";
    internal string Message => Rom == null ? "The game has an update. Choose your ROM to rebuild it."
        : $"The game has an update. Rebuild with your saved ROM ({Path.GetFileName(Rom)}).";

    internal static GameRebuild? Detect(BuildRecord? record, string selectedRom)
    {
        if (record == null || record.ContentVersion > GameContent.CurrentVersion || record.RecipeVersion > BuildPipeline.CurrentRecipeVersion) return null;
        if (record.RecipeVersion == BuildPipeline.CurrentRecipeVersion && !SourcesChanged(record)) return null;

        var path = string.IsNullOrWhiteSpace(selectedRom) ? record.RomPath : selectedRom;
        if (!string.IsNullOrWhiteSpace(path) && !Path.IsPathFullyQualified(path))
        {
            var portable = Path.Combine(LauncherUpdate.Home, path);
            if (File.Exists(portable)) path = portable;
        }
        return new(File.Exists(path) ? Path.GetFullPath(path) : null);
    }

    private static bool SourcesChanged(BuildRecord record)
    {
        if (record.SourceSha256 is { Length: 64 } hash && hash.All(Uri.IsHexDigit))
            return packageDigest.Value is string current && !current.Equals(hash, StringComparison.OrdinalIgnoreCase);
        // Records without a fingerprint predate the native SteamVR changes shipped in Build 131.
        return record.ContentVersion < 131;
    }
}
