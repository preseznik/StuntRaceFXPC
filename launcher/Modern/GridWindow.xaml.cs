using System.ComponentModel;
using System.Diagnostics;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace StuntRaceFX.Launcher.Modern;

using Button = System.Windows.Controls.Button;
using ComboBox = System.Windows.Controls.ComboBox;
using KeyEventArgs = System.Windows.Input.KeyEventArgs;
using Visibility = System.Windows.Visibility;

internal partial class GridWindow : Window
{
    private enum Page { Play, Setup, Network, Settings }
    private sealed record Vehicle(string Id, string Name) { public override string ToString() => Name; }
    private sealed record Tile(GameCourse Track, BitmapImage? Picture) { public string Name => Track.Name; }
    private static readonly string[] styles = ["Classic", "N64 Mode", "Remastered"];
    private readonly string settingsFile;
    private LauncherSettings settings;
    private InstalledContent installed = new(null, null, []);
    private readonly CaptureLibrary pictures = new();
    private Page page;
    private bool syncing, closed, starting;
    private int libraryGeneration, pictureGeneration;
    private (string? Stage, string Car, RaceStyle Style, bool Custom)? tileKey;
    private string settingsSection = "Graphics";
    private string profile = "";
    private CancellationTokenSource? build;
    private bool closeAfterBuild;
    private readonly string? loadError;
    private string? buildReadError;
    private readonly DispatcherTimer padTimer = new();
    private ushort lastButtons;

    public GridWindow(string settingsPath, LauncherSettings initial, string? error = null)
    {
        settingsFile = settingsPath; settings = initial; loadError = error;
        InitializeComponent();
        if (initial.WindowSize is { IsValid: true } size)
        {
            var area = SystemParameters.WorkArea;
            Width = Math.Clamp(size.Width, MinWidth, Math.Max(MinWidth, area.Width));
            Height = Math.Clamp(size.Height, MinHeight, Math.Max(MinHeight, area.Height));
            if (size.Maximized) WindowState = WindowState.Maximized;
        }
        look = Enum.TryParse<LauncherLook>(settings.WpfTheme, out var saved) && Enum.IsDefined(saved) ? saved : LauncherLook.StartingGrid;
        SystemParameters.StaticPropertyChanged += SystemAppearance;
        DriverName.Text = Environment.UserName;
        Seats.ItemsSource = new[] { 2, 3, 4 }; Seats.SelectedItem = 4;
        LanStyle.ItemsSource = styles; LanStyle.SelectedIndex = 0;
        ModeChoice.ItemsSource = new[] { "Full game", "Quick Race" }; DisplayChoice.ItemsSource = new[] { "Desktop", "VR" };
        StyleChoice.ItemsSource = styles; WeatherChoice.ItemsSource = new[] { "Clear", "Partly cloudy", "Overcast", "Fog", "Rain" };
        HourChoice.ItemsSource = Enumerable.Range(0, 24).Select(h => $"{h:00}:00").ToArray();
        ModeChoice.SelectionChanged += (_, _) => EditSetup(() => settings = settings with
        {
            QuickRace = ModeChoice.SelectedIndex == 1,
            NormalVr = RaceLaunch.Vr(settings) && ModeChoice.SelectedIndex == 0,
            EasyRideVr = RaceLaunch.Vr(settings) && ModeChoice.SelectedIndex == 1
        });
        DisplayChoice.SelectionChanged += (_, _) => EditSetup(() => settings = settings with { NormalVr = DisplayChoice.SelectedIndex == 1 && !settings.QuickRace, EasyRideVr = DisplayChoice.SelectedIndex == 1 && settings.QuickRace });
        StyleChoice.SelectionChanged += (_, _) => EditSetup(() =>
        {
            var id = ((RaceStyle)StyleChoice.SelectedIndex).ToString();
            settings = RaceLaunch.Vr(settings) ? settings with { VrStyle = id } : settings.QuickRace ? settings with { QuickRaceStyle = id } : settings with { NormalStyle = id };
        });
        CarChoice.SelectionChanged += (_, _) => EditSetup(() =>
        {
            if (CarChoice.SelectedItem is Vehicle car) settings = RaceLaunch.Vr(settings) ? settings with { VrCar = car.Id } : settings with { Car = car.Id };
        });
        WeatherChoice.SelectionChanged += (_, _) => { if (!syncing && WeatherChoice.SelectedIndex >= 0) settings = settings with { RemasteredWeather = GameContent.WeatherIds[WeatherChoice.SelectedIndex] }; };
        HourChoice.SelectionChanged += (_, _) => { if (!syncing && HourChoice.SelectedIndex >= 0) settings = settings with { RemasteredHour = HourChoice.SelectedIndex }; };
        foreach (string section in new[] { "Graphics", "Controls", "Gameplay", "Content", "Game files", "Appearance" })
        {
            var link = new Button { Content = section, Style = (Style)FindResource("Nav"), HorizontalContentAlignment = System.Windows.HorizontalAlignment.Left, Margin = new Thickness(0, 0, 0, 4) };
            link.Click += (_, _) => ShowSettings(section); SettingsRail.Children.Add(link);
        }
        ApplyLook();
        Tiles.SizeChanged += (_, e) => Tiles.Tag = Math.Clamp((int)((e.NewSize.Width - 14) / 215), 2, 6);
        WireLobby();
        Loaded += async (_, _) => { ShowPage(Page.Play); await ReadBuild(); if (page == Page.Play) FocusPlayAction(); };
        Closing += OnClosing;
        ContentRendered += async (_, _) => await CheckUpdates(false);
        PreviewKeyDown += WindowKey;
        padTimer.Interval = TimeSpan.FromMilliseconds(90); padTimer.Tick += (_, _) => PollPad();
        Activated += (_, _) => { lastButtons = ushort.MaxValue; padTimer.Start(); }; Deactivated += (_, _) => padTimer.Stop();
        maximized = WindowState == WindowState.Maximized;
        MaximizeButton.Content = maximized ? "❐" : "□";
        StateChanged += (_, _) => {
            if (WindowState != WindowState.Minimized) maximized = WindowState == WindowState.Maximized;
            MaximizeButton.Content = maximized ? "❐" : "□";
        };
    }

