using System.Diagnostics;
using System.Net;
using System.Net.Sockets;

namespace StuntRaceFX.Launcher;

internal sealed class MultiplayerPanel : UserControl
{
    private readonly TextBox name = new() { Text = Environment.UserName, MaxLength = 32, Width = 140 };
    private readonly TextBox address = new() { PlaceholderText = "Host IP or name", Width = 158 };
    private readonly Button host = Button("Host"), join = Button("Join"), leave = Button("Leave");
    private readonly ListBox servers = new() { Width = 410, Height = 56, IntegralHeight = false, AccessibleName = "Discovered lobbies" };
    private readonly Button refresh = Button("Refresh");
    private readonly Label scanStatus = Label("");
    private CancellationTokenSource? scan;
    private bool tabOpen;
    private readonly ComboBox track = List(), car = List("4WD", "Coupe", "F-Type");
    private readonly NumericUpDown capacity = new() { Minimum = 2, Maximum = LanLobby.MaxDrivers, Value = LanLobby.MaxDrivers, Width = 46, AccessibleName = "Lobby capacity" };
    private readonly ComboBox style = List("Classic", "N64 Mode", "Remastered");
    private readonly Button ready = Button("Ready"), start = Button("Start"), more = Button("More…");
    private readonly ToolStripMenuItem force = new("Force start");
    private readonly ContextMenuStrip hostMenu = new();
    private readonly TableLayoutPanel discovery = PageLayout(), room = PageLayout();
    private readonly Panel discoveryChoices = ChoiceSlot(), roomChoices = ChoiceSlot();
    private readonly TableLayoutPanel choices;
    private readonly TableLayoutPanel roomBody = LauncherDesign.Columns(58,42);
    private readonly TableLayoutPanel grid = LauncherDesign.CardBox("Starting grid"), messages = LauncherDesign.CardBox("Lobby chat");
    private LauncherLook look;
    private (bool Atlas,bool Side,bool Compact)? discoveryLayout;

    private readonly TableLayoutPanel sections = LauncherDesign.Columns(100,0);
    private readonly TableLayoutPanel browser = LauncherDesign.CardBox("Available lobbies");
    private string? previewStage, bannerPath;
    private int buildVersion;
    private string defaultTrack = GameContent.DefaultCourse;
    private readonly TableLayoutPanel actions = LauncherDesign.Columns(59, 41);
    private readonly TableLayoutPanel driverSetup = LauncherDesign.CardBox("Driver setup");
    private readonly TableLayoutPanel joinCard = LauncherDesign.CardBox("Join a lobby"), hostCard = LauncherDesign.CardBox("Host a lobby");
    private readonly RaceBanner banner = new() { Text = "Multiplayer", Subtitle = "Race with drivers on your local network." };
    private readonly Label emptyHint = Label("Join by address or host a race below.");
    private readonly Label waiting = Label(""), discoveryError = Label("");
    private readonly Label trackLabel;
    private readonly ListView drivers = new() { View = View.Details, HeaderStyle = ColumnHeaderStyle.Nonclickable,
        FullRowSelect = true, MultiSelect = false, Dock = DockStyle.Top, AccessibleName = "Connected drivers" };
    private readonly ListBox chat = new() { Dock = DockStyle.Fill, IntegralHeight = false, AccessibleName = "Lobby chat" };
    private readonly TextBox message = new() { PlaceholderText = "Message the lobby", MaxLength = 256, Width = 300 };
    private readonly Button send = Button("Send");
    private readonly Func<LanGrid, RaceStyle, Process> launch;
    private LanLobby? lobby;
    internal event Action<bool>? ConnectionChanged;
    private bool customContent;
    [System.ComponentModel.DefaultValue(false)]
    internal bool EnhancedPhysics { get; set; }
    internal void SetCustomContent(bool enabled)
    {
        customContent = enabled;
        UpdateChoices(enabled);
        servers.Items.Clear(); servers.Visible = false; emptyHint.Visible = true;
    }
    private void UpdateChoices(bool enabled)
    {
        if (enabled == car.Items.Contains(GameContent.CustomCarName)) return;
        if (enabled) {
            track.Items.AddRange(GameContent.CustomCourses);
            car.Items.AddRange(GameContent.CustomCars.Select(GameContent.DisplayCar).ToArray());
        } else {
            if (track.SelectedItem is GameCourse selected && GameContent.IsCustomCourse(selected.Id)) track.SelectedIndex = 0;
            if (GameContent.CustomCars.Contains(Car)) car.SelectedIndex = 0;
            foreach (var course in GameContent.CustomCourses) track.Items.Remove(course);
            foreach (var id in GameContent.CustomCars) car.Items.Remove(GameContent.DisplayCar(id));
        }
    }
    private bool busy;
    private string Car => car.Text == GameContent.CustomCarName ? GameContent.CustomCar : car.Text.ToLowerInvariant();

