namespace StuntRaceFX.Launcher;

internal sealed record DisplaySettings(string Resolution = "1920 x 1080", bool Borderless = false, int DesktopFps = 0);

// Outer window size in device-independent pixels, shared by both interfaces.
internal sealed record LauncherWindowSize(double Width, double Height, bool Maximized)
{
    internal bool IsValid => double.IsFinite(Width) && double.IsFinite(Height) && Width > 0 && Height > 0;
}

internal sealed record LauncherSettings(string RomPath = "", string OutputPath = "",
    string Car = "4wd", string Resolution = "1920 x 1080", bool Borderless = false,
    string Course = GameContent.DefaultCourse, bool QuickRace = false, bool EasyRideVr = false,
    int DesktopFps = 0, int VrRefresh = 0, int VrScale = 100, string VrCourse = GameContent.DefaultCourse, bool N64Mode = false,
    bool N64LowRaster = true, bool RemasteredMode = false, Dictionary<string, DisplaySettings>? GraphicsProfiles = null, int RemasteredHour = 12,
    int RemasteredMsaa = 0, string RemasteredWeather = "clear", string RemasteredCamera = "chase", bool CameraLook = false,
    int MaxCameraDistance = GameContent.DefaultCameraDistance, string? QuickRaceStyle = null, string? NormalStyle = null, bool NormalVr = false, string VrCar = "4wd", bool ClassicSnes = false, bool HideRaceHud = false, bool RayTracedShadows = false, bool RayTracedReflections = false, bool RayTracedIndirect = false, bool MaterialRoughness = false, bool CarClearCoat = false, bool HighDetailRoad = false, bool HighDetailScenery = false, int ShadowRays = 4, bool WetTireSpray = false, bool VolumetricLighting = false, string? VrStyle = null, bool SmoothVrMotion = true, bool CustomContent = false, string? LauncherTheme = null, bool ClassicLauncher = false, string? WpfTheme = null,
    string DesktopQualityPreset = "Custom", RemasteredQuality? VrQuality = null, bool EnhancedPhysics = false,
    LauncherWindowSize? WindowSize = null, string VrRuntime = "");

internal sealed record RemasteredQuality(
    int RemasteredMsaa = 0, bool RayTracedShadows = false, bool RayTracedReflections = false,
    bool RayTracedIndirect = false, bool MaterialRoughness = false, bool CarClearCoat = false,
    bool HighDetailRoad = false, bool HighDetailScenery = false, int ShadowRays = 4,
    bool WetTireSpray = false, bool VolumetricLighting = false, string Preset = "Custom")
{
    internal static readonly RemasteredQuality Performance = new(MaterialRoughness: true, CarClearCoat: true,
        HighDetailRoad: true, ShadowRays: 1, Preset: "Performance");
    internal static readonly RemasteredQuality Balanced = Performance with
    {
        RemasteredMsaa = 2, HighDetailScenery = true, WetTireSpray = true, VolumetricLighting = true, Preset = "Balanced"
    };
    internal static readonly RemasteredQuality Quality = Balanced with
    {
        RemasteredMsaa = 4, RayTracedShadows = true, ShadowRays = 2, RayTracedReflections = true,
        RayTracedIndirect = true, Preset = "Quality"
    };

    internal static RemasteredQuality Read(LauncherSettings s, bool vr) => vr && s.VrQuality != null ? s.VrQuality :
        new(s.RemasteredMsaa, s.RayTracedShadows, s.RayTracedReflections, s.RayTracedIndirect,
            s.MaterialRoughness, s.CarClearCoat, s.HighDetailRoad, s.HighDetailScenery, s.ShadowRays,
            s.WetTireSpray, s.VolumetricLighting, s.DesktopQualityPreset);

    internal LauncherSettings Store(LauncherSettings s, bool vr) => vr ? s with { VrQuality = this } : s with
    {
        VrQuality = Read(s, true), DesktopQualityPreset = Preset,
        RemasteredMsaa = RemasteredMsaa, RayTracedShadows = RayTracedShadows,
        RayTracedReflections = RayTracedReflections, RayTracedIndirect = RayTracedIndirect,
        MaterialRoughness = MaterialRoughness, CarClearCoat = CarClearCoat,
        HighDetailRoad = HighDetailRoad, HighDetailScenery = HighDetailScenery, ShadowRays = ShadowRays,
        WetTireSpray = WetTireSpray, VolumetricLighting = VolumetricLighting
    };
}