    private void SystemAppearance(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(SystemParameters.HighContrast)) Dispatcher.InvokeAsync(ApplyContrast);
    }

    private void ApplyContrast()
    {
        foreach (var (key, brush) in new[] {
            ("Base", System.Windows.SystemColors.WindowBrush), ("Surface", System.Windows.SystemColors.WindowBrush),
            ("Field", System.Windows.SystemColors.ControlBrush), ("Line", System.Windows.SystemColors.WindowTextBrush),
            ("Ink", System.Windows.SystemColors.WindowTextBrush), ("Muted", System.Windows.SystemColors.ControlTextBrush),
            ("Orange",System.Windows.SystemColors.HighlightBrush), ("Gold",System.Windows.SystemColors.HighlightBrush),
            ("NavSelectedFill",System.Windows.SystemColors.HighlightBrush), ("NavSelectedInk",System.Windows.SystemColors.HighlightTextBrush),
            ("OnAccent",System.Windows.SystemColors.HighlightTextBrush) })
        {
            if (SystemParameters.HighContrast) Resources[key] = brush;
            else Resources.Remove(key);
        }
    }

    private Vehicle[] Cars(bool custom) => LanLobby.Cars.Concat(custom ? GameContent.CustomCars : []).Select(id => new Vehicle(id, GameContent.DisplayCar(id).ToUpperInvariant())).ToArray();
    private GameCourse[] Courses(bool custom) => GameContent.Courses.Concat(custom ? GameContent.CustomCourses : []).ToArray();
    private void EditSetup(Action edit) { if (syncing) return; edit(); RefreshSetup(); }
    private static Visibility Show(bool value) => value ? Visibility.Visible : Visibility.Collapsed;
    private void Say(string? text) { Notice.Text = text ?? ""; NoticeBar.Visibility = Show(!string.IsNullOrEmpty(text)); }

    private async Task ReadBuild()
    {
        int generation = ++libraryGeneration;
        string output = settings.OutputPath;
        try
        {
            var found = await Task.Run(() => InstalledContent.Read(output));
            if (closed || generation != libraryGeneration) return;
            installed = found; loadedOutput = output; buildReadError = null;
        }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException or ArgumentException or InvalidDataException)
        {
            if (closed || generation != libraryGeneration) return;
            installed = new(null, null, []); loadedOutput = output; buildReadError = e.Message;
        }
        pictures.Clear(); tileKey = null; RefreshSetup();
        if (page == Page.Settings) ShowSettings(settingsSection);
        if (loadError != null) Say(loadError);
    }

    private void RefreshSetup()
    {
        syncing = true;
        try
        {
            var car = RaceLaunch.Car(settings); var style = RaceLaunch.Style(settings); bool vr = RaceLaunch.Vr(settings);
            DeskFull.IsChecked = !settings.QuickRace; DeskQuick.IsChecked = settings.QuickRace;
            ModeChoice.SelectedIndex = settings.QuickRace ? 1 : 0; DisplayChoice.SelectedIndex = vr ? 1 : 0;
            int carCount = LanLobby.Cars.Length + (settings.CustomContent ? GameContent.CustomCars.Length : 0);
            if (CarChoice.Items.Count != carCount) CarChoice.ItemsSource = Cars(settings.CustomContent);
            CarChoice.SelectedValuePath = nameof(Vehicle.Id); CarChoice.SelectedValue = car;
            StyleChoice.SelectedIndex = (int)style;
            Conditions.Visibility = Show(style == RaceStyle.Remastered);
            HourChoice.SelectedIndex = Math.Clamp(settings.RemasteredHour, 0, 23);
            bool courseTime = settings.QuickRace && !vr && RaceLaunch.Course(settings) is "night-owl" or "night-cruise" or "harbor-city";
            HourChoice.IsEnabled = installed.Version >= GameContent.SunLightingVersion && !courseTime;
            HourChoice.ToolTip = courseTime ? "This course sets its own time of day." : null;
            WeatherChoice.IsEnabled = installed.Version >= GameContent.AtmosphereVersion;
            WeatherChoice.SelectedIndex = Math.Max(0, Array.IndexOf(GameContent.WeatherIds, settings.RemasteredWeather));
            CarChoice.Visibility = CarLabel.Visibility = Show(settings.QuickRace);
            DeskCourseField.Visibility = Show(DeskHome && settings.QuickRace);
            if (DeskCourseField.Visibility == Visibility.Visible)
            {
                int trackCount = GameContent.Courses.Length + (settings.CustomContent ? GameContent.CustomCourses.Length : 0);
                if (DeskCourse.Items.Count != trackCount) DeskCourse.ItemsSource = Courses(settings.CustomContent);
                DeskCourse.SelectedValue = RaceLaunch.Course(settings);
            }
            Tiles.Visibility = Show(!DeskHome && settings.QuickRace);
            DeskPreview.Visibility = Show(DeskHome);
            FullHint.Visibility = Show(!DeskHome && !settings.QuickRace);
            ModeTitle.Text = settings.QuickRace ? "QUICK RACE" : "FULL GAME";
            RaceName.Text = settings.QuickRace ? $"{GameContent.Course(RaceLaunch.Course(settings)).Name}  ·  {GameContent.DisplayCar(car)}".ToUpperInvariant() : "START AT THE TITLE SCREEN";
            DisplaySummary.Text = $"{(vr ? "VR" : "Desktop")}  ·  {styles[(int)style]}".ToUpperInvariant();
            CatalogTitle.Text = DeskHome ? "Race Desk" : settings.QuickRace ? "Course Atlas" : "Full game";
            SetupTitle.Text = DeskHome ? "Race setup" : settings.QuickRace ? GameContent.Course(RaceLaunch.Course(settings)).Name : "Launch setup";
            Start.Content = settings.QuickRace ? "START RACE" : "START GAME";
            var reason = installed.BlockReason(settings);
            Start.IsEnabled = reason == null && !starting && build == null;
            var rebuild = RebuildOffer;
            Repair.Content = rebuild?.ActionText ?? "Open game files";
            Repair.Visibility = Show(build == null && (reason != null || rebuild != null));
            if (page == Page.Play) Say(loadError ?? buildReadError ?? reason ?? rebuild?.Message);
            if (GalleryVisible) SyncSelectedTile();
        }
        finally { syncing = false; }
        _ = UpdatePictures();
    }

    private async Task UpdatePictures()
    {
        var generation = ++pictureGeneration;
        var style = RaceLaunch.Style(settings); var car = RaceLaunch.Car(settings);
        if (page == Page.Play && look != LauncherLook.CourseAtlas)
        {
            var file = settings.QuickRace ? GameContent.CaptureName(RaceLaunch.Course(settings), car, style, installed.Version) : "native-smoke.png";
            var image = await pictures.Load(installed.Stage, file, 960);
            if (!closed && generation == pictureGeneration) Hero.Source = image;
        }
        else if (GalleryVisible && !settings.QuickRace)
        {
            var image = await pictures.Load(installed.Stage, "native-smoke.png", 850);
            if (!closed && generation == pictureGeneration) BootArt.Source = image;
        }
        else if (GalleryVisible)
        {
            var key = (installed.Stage, car, style, settings.CustomContent);
            if (tileKey == key) return;
            var rows = await Task.WhenAll(Courses(settings.CustomContent).Select(async c => new Tile(c,
                await pictures.Load(installed.Stage, GameContent.CaptureName(c.Id, car, style, installed.Version), 320))));
            if (closed || generation != pictureGeneration) return;
            syncing = true;
            try { Tiles.ItemsSource = rows; tileKey = key; SyncSelectedTile(); } finally { syncing = false; }
        }
    }

    private void SyncSelectedTile()
    {
        var selected = Tiles.Items.OfType<Tile>().FirstOrDefault(t => t.Track.Id == RaceLaunch.Course(settings));
        if (selected != null && !ReferenceEquals(Tiles.SelectedItem, selected)) { Tiles.SelectedItem = selected; Tiles.ScrollIntoView(selected); }
    }
    private void TileChanged(object sender, SelectionChangedEventArgs e)
    {
        if (syncing || Tiles.SelectedItem is not Tile tile) return;
        SelectCourse(tile.Track.Id);
    }

    private void DeskModeChanged(object sender, RoutedEventArgs e)
    {
        if (!syncing && DeskHome) ModeChoice.SelectedIndex = sender == DeskQuick ? 1 : 0;
    }

    private void DeskCourseChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!syncing && DeskHome && DeskCourse.SelectedItem is GameCourse course) SelectCourse(course.Id);
    }

    private void SelectCourse(string id)
    {
        settings = RaceLaunch.Vr(settings) ? settings with { VrCourse = id } : settings with { Course = id };
        RefreshSetup();
    }

    private void ShowPage(Page next)
    {
        bool scan = page != Page.Network && next == Page.Network && lobby is not { Closed: false };
        if (page == Page.Network && next != Page.Network) search?.Cancel();
        bool animate = IsLoaded && page != next && SystemParameters.ClientAreaAnimation && !SystemParameters.HighContrast;
        page = next; Say(null);
        SetPlayLayout();
        Network.Visibility = Show(next == Page.Network); Options.Visibility = Show(next == Page.Settings);
        PlayNav.Tag = next is Page.Play or Page.Setup ? "active" : null; LanNav.Tag = next == Page.Network ? "active" : null;
        SettingsNav.Tag = next == Page.Settings ? "active" : null;
        if (animate)
        {
            Button tab = next switch { Page.Network => LanNav, Page.Settings => SettingsNav, _ => PlayNav };
            tab.BeginAnimation(OpacityProperty, new System.Windows.Media.Animation.DoubleAnimation(.6, 1, TimeSpan.FromMilliseconds(135))
            { FillBehavior = System.Windows.Media.Animation.FillBehavior.Stop });
        }
        if (next is Page.Play or Page.Setup) RefreshSetup();
        if (next == Page.Settings) ShowSettings(settingsSection);
        if (next == Page.Network) { UpdateRoom(); if (scan) _ = ScanAsync(); }
    }

    private void OpenHome(object sender, RoutedEventArgs e) { ShowPage(Page.Play); FocusPlayAction(); }
    private void OpenSetup(object sender, RoutedEventArgs e) { ShowPage(Page.Setup); ModeChoice.Focus(); }
    private void OpenNetwork(object sender, RoutedEventArgs e) => ShowPage(Page.Network);
    private void OpenGraphics(object sender, RoutedEventArgs e)
    {
        settingsSection = "Graphics";
        OpenOptions(sender, e);
        qualityPresetFocus?.Focus();
    }
    private void OpenOptions(object sender, RoutedEventArgs e) { profile = RaceLaunch.Profile(settings); ShowPage(Page.Settings); }
    private async void RepairGame(object sender, RoutedEventArgs e)
    {
        settingsSection = "Game files"; ShowPage(Page.Settings);
        if (RebuildOffer != null && loadError == null) await RunBuild(true);
    }
    private void Minimize(object sender, RoutedEventArgs e) => SystemCommands.MinimizeWindow(this);
    private void Maximize(object sender, RoutedEventArgs e) { if (WindowState == WindowState.Maximized) SystemCommands.RestoreWindow(this); else SystemCommands.MaximizeWindow(this); }
    private void Exit(object sender, RoutedEventArgs e) => Close();

    private bool switchingInterface;
    private bool maximized;
    private LauncherSettings CurrentSettings()
    {
        var display = RaceLaunch.Display(settings, RaceLaunch.Profile(settings));
        var bounds = RestoreBounds;
        return settings with
        {
            Resolution = display.Resolution,
            Borderless = display.Borderless,
            DesktopFps = display.DesktopFps,
            WindowSize = bounds.Width > 0 && bounds.Height > 0
                ? new LauncherWindowSize(bounds.Width, bounds.Height, maximized) : settings.WindowSize,
            N64Mode = RaceLaunch.Style(settings) == RaceStyle.N64,
            RemasteredMode = RaceLaunch.Style(settings) == RaceStyle.Remastered
        };
    }
    private void Save()
    {
        if (loadError != null || switchingInterface) return;
        BuildPipeline.AtomicWrite(settingsFile, JsonSerializer.Serialize(CurrentSettings(), BuildPipeline.JsonOptions));
    }
    private async void StartGame(object sender, RoutedEventArgs e)
    {
        if (!Start.IsEnabled || starting) return;
        starting = true; Start.IsEnabled = false;
        try { Save(); var snapshot = settings; using var game = await Task.Run(() => RaceLaunch.Start(snapshot)); if (!closed) Say("Game launched."); }
        catch (Exception ex) { Say(ex.Message); }
        finally { starting = false; Start.IsEnabled = installed.BlockReason(settings) == null && build == null; }
    }

    private void WindowKey(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Escape && Keyboard.FocusedElement is not ComboBox { IsDropDownOpen: true } && page != Page.Play)
        { ShowPage(Page.Play); FocusPlayAction(); e.Handled = true; }
        else if (e.Key == Key.Enter && page == Page.Play && Start.IsEnabled && look == LauncherLook.StartingGrid && Keyboard.FocusedElement is not Button)
        { StartGame(this, new RoutedEventArgs()); e.Handled = true; }
    }

    private void OnClosing(object? sender, CancelEventArgs e)
    {
        if (build != null) { closeAfterBuild = true; build.Cancel(); e.Cancel = true; return; }
        try { Save(); } catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { Say(ex.Message); }
        SystemParameters.StaticPropertyChanged -= SystemAppearance; closed = true; search?.Cancel(); lobby?.Dispose(); padTimer.Stop();
    }
}

