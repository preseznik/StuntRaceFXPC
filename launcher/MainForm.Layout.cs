namespace StuntRaceFX.Launcher;

internal sealed partial class MainForm
{
    private readonly TabControl settingsTabs = new LauncherTabs { Dock = DockStyle.Fill, Name = "SettingsTabs", Padding = new Point(12, 8) };
    private readonly Button browseCourses = new() { Name = "BrowseCourses", Text = "Browse courses…", AutoSize = true, Anchor = AnchorStyles.Left };
    private readonly Button remedy = ActionButton("Open game files…");
    private readonly Button conditions = ActionButton("Race conditions");
    private readonly TableLayoutPanel conditionsBody = GroupBody();
    private readonly TableLayoutPanel remasteredOptions = GroupBody();
    private readonly Label fpsLabel = Hint("Desktop driving frame limit", 2, 8);
    private TabPage settingsPage = null!, filesPage = null!;

    private static Button ActionButton(string title) => new() { Text = title, AutoSize = true, Anchor = AnchorStyles.Left };
    private static TableLayoutPanel GroupBody()
    {
        var panel = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 1, Margin = Padding.Empty };
        panel.ColumnStyles.Add(new(SizeType.Percent, 100));
        return panel;
    }

    private void OpenSettings(TabPage destination)
    {
        settingsTabs.SelectedTab = destination;
        tabs.SelectedTab = settingsPage;
        settingsTabs.Focus();
    }

    private void BuildPlay(TabPage play)
    {
        var layout = playLayout;
        layout.ColumnStyles.Add(new(SizeType.Percent, 46));
        layout.ColumnStyles.Add(new(SizeType.Percent, 54));
        layout.RowStyles.Add(new(SizeType.AutoSize));
        layout.RowStyles.Add(new(SizeType.Percent, 100));
        layout.RowStyles.Add(new(SizeType.AutoSize));
        var header = LauncherDesign.Columns(36,64);
        header.Margin = new Padding(0,0,0,12);
        header.Controls.Add(Heading("Play"));
        header.Controls.Add(launchMode);
        layout.Controls.Add(header); layout.SetColumnSpan(header, 2);
        visualPanel.Controls.Add(preview); visualPanel.Controls.Add(atlas);
        layout.Controls.Add(visualPanel, 0, 1);
        var scroll = setupScroll;
        var setup = LauncherDesign.CardBox("Race setup"); scroll.Controls.Add(setup);
        var fields = LauncherDesign.Column();
        void Field(Label label, ComboBox box)
        {
            label.Dock = DockStyle.Top; label.Tag = SurfaceRole.Caption;
            label.Margin = new Padding(0, 6, 0, 3);
            box.Dock = DockStyle.Top; box.Margin = new Padding(0,0,0,5);
            fields.Controls.Add(label); fields.Controls.Add(box);
        }
        Field(desktopCourseHeading, course); Field(vrCourseHeading, vrCourse);
        fields.Controls.Add(browseCourses);
        Field(desktopCarHeading, car); Field(vrCarHeading, vrCar);
        Field(Hint("Display"), displayTarget); Field(styleHeading, visualStyle);
        setup.Controls.Add(fields);
        browseCourses.Click += (_, _) => BrowseCourses();
        var graphics = ActionButton("Graphics settings…");
        graphics.Name = "GraphicsShortcut";
        graphics.Click += (_, _) => { graphicsProfile.SelectedItem = LaunchProfile; OpenSettings(settingsTabs.TabPages[0]); };
        setup.Controls.Add(graphics);
        weather.Items.AddRange(["Clear", "Partly cloudy", "Overcast", "Fog", "Rain"]);
        weather.SelectedIndex = 0;
        for (int hour = 0; hour < 24; hour++) timeOfDay.Items.Add($"{hour:00}:00");
        timeOfDay.SelectedIndex = 12;
        weather.SelectedIndexChanged += (_, _) => RefreshLaunch();
        timeOfDay.SelectedIndexChanged += (_, _) => RefreshLaunch();
        conditionsBody.Controls.AddRange([Hint("Weather", 2), weather, Hint("Time of day", 2), timeOfDay]);
        conditionsBody.Visible = false;
        conditions.Click += (_, _) => { conditionsExpanded = !conditionsExpanded; RefreshLaunch(); };
        setup.Controls.AddRange([conditions, conditionsBody]);
        setup.Controls.Add(session); session.Tag = SurfaceRole.Caption; session.Margin = new Padding(0,12,0,0);
        layout.Controls.Add(scroll, 1, 1);

        var footer = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 4, AutoSize = true, Padding = new Padding(0, 8, 0, 0) };
        footer.ColumnStyles.Add(new(SizeType.Percent, 100)); footer.ColumnStyles.Add(new(SizeType.AutoSize)); footer.ColumnStyles.Add(new(SizeType.AutoSize));
        status.Anchor = AnchorStyles.Left;
        footer.ColumnStyles.Add(new(SizeType.AutoSize));
        launch.Tag = SurfaceRole.Primary; footer.Controls.Add(status,0,0); footer.Controls.Add(remedy,1,0);
        footer.Controls.Add(changeSetup,2,0); footer.Controls.Add(launch,3,0);
        layout.Controls.Add(footer, 0, 2); layout.SetColumnSpan(footer, 2);
        play.Controls.Add(layout);
    }

    private bool conditionsExpanded;
    private void UpdateConditionsSummary(bool fixedTime)
    {
        conditions.Text = $"Race conditions  {(conditionsExpanded ? "−" : "+")}{Environment.NewLine}{weather.Text} · {(fixedTime ? "Course time" : timeOfDay.Text)}";
        conditionsBody.Visible = RemasteredSelected && conditionsExpanded;
    }

    private static Button AddSection(TableLayoutPanel parent, string title, params Control[] controls)
    {
        var toggle = ActionButton(title + "   +");
        toggle.Dock = DockStyle.Top; toggle.TextAlign = ContentAlignment.MiddleLeft;
        toggle.Tag = SurfaceRole.Section; toggle.Margin = new Padding(0, 2, 0, 4);
        var body = GroupBody(); body.Tag = SurfaceRole.Card; body.Padding = new Padding(16, 12, 16, 12); body.Visible = false;
        body.Controls.AddRange(controls);
        bool open = false;
        toggle.Click += (_, _) => { open = !open; body.Visible = open; toggle.Text = title + (open ? "   −" : "   +"); };
        parent.Controls.Add(toggle); parent.Controls.Add(body);
        return toggle;
    }

    private readonly TableLayoutPanel graphicsColumns = LauncherDesign.Columns(100,0);
    private readonly TableLayoutPanel deviceCard = LauncherDesign.CardBox("Display and headset"), styleCard = LauncherDesign.CardBox("Visual style");
    private void ArrangeGraphics()
    {
        bool split = look != LauncherLook.StartingGrid && graphicsColumns.Width >= LogicalToDeviceUnits(700);
        var cell = split ? new TableLayoutPanelCellPosition(1,0) : new TableLayoutPanelCellPosition(0,1);
        if (graphicsColumns.GetCellPosition(styleCard) != cell) graphicsColumns.SetCellPosition(styleCard,cell);
        graphicsColumns.ColumnStyles[0].Width = split ? 43 : 100;
        graphicsColumns.ColumnStyles[1].Width = split ? 57 : 0;
        deviceCard.Margin = split ? new Padding(0,0,10,12) : new Padding(0,0,0,12);
    }

    private void BuildGraphics()
    {
        var scroll = new Panel { Dock = DockStyle.Fill, AutoScroll = true };
        SettingsPage("Graphics").Controls.Add(scroll);
        var graphics = LauncherDesign.Column(); scroll.Controls.Add(graphics);
        graphics.Controls.Add(Heading("Graphics"));
        var profile = LauncherDesign.FieldBox("Settings for", graphicsProfile); profile.Margin = new Padding(0,0,0,14);
        graphics.Controls.Add(profile);
        graphics.Controls.Add(graphicsColumns);
        graphicsColumns.Controls.Add(deviceCard,0,0); graphicsColumns.Controls.Add(styleCard,0,1);
        graphicsColumns.SizeChanged += (_,_) => ArrangeGraphics();
        AddSection(deviceCard, "Desktop display", Hint("Window mode", 2), windowMode, Hint("Window resolution", 2), resolution,
            fpsLabel, desktopFps, hideRaceHud, Hint("F11 switches window mode during a race."));
        var browseRuntime = new Button { Text = "Choose runtime file…", AutoSize = true };
        browseRuntime.Click += (_, _) =>
        {
            using var pick = new OpenFileDialog { Filter = "OpenXR runtime manifests (*.json)|*.json" };
            if (pick.ShowDialog(this) != DialogResult.OK) return;
            try { OpenXrRuntime.ReadManifest(pick.FileName); RestoreVrRuntime(pick.FileName); }
            catch (Exception e) when (OpenXrRuntime.DiscoveryError(e)) { ShowError(e); }
        };
        AddSection(deviceCard, "VR headset", Hint("OpenXR runtime", 2), vrRuntime, browseRuntime, lastVrSession,
            Hint("Preferred refresh", 2), vrRefresh, Hint("Eye resolution", 2), vrScale, smoothVrMotion,
            Hint("Lower eye resolution reduces GPU work. The HUD stays at full resolution."));
        styleCard.Controls.AddRange([classicSnes, n64LowRaster, remasteredOptions]);
        remasteredOptions.Controls.Add(LauncherDesign.FieldBox("Anti-aliasing", msaa));
        AddSection(remasteredOptions, "Lighting and shadows", rayShadows, shadowQualityLabel, shadowQuality, indirect, rayReflections).Name = "LightingAndShadows";
        AddSection(remasteredOptions, "Surfaces and scenery", roadDetail, sceneryDetail, roughness, clearCoat);
        AddSection(remasteredOptions, "Weather effects", wetTireSpray, volumetricLighting);
        camera.Items.AddRange(["Chase", "Far chase", "Cockpit", "Bonnet / hood", "Isometric"]); camera.SelectedIndex = 0;
        var distance = Hint($"Maximum zoom distance: {cameraDistance.Value} m");
        cameraDistance.ValueChanged += (_, _) => distance.Text = $"Maximum zoom distance: {cameraDistance.Value} m";
        AddSection(remasteredOptions, "Camera", Hint("Starting camera", 2), camera, cameraLook, distance, cameraDistance, lightingHint);
        shadowQuality.ValueChanged += (_, _) => shadowQualityLabel.Text = shadowQuality.Value switch {
            0 => "Shadow quality: Performance (1 ray)", 1 => "Shadow quality: Balanced (2 rays)", _ => "Shadow quality: High (4 rays)"
        };
    }

    private void RestoreVrRuntime(string selected)
    {
        var choices = OpenXrRuntime.Choices(selected);
        vrRuntime.DataSource = choices;
        vrRuntime.SelectedItem = choices.First(r => r.Id.Equals(selected, StringComparison.OrdinalIgnoreCase));
    }

    private void BrowseCourses()
    {
        var picker = VrSelected ? vrCourse : course;
        var selectedCar = VrSelected ? VrCarId : CarId;
        using var gallery = new CourseGallery(picker.Items.Cast<GameCourse>(), (GameCourse)picker.SelectedItem!,
            output.Text, selectedCar, SelectedStyle);
        design.Apply(gallery);
        if (gallery.ShowDialog(this) == DialogResult.OK) picker.SelectedItem = gallery.Selection;
        browseCourses.Focus();
    }
}