    internal MultiplayerPanel(Func<LanGrid, RaceStyle, Process> launch)
    {
        this.launch = launch;
        Dock = DockStyle.Fill;
        name.AccessibleName = "Driver name"; address.AccessibleName = "Host address";
        track.Items.AddRange(GameContent.LanCourseVersions.Keys.Where(id => GameContent.AllowsCourse(id, false)).Select(GameContent.Course).ToArray()); track.SelectedIndex = 0;
        track.Name = "LanTrack"; car.Name = "LanCar"; style.Name = "LanStyle";
        track.AccessibleName = "Track"; car.AccessibleName = "Your car"; style.AccessibleName = "Your visual style";
        foreach (string column in new[] { "Seat", "Driver", "Car", "Status" }) drivers.Columns.Add(column);
        drivers.SizeChanged += (_, _) => {
            int unit = Math.Max(1, (drivers.ClientSize.Width - 8) / 10);
            drivers.Columns[0].Width = unit * 2; drivers.Columns[1].Width = unit * 3;
            drivers.Columns[2].Width = unit * 2; drivers.Columns[3].Width = unit * 3;
        };
        servers.Dock = DockStyle.Top; servers.Height = 68; servers.Visible = false;
        choices = LauncherDesign.Columns(48, 52);
        choices.Controls.Add(LauncherDesign.FieldBox("Your car", car));
        choices.Controls.Add(LauncherDesign.FieldBox("Your visual style", style));
        discoveryChoices.Controls.Add(choices);
        var identity = LauncherDesign.Columns(34, 66);
        identity.Controls.Add(LauncherDesign.FieldBox("Driver name", name)); identity.Controls.Add(discoveryChoices);
        driverSetup.Controls.Add(identity);

        var scanRow = LauncherDesign.Columns(100,0);
        scanRow.ColumnStyles[1].SizeType = SizeType.AutoSize;
        var heading = browser.Controls[0];
        heading.Anchor = AnchorStyles.Left; heading.Dock = DockStyle.None; heading.Margin = Padding.Empty;
        refresh.Name = "RefreshLobbies"; refresh.Anchor = AnchorStyles.Right; refresh.Margin = Padding.Empty;
        scanRow.Controls.AddRange([heading,refresh]); scanRow.Margin = new Padding(0,0,0,6);
        scanStatus.Dock = DockStyle.Top; scanStatus.TextAlign = ContentAlignment.MiddleLeft;
        browser.Controls.AddRange([scanRow,scanStatus, servers, emptyHint]);
        emptyHint.Tag = SurfaceRole.Caption;
        emptyHint.AutoSize = true; emptyHint.Dock = DockStyle.Top;

        joinCard.Controls.Add(LauncherDesign.FieldBox("Host address", address)); joinCard.Controls.Add(join);
        hostCard.Controls.Add(LauncherDesign.FieldBox("Driver seats", capacity)); hostCard.Controls.Add(host);
        join.Tag = host.Tag = start.Tag = SurfaceRole.Primary;
        join.Margin = host.Margin = new Padding(0,6,0,0);
        scanStatus.Margin = emptyHint.Margin = Padding.Empty;
        actions.Controls.Add(joinCard, 0, 0); actions.Controls.Add(hostCard, 1, 0);
        sections.SizeChanged += (_,_) => ArrangeDiscovery();
        sections.Controls.Add(driverSetup,0,0); sections.Controls.Add(browser,0,1);
        sections.Controls.Add(actions,0,2); sections.Controls.Add(discoveryError,0,3);
        var findScroll = new Panel { AutoScroll = true, Dock = DockStyle.Fill };
        findScroll.SizeChanged += (_,_) => ArrangeDiscovery();
        findScroll.Controls.Add(sections); discovery.Controls.Add(findScroll);
        discovery.RowStyles.Add(new(SizeType.Percent, 100));

        var trackField = LauncherDesign.FieldBox("Track", track);
        trackLabel = (Label)trackField.Controls[0];
        grid.Controls.AddRange([trackField,drivers,roomChoices,Row(ready,start,more,leave),waiting]);

        chat.Dock = DockStyle.Top; chat.Height = 160;
        var composer = LauncherDesign.Columns(100,0);
        composer.ColumnStyles[1].SizeType = SizeType.AutoSize;
        message.Dock = DockStyle.Top; send.Anchor = AnchorStyles.Right; send.Margin = new Padding(8,0,0,0);
        composer.Controls.AddRange([message,send]); messages.Controls.AddRange([chat,composer]);
        roomBody.Controls.Add(grid,0,0); roomBody.Controls.Add(messages,1,0);
        roomBody.SizeChanged += (_,_) => ArrangeRoom();
        var roomScroll = new Panel { Dock = DockStyle.Fill, AutoScroll = true };
        roomScroll.Controls.Add(roomBody); room.Controls.Add(roomScroll); room.RowStyles.Add(new(SizeType.Percent,100));
        var pages = new Panel {Dock = DockStyle.Fill}; pages.Controls.Add(discovery); pages.Controls.Add(room);
        var frame = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2 };
        frame.ColumnStyles.Add(new(SizeType.Percent,100)); frame.RowStyles.Add(new(SizeType.Absolute,108)); frame.RowStyles.Add(new(SizeType.Percent,100));
        banner.Margin = new Padding(0,0,0,10);
        frame.Controls.AddRange([banner,pages]); Controls.Add(frame);
        frame.SizeChanged += (_,_) => {
            frame.RowStyles[0].Height = LogicalToDeviceUnits(frame.Height < LogicalToDeviceUnits(500) ? 68 : 96);
        };
        servers.KeyDown += (_,e) => {
            if (e.KeyCode == Keys.Enter && servers.SelectedItem != null)
            { e.SuppressKeyPress = true; join.PerformClick(); }
        };
        servers.DoubleClick += (_,_) => { if (servers.SelectedItem != null) join.PerformClick(); };
        address.TextChanged += (_,_) => join.Enabled = !busy && lobby is not { Closed:false } && address.Text.Trim().Length > 0;
        hostMenu.Items.Add(force);
        more.Click += (_, _) => hostMenu.Show(more, new Point(0, more.Height));
        style.SelectedIndexChanged += (_,_) => UpdateBannerImage();
        car.SelectedIndexChanged += (_,_) => UpdateBannerImage();
        refresh.Click += async (_, _) => await Scan();
        servers.SelectedIndexChanged += (_, _) =>
        {
            if (servers.SelectedItem is DiscoveredLobby server) address.Text = server.Address.ToString();
        };
        host.Click += async (_, _) => await Execute(() => Connect(true));
        join.Click += async (_, _) => await Execute(() => Connect(false));
        leave.Click += (_, _) => lobby?.Dispose();
        car.SelectedIndexChanged += async (_, _) => await Execute(() => lobby?.SetDriver(Car, false) ?? Task.CompletedTask);
        track.SelectedIndexChanged += async (_, _) => await Execute(() => lobby?.SelectCourse(((GameCourse)track.SelectedItem!).Id) ?? Task.CompletedTask);
        ready.Click += async (_, _) => await Execute(() => lobby!.SetDriver(Car, !lobby.Local.Ready));
        start.Click += async (_, _) => await Execute(() => lobby!.Start(false));
        force.Click += async (_, _) => await Execute(() => lobby!.Start(true));
        send.Click += async (_, _) => await Execute(SendChat);
        message.KeyDown += async (_, e) =>
        {
            if (e.KeyCode != Keys.Enter) return;
            e.SuppressKeyPress = true;
            await Execute(SendChat);
        };
        RefreshLobby();
    }

    internal void SetPreviewSource(string? stage, int version, string trackId)
    {
        previewStage = stage; buildVersion = version; defaultTrack = trackId; UpdateBannerImage();
    }

    private void UpdateBannerImage()
    {
        string trackId = lobby is { Closed:false } ? lobby.Course : defaultTrack;
        string? path = previewStage == null ? null : Path.Combine(previewStage,
            GameContent.CaptureName(trackId,Car,(RaceStyle)style.SelectedIndex,buildVersion));
        if (path == bannerPath) return;
        bannerPath = path;
        var previous = banner.CourseImage; banner.CourseImage = null;
        try
        {
            if (path != null && File.Exists(path))
            {
                using var source = Image.FromFile(path); banner.CourseImage = new Bitmap(source);
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or OutOfMemoryException) { }
        finally { previous?.Dispose(); banner.Invalidate(); }
    }

    internal void SetDesign(LauncherDesign design)
    {
        look = design.Look; ArrangeRoom(); ArrangeDiscovery();
        banner.Design = design; banner.Invalidate();
        bool stack = design.Look == LauncherLook.StartingGrid;
        actions.SetCellPosition(hostCard, stack ? new TableLayoutPanelCellPosition(0,1) : new TableLayoutPanelCellPosition(1,0));
        actions.ColumnStyles[0].Width = stack ? 100 : 59; actions.ColumnStyles[1].Width = stack ? 0 : 41;
        joinCard.Margin = stack ? new Padding(0,0,0,10) : new Padding(0,0,6,12);
        hostCard.Margin = stack ? Padding.Empty : new Padding(6,0,0,12);
        hostMenu.BackColor = design.Card; hostMenu.ForeColor = design.Text;
    }

    private void ArrangeDiscovery()
    {
        if (driverSetup.Parent == null) return;
        bool side = look == LauncherLook.CourseAtlas && sections.Width >= LogicalToDeviceUnits(820);
        bool atlas = look == LauncherLook.CourseAtlas;
        bool compact = sections.Parent != null && sections.Parent.Height < LogicalToDeviceUnits(475);
        if (discoveryLayout == (atlas,side,compact)) return;
        discoveryLayout = (atlas,side,compact);
        sections.SuspendLayout();
        try
        {
            sections.ColumnStyles[0].Width = side ? 49 : 100; sections.ColumnStyles[1].Width = side ? 51 : 0;
            sections.SetCellPosition(browser,new(0,atlas || compact ? 0 : 1));
            sections.SetCellPosition(driverSetup,side ? new(1,0) : new(0,compact ? 2 : atlas ? 1 : 0));
            sections.SetCellPosition(actions,new(0,side || compact ? 1 : 2));
            sections.SetCellPosition(discoveryError,new(0,side ? 2 : 3));
            sections.SetColumnSpan(actions,side ? 2 : 1);
            driverSetup.Margin = side ? new Padding(8,0,0,12) : new Padding(0,0,0,12);
        }
        finally { sections.ResumeLayout(true); }
    }

    private void ArrangeRoom()
    {
        bool split = look != LauncherLook.StartingGrid && roomBody.Width >= LogicalToDeviceUnits(780);
        var position = split ? new TableLayoutPanelCellPosition(1,0) : new TableLayoutPanelCellPosition(0,1);
        if (roomBody.GetCellPosition(messages) != position) roomBody.SetCellPosition(messages,position);
        roomBody.ColumnStyles[0].Width = split ? 58 : 100;
        roomBody.ColumnStyles[1].Width = split ? 42 : 0;
        grid.Margin = split ? new Padding(0,0,8,12) : new Padding(0,0,0,12);
    }

    internal bool ActivateFocused(Control? control)
    {
        if (control == servers && servers.SelectedItem != null) { join.PerformClick(); return true; }
        if (control == message) { send.PerformClick(); return true; }
        return false;
    }

    internal bool NavigateMenu(ushort buttons)
    {
        if (!hostMenu.Visible) return false;
        if ((buttons & 0x2000) != 0) hostMenu.Close();
        else if ((buttons & 0x1010) != 0 && force.Enabled) { hostMenu.Close(); force.PerformClick(); }
        else force.Select();
        return true;
    }

    internal void SetTabOpen(bool open)
    {
        if (tabOpen == open) return;
        tabOpen = open;
        if (open && lobby is not { Closed: false }) _ = Scan();
        else CancelScan();
    }

    private void CancelScan()
    {
        scan?.Cancel();
        scan = null;
        refresh.Enabled = true;
    }

    private async Task Scan()
    {
        if (scan != null) return;
        using var current = new CancellationTokenSource();
        scan = current;
        refresh.Enabled = false;
        scanStatus.Text = "Scanning…"; emptyHint.Visible = false;
        // Discovery can also run in a launcher hosted without a WinForms synchronization context.
        // Create and update the newly visible results control on its owning UI thread.
        _ = Handle;
        DiscoveredLobby[] results = [];
        string? failure = null;
        try { results = await LanDiscovery.Scan(current.Token, customContent).ConfigureAwait(false); }
        catch (OperationCanceledException) when (current.IsCancellationRequested) { return; }
        catch (Exception error) when (error is SocketException or System.Net.NetworkInformation.NetworkInformationException)
        { failure = "Scan failed. Join by IP or refresh."; }
        if (current.IsCancellationRequested) return;
        try
        {
            await InvokeAsync(() =>
            {
                if (current.IsCancellationRequested || scan != current) return;
                scan = null; refresh.Enabled = true;
                if (failure != null) { scanStatus.Text = failure; return; }
                var previous = servers.SelectedItem as DiscoveredLobby;
                servers.Items.Clear(); servers.Items.AddRange(results);
                servers.Visible = results.Length > 0; emptyHint.Visible = results.Length == 0;
                if (previous != null && previous.Address.ToString() == address.Text)
                {
                    servers.SelectedItem = results.FirstOrDefault(item => item.Address.Equals(previous.Address));
                    if (servers.SelectedIndex < 0) address.Clear();
                }
                scanStatus.Text = results.Length switch { 0 => "No lobbies found", 1 => "1 lobby found", _ => $"{results.Length} lobbies found" };
            }, current.Token);
        }
        catch (OperationCanceledException) when (current.IsCancellationRequested) { }
        catch (InvalidOperationException) when (IsDisposed || Disposing) { }
    }

    private async Task Execute(Func<Task> action)
    {
        try { await action(); }
        catch (Exception error)
        {
            if (lobby is { Closed: false }) lobby.Close(error.Message);
            else Append(error.Message);
        }
        finally { if (!IsDisposed) RefreshLobby(); }
    }

    private async Task Connect(bool hosting)
    {
        if (busy || lobby is { Closed: false }) return;
        busy = true;
        try
        {
            CancelScan(); discoveryError.Text = ""; chat.Items.Clear();
            lobby = new LanLobby(hosting, name.Text, Car, grid => launch(grid, (RaceStyle)style.SelectedIndex), (int)capacity.Value,
                customContent, EnhancedPhysics, buildVersion >= GameContent.RacerPhysicsVersion);
            lobby.Changed += RefreshLobby;
            lobby.Log += Append;
            RefreshLobby();
            if (hosting)
            {
                lobby.Listen();
                var ips = Dns.GetHostAddresses(Dns.GetHostName()).Where(a => a.AddressFamily == AddressFamily.InterNetwork);
                Append($"Hosting on {string.Join(", ", ips)} (TCP {LanLobby.Port}).");
            }
            else await lobby.Join(address.Text);
        }
        finally { busy = false; }
    }

    private async Task SendChat()
    {
        if (lobby?.Connected != true) return;
        var text = message.Text; message.Clear();
        await lobby.Chat(text);
    }

    private void RefreshLobby()
    {
        if (IsDisposed || Disposing) return;
        if (InvokeRequired) { BeginInvoke(RefreshLobby); return; }
        bool active = lobby is { Closed: false }, connected = active && lobby!.Connected;
        ConnectionChanged?.Invoke(active || busy);
        bool wasInRoom = choices.Parent == roomChoices;
        if (active != wasInRoom)
        {
            (active ? roomChoices : discoveryChoices).Controls.Add(choices);
            if (!active && tabOpen) _ = Scan();
        }
        room.Visible = active;
        discovery.Visible = !active;
        bool racing = active && lobby!.Racing;
        capacity.Enabled = servers.Enabled = name.Enabled = address.Enabled = host.Enabled = join.Enabled = !active && !busy;
        if (active && lobby!.Connected) capacity.Value = lobby.Drivers.Length;
        leave.Enabled = active;
        style.Enabled = car.Enabled = !racing;
        banner.Text = active ? GameContent.Course(lobby!.Course).Name : "Find a lobby";
        banner.Subtitle = active ? (lobby!.Host ? "LAN lobby   /   Hosting" : "LAN lobby   /   Guest") : "Race with drivers on your local network.";
        if (active && lobby!.EnhancedPhysics) banner.Subtitle += "   /   Enhanced physics";
        UpdateBannerImage(); banner.Invalidate();
        join.Enabled = !active && !busy && address.Text.Trim().Length > 0;
        trackLabel.Visible = track.Visible = more.Visible = start.Visible = active && lobby!.Host;
        track.Enabled = active && lobby!.Host && !racing;
        if (active && track.SelectedItem is GameCourse selected && selected.Id != lobby!.Course)
            track.SelectedItem = GameContent.Course(lobby.Course);
        ready.Enabled = connected && !racing;
        UpdateChoices(customContent && (!active || lobby!.CustomAllowed));
        if (racing && !lobby!.Host)
            car.SelectedIndex = car.FindStringExact(GameContent.DisplayCar(lobby.Local.Car));
        ready.Text = active && lobby!.Local.Ready ? "Unready" : "Ready";
        force.Text = active && lobby!.Host && !connected && !racing ? "Force Start (CPU)" : "Force Start";
        force.Enabled = active && lobby!.Host && !racing;
        start.Enabled = connected && force.Enabled && lobby!.AllReady;
        var unready = active ? lobby!.Drivers.Where(d => d is { Ready: false }).Select(d => d!.Name).ToArray() : [];
        waiting.Text = !active ? "" : racing ? "Race in progress" : !connected ? "Waiting for drivers"
            : unready.Length > 0 ? "Waiting for " + string.Join(", ", unready) : "All drivers ready";
        more.Enabled = force.Enabled;
        message.Enabled = send.Enabled = connected;
        if (active) RefreshDrivers(lobby!, racing);
    }

    private void RefreshDrivers(LanLobby session, bool racing)
    {
        var seats = session.Drivers;
        drivers.BeginUpdate();
        try
        {
            while (drivers.Items.Count > seats.Length) drivers.Items.RemoveAt(drivers.Items.Count - 1);
            for (var i = 0; i < seats.Length; ++i)
            {
                var driver = seats[i];
                string role = driver == null ? (i + 1).ToString() : i == 0 ? "Host" : "Guest";
                string state = driver == null ? (racing ? "Open to join" : "Open") : racing ? "Racing" : driver.Ready ? "Ready" : "Not ready";
                string[] cells = [role, driver?.Name ?? (racing ? "CPU" : "—"),
                    driver == null ? "—" : GameContent.DisplayCar(driver.Car).ToUpperInvariant(), state];
                if (i == drivers.Items.Count) drivers.Items.Add(new ListViewItem(cells));
                else
                {
                    var row = drivers.Items[i];
                    for (int col = 0; col < cells.Length; ++col)
                        if (row.SubItems[col].Text != cells[col]) row.SubItems[col].Text = cells[col];
                }
            }
            drivers.Height = (drivers.Font.Height + LogicalToDeviceUnits(6)) * (seats.Length + 1) + LogicalToDeviceUnits(8);
        }
        finally { drivers.EndUpdate(); }
    }

    private void Append(string text)
    {
        if (IsDisposed || Disposing) return;
        if (InvokeRequired) { BeginInvoke(() => Append(text)); return; }
        if (lobby is not { Closed: false }) discoveryError.Text = text;
        if (chat.Items.Count >= 200) chat.Items.RemoveAt(0);
        chat.Items.Add(text);
        chat.TopIndex = chat.Items.Count - 1;
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) { tabOpen = false; CancelScan(); hostMenu.Dispose(); lobby?.Dispose(); banner.CourseImage?.Dispose(); banner.CourseImage = null; }
        base.Dispose(disposing);
    }
    private static TableLayoutPanel PageLayout()
    {
        var panel = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, AutoScroll = true };
        panel.ColumnStyles.Add(new(SizeType.Percent, 100)); return panel;
    }
    private static Panel ChoiceSlot() => new() { Margin = Padding.Empty, Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink };
    private static Label Label(string text) => new() { Text = text, AutoSize = true, Anchor = AnchorStyles.Left };
    private static Button Button(string text) => new() { Text = text, AutoSize = true };
    private static ComboBox List(params string[] items)
    {
        var box = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 150 };
        box.Items.AddRange(items); if (items.Length > 0) box.SelectedIndex = 0;
        return box;
    }
    private static FlowLayoutPanel Row(params Control[] children)
    {
        var row = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true };
        row.Controls.AddRange(children);
        return row;
    }
}
