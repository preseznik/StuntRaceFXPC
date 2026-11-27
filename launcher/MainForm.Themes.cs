namespace StuntRaceFX.Launcher;

internal sealed partial class MainForm
{
    private LauncherLook look;
    private GridNavigation? gridNavigation;

    internal bool ActivateLobbyControl(Control? target) => networkPanel.ActivateFocused(target);
    internal bool HandleLobbyMenu(ushort buttons) => networkPanel.NavigateMenu(buttons);
    internal void StepDestination(int direction)
    {
        tabs.SelectedIndex = (tabs.SelectedIndex+direction+tabs.TabCount)%tabs.TabCount;
        if (tabs.SelectedIndex == 0 && launch.Enabled) launch.Focus();
    }
    internal void BackFromGrid()
    {
        if (tabs.SelectedIndex != 0) tabs.SelectedIndex = 0;
        else if (gridEditing) { gridEditing = false; ArrangePlay(); changeSetup.Focus(); }
    }

    private LauncherDesign design = LauncherDesign.For(LauncherLook.RaceDesk);
    private readonly ComboBox themePicker = new() { Name = "LauncherTheme", AccessibleName = "Launcher theme",
        DropDownStyle = ComboBoxStyle.DropDownList, Width = 160, Anchor = AnchorStyles.Right };
    private MultiplayerPanel networkPanel = null!;
    private readonly TableLayoutPanel playLayout = new() { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 3 };
    private readonly Panel visualPanel = new() { Dock = DockStyle.Fill, Margin = new Padding(0,0,12,0) };
    private readonly Panel setupScroll = new() { Dock = DockStyle.Fill, AutoScroll = true, Name = "PlaySetup", Margin = Padding.Empty };
    private readonly CourseTiles atlas = new() { Name = "AtlasCourses" };
    private static readonly Font GridActionFont = new("Bahnschrift",13,FontStyle.Bold);
    private readonly Button changeSetup = ActionButton("Change setup");
    private readonly List<RadioButton> appearanceChoices = [];
    private bool designReady, gridEditing, syncingTiles;
    private (string? Stage, string Car, RaceStyle Style, int Count)? atlasCatalog;

    protected override void OnSystemColorsChanged(EventArgs e)
    {
        base.OnSystemColorsChanged(e);
        if (designReady && !IsDisposed && !Disposing) ApplyLook(look);
    }

