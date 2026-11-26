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
    private readonly Button refresh = Button("Scan / Refresh");
    private readonly Label scanStatus = Label("");
    private CancellationTokenSource? scan;
    private bool tabOpen;
    private readonly ComboBox track = List(), car = List("4WD", "Coupe", "F-Type");
    private readonly NumericUpDown capacity = new() { Minimum = 2, Maximum = LanLobby.MaxDrivers, Value = LanLobby.MaxDrivers, Width = 46, AccessibleName = "Lobby capacity" };
    private readonly ComboBox style = List("Classic", "N64 Mode", "Remastered");
    private readonly Button ready = Button("Ready"), start = Button("Start"), force = Button("Force Start");
    private readonly TextBox drivers = new() { ReadOnly = true, Multiline = true, Dock = DockStyle.Fill, Height = 90, AccessibleName = "Connected drivers" };
    private readonly ListBox chat = new() { Dock = DockStyle.Fill, IntegralHeight = false, AccessibleName = "Lobby chat" };
    private readonly TextBox message = new() { PlaceholderText = "Message the lobby", MaxLength = 256, Width = 300 };
    private readonly Button send = Button("Send");
    private readonly Func<LanGrid, RaceStyle, Process> launch;
    private LanLobby? lobby;
    private bool busy;
    private string Car => car.Text.ToLowerInvariant();

    internal MultiplayerPanel(Func<LanGrid, RaceStyle, Process> launch)
    {
        this.launch = launch;
        Dock = DockStyle.Fill;
        name.AccessibleName = "Driver name"; address.AccessibleName = "Host address";
        track.Items.AddRange(GameContent.LanCourseVersions.Keys.Select(GameContent.Course).ToArray()); track.SelectedIndex = 0;
        track.AccessibleName = "Track"; car.AccessibleName = "Your car"; style.AccessibleName = "Your visual style";
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 9, AutoScroll = true };
        layout.ColumnStyles.Add(new(SizeType.Percent, 100));
        foreach (var _ in Enumerable.Range(0, 7)) layout.RowStyles.Add(new(SizeType.AutoSize));
        layout.RowStyles.Add(new(SizeType.Percent, 100));
        layout.RowStyles.Add(new(SizeType.AutoSize));
        layout.Controls.Add(Row(Label("LAN lobbies"), servers, refresh, scanStatus));
        layout.Controls.Add(Row(Label("Driver"), name, Label("Seats"), capacity, host, Label("Host address"), address, join, leave));
        layout.Controls.Add(Row(Label("Track"), track, Label("Your car"), car, Label("Visual style"), style));
        layout.Controls.Add(Label("Connected drivers"));
        layout.Controls.Add(drivers);
        layout.Controls.Add(Row(ready, start, force));
        layout.Controls.Add(Label("Lobby chat"));
        layout.Controls.Add(chat);
        layout.Controls.Add(Row(message, send));
        Controls.Add(layout);
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

    internal void SetTabOpen(bool open)
    {
        if (tabOpen == open) return;
        tabOpen = open;
        if (open) _ = Scan();
        else CancelScan();
    }

    private void CancelScan()
    {
        scan?.Cancel();
        scan = null;
    }

    private async Task Scan()
    {
        if (scan != null) return;
        using var current = new CancellationTokenSource();
        scan = current;
        refresh.Enabled = false;
        scanStatus.Text = "Scanning…";
        try
        {
            var results = await LanDiscovery.Scan(current.Token);
            if (current.IsCancellationRequested) return;
            var previous = servers.SelectedItem as DiscoveredLobby;
            servers.Items.Clear();
            servers.Items.AddRange(results);
            if (previous != null && previous.Address.ToString() == address.Text)
            {
                servers.SelectedItem = results.FirstOrDefault(item => item.Address.Equals(previous.Address));
                if (servers.SelectedIndex < 0) address.Clear();
            }
            scanStatus.Text = results.Length switch { 0 => "No lobbies found", 1 => "1 lobby found", _ => $"{results.Length} lobbies found" };
        }
        catch (OperationCanceledException) when (current.IsCancellationRequested) { }
        catch (Exception error) when (error is SocketException or System.Net.NetworkInformation.NetworkInformationException)
        {
            if (!current.IsCancellationRequested) scanStatus.Text = "Scan failed. Join by IP or refresh.";
        }
        finally
        {
            if (scan == current)
            {
                scan = null;
                refresh.Enabled = true;
            }
        }
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
            lobby = new LanLobby(hosting, name.Text, Car, grid => launch(grid, (RaceStyle)style.SelectedIndex), (int)capacity.Value);
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
        bool active = lobby is { Closed: false }, connected = active && lobby!.Connected;
        bool racing = active && lobby!.Racing;
        capacity.Enabled = servers.Enabled = name.Enabled = address.Enabled = host.Enabled = join.Enabled = !active && !busy;
        if (active && lobby!.Connected) capacity.Value = lobby.Drivers.Length;
        leave.Enabled = active;
        style.Enabled = car.Enabled = !racing;
        track.Enabled = active && lobby!.Host && !racing;
        if (active && track.SelectedItem is GameCourse selected && selected.Id != lobby!.Course)
            track.SelectedItem = GameContent.Course(lobby.Course);
        ready.Enabled = connected && !racing;
        if (racing && !lobby!.Host) car.SelectedIndex = Array.FindIndex(new[] { "4wd", "coupe", "f-type" }, id => id == lobby.Local.Car);
        ready.Text = active && lobby!.Local.Ready ? "Unready" : "Ready";
        force.Text = active && lobby!.Host && !connected && !racing ? "Force Start (CPU)" : "Force Start";
        force.Enabled = active && lobby!.Host && !racing;
        start.Enabled = connected && force.Enabled && lobby!.AllReady;
        message.Enabled = send.Enabled = connected;
        string Driver(LanDriver d, bool owner) => $"{(owner ? "Host" : "Guest")}   {d.Name}   ·   {d.Car.ToUpperInvariant()}   ·   {(racing ? "Racing" : d.Ready ? "Ready" : "Not ready")}";
        drivers.Lines = !active ? [] : lobby!.Drivers.Select((d,i) => d != null ? Driver(d,i==0)
            : racing ? $"Seat {i+1}   ·   CPU (open to join)" : $"Seat {i+1}   ·   Open").ToArray();
    }

    private void Append(string text)
    {
        if (IsDisposed || Disposing) return;
        if (chat.Items.Count >= 200) chat.Items.RemoveAt(0);
        chat.Items.Add(text);
        chat.TopIndex = chat.Items.Count - 1;
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) { CancelScan(); lobby?.Dispose(); }
        base.Dispose(disposing);
    }
    private static Label Label(string text) => new() { Text = text, AutoSize = true, Anchor = AnchorStyles.Left };
    private static Button Button(string text) => new() { Text = text, AutoSize = true };
    private static ComboBox List(params string[] items)
    {
        var box = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 110 };
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