    private void BuildShell()
    {
        var shell = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2, Margin = Padding.Empty };
        shell.ColumnStyles.Add(new(SizeType.Percent,100));
        shell.RowStyles.Add(new(SizeType.AutoSize)); shell.RowStyles.Add(new(SizeType.Percent,100));
        var bar = new TableLayoutPanel { ColumnCount = 3, Dock = DockStyle.Top, AutoSize = true,
            Padding = new Padding(18,8,18,8), Margin = Padding.Empty, Tag = SurfaceRole.Card };
        bar.ColumnStyles.Add(new(SizeType.Percent,100)); bar.ColumnStyles.Add(new(SizeType.AutoSize)); bar.ColumnStyles.Add(new(SizeType.AutoSize));
        var brand = new Label { Text = "STUNT RACE FX   /   PC", Font = new Font("Bahnschrift",15,FontStyle.Bold),
            AutoSize = true, Anchor = AnchorStyles.Left, Margin = Padding.Empty };
        var themeLabel = new Label { Text = "Launcher theme", AutoSize = true, Anchor = AnchorStyles.Right,
            Tag = SurfaceRole.Caption, Margin = new Padding(8,0,10,0) };
        themePicker.Items.AddRange(LauncherDesign.Names);
        themePicker.SelectedIndexChanged += (_,_) => { if (themePicker.SelectedIndex >= 0 && (int)look != themePicker.SelectedIndex) ApplyLook((LauncherLook)themePicker.SelectedIndex); };
        bar.Controls.AddRange([brand,themeLabel,themePicker]);
        settingsTabs.SizeChanged += (_,_) => ArrangeSettings();
        shell.Controls.AddRange([bar,tabs]); Controls.Add(shell);
        changeSetup.Name = "ChangeSetup";
        changeSetup.Click += (_,_) => { gridEditing = !gridEditing; ArrangePlay(); if (gridEditing) setupScroll.SelectNextControl(null,true,true,true,false); };
        atlas.SelectedIndexChanged += (_,_) => {
            if (syncingTiles || look != LauncherLook.CourseAtlas || atlas.SelectedItems.Count != 1) return;
            (VrSelected ? vrCourse : course).SelectedItem = atlas.SelectedItems[0].Tag;
        };
    }

    private void ApplyLook(LauncherLook selected)
    {
        SuspendLayout();
        try
        {
            look = selected; design = LauncherDesign.For(look); themePicker.SelectedIndex = (int)look;
            tabs.Alignment = look switch { LauncherLook.CourseAtlas => TabAlignment.Top, LauncherLook.StartingGrid => TabAlignment.Bottom, _ => TabAlignment.Left };
            tabs.ItemSize = LogicalToDeviceUnits(look == LauncherLook.RaceDesk ? new Size(46,122) : new Size(148,40));
            foreach (TabPage page in tabs.TabPages) page.Padding = new Padding(LogicalToDeviceUnits(look == LauncherLook.StartingGrid ? 18 : 14));
            foreach (var choice in appearanceChoices) choice.Checked = choice.TabIndex == (int)look;
            ArrangeSettings(); ArrangeGraphics();
            design.Apply(this);
            networkPanel.SetDesign(design);
            ArrangePlay();
            gridNavigation ??= new GridNavigation(this);
            gridNavigation.Enable(look == LauncherLook.StartingGrid);
            designReady = true; RefreshLaunch();
        }
        finally { ResumeLayout(true); }
    }

    private readonly CheckBox classicLauncher = new() { Name = "ClassicLook", Text = "Classic Look (restarts launcher)", Checked = true, AutoSize = true };

    private void BuildAppearance()
    {
        var page = Stack(); SettingsPage("Appearance").Controls.Add(page);
        var card = LauncherDesign.CardBox("Classic launcher themes"); page.Controls.Add(card); card.Controls.Add(classicLauncher);
        classicLauncher.Click += (_, _) =>
        {
            try
            {
                Program.SwitchInterface(settingsFile, CurrentSettings(), false);
                switchingInterface = true;
                Close();
            }
            catch (Exception error) { classicLauncher.Checked = true; ShowError(error); }
        };
        string[] descriptions = ["Race preview and setup side by side, with navigation on the left.",
            "A gallery of course captures with setup beside the selected track.",
            "A full-width race preview, larger targets and controller navigation."];
        for (var index = 0; index < LauncherDesign.Names.Length; ++index)
        {
            var option = new RadioButton { Text = LauncherDesign.Names[index], AutoSize = true,
                TabIndex = index, Margin = new Padding(0,8,0,1) };
            var value = (LauncherLook)index;
            option.CheckedChanged += (_,_) => { if (option.Checked && look != value) ApplyLook(value); };
            appearanceChoices.Add(option);
            var hint = Hint(descriptions[index]); hint.Margin = new Padding(24,0,0,10); hint.Tag = SurfaceRole.Caption;
            card.Controls.AddRange([option,hint]);
        }
        var padHelp = Hint("Starting Grid controller: D-pad up/down moves focus. Left/right changes a value. A selects, B goes back, and the bumpers switch pages.");
        padHelp.Margin = new Padding(0,12,0,0); card.Controls.Add(padHelp);
    }

    private void ArrangeSettings()
    {
        bool sidebar = look == LauncherLook.CourseAtlas && settingsTabs.Width >= LogicalToDeviceUnits(820);
        var alignment = sidebar ? TabAlignment.Left : TabAlignment.Top;
        var size = LogicalToDeviceUnits(sidebar ? new Size(42,130) :
            look == LauncherLook.StartingGrid ? new Size(108,44) : new Size(98,34));
        if (settingsTabs.Alignment != alignment) settingsTabs.Alignment = alignment;
        settingsTabs.SizeMode = TabSizeMode.Fixed;
        if (settingsTabs.ItemSize != size) settingsTabs.ItemSize = size;
    }

    private void ArrangePlay()
    {
        bool grid = look == LauncherLook.StartingGrid;
        bool summary = grid && !gridEditing;
        setupScroll.Visible = !summary;
        launch.Font = grid ? GridActionFont : Font;
        launch.MinimumSize = grid ? LogicalToDeviceUnits(new Size(145,42)) : LogicalToDeviceUnits(new Size(114,34));
        changeSetup.MinimumSize = grid ? LogicalToDeviceUnits(new Size(128,42)) : Size.Empty;
        changeSetup.Visible = grid; changeSetup.Text = summary ? "Change setup" : "Done";
        playLayout.SetColumnSpan(visualPanel, summary ? 2 : 1);
        float left = look == LauncherLook.CourseAtlas ? 58 : 45;
        playLayout.ColumnStyles[0].Width = left; playLayout.ColumnStyles[1].Width = 100-left;
        visualPanel.Margin = summary ? Padding.Empty : new Padding(0,0,12,0);
    }

    private void UpdateAtlas(string? stage)
    {
        bool show = look == LauncherLook.CourseAtlas && QuickRaceSelected;
        atlas.Visible = show; preview.Visible = !show;
        if (!show || syncingTiles) return;
        var picker = VrSelected ? vrCourse : course;
        var selectedCar = VrSelected ? VrCarId : CarId;
        var catalog = (stage, selectedCar, SelectedStyle, picker.Items.Count);
        syncingTiles = true;
        try
        {
            if (atlasCatalog != catalog)
            {
                atlas.LoadCourses(picker.Items.Cast<GameCourse>(), (GameCourse)picker.SelectedItem!, output.Text, selectedCar, SelectedStyle);
                atlasCatalog = catalog;
            }
            var index = picker.SelectedIndex;
            if (index >= 0 && !atlas.Items[index].Selected)
            {
                var tile = atlas.Items[index]; tile.Selected = tile.Focused = true; tile.EnsureVisible();
            }
        }
        finally { syncingTiles = false; }
    }

    private void BuildControlsGameplayAndContent()
    {
        var controls = Stack(); SettingsPage("Controls").Controls.Add(controls);
        var pad = LauncherDesign.CardBox("Controller");
        pad.Controls.Add(Hint("Configure button mappings, steering and stick deadzone.")); pad.Controls.Add(controller);
        var keys = LauncherDesign.CardBox("Keyboard");
        var map = LauncherDesign.Columns(32,68);
        foreach (var (action,binding) in new[] { ("Accelerate","W or Up"), ("Brake","S or Down"), ("Steer","A and D"),
            ("Boost","Space"), ("Drift (Enhanced)","Hold Shift"), ("Reverse","X"), ("Retire or restart","R"), ("Pause","P"), ("Fullscreen","F11") })
        {
            map.Controls.Add(Hint(action,5)); map.Controls.Add(Hint(binding,5));
        }
        keys.Controls.Add(map); controls.Controls.AddRange([pad,keys]);
        var gameplay = Stack(); SettingsPage("Gameplay").Controls.Add(gameplay);
        var driving = LauncherDesign.CardBox("Driving physics");
        driving.Controls.AddRange([enhancedPhysics,Hint(GameContent.DrivingPhysicsHelp),practice]);
        gameplay.Controls.Add(driving);
        var content = Stack(); SettingsPage("Content").Controls.Add(content);
        var library = LauncherDesign.CardBox("Custom content");
        library.Controls.AddRange([customContent,Hint("Include installed custom tracks and vehicles in Quick Race and LAN lobbies.")]);
        content.Controls.Add(library);
    }
}

