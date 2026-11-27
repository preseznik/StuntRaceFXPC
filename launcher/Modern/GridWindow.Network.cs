using System.Net;
using System.Net.Sockets;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace StuntRaceFX.Launcher.Modern;

using KeyEventArgs = System.Windows.Input.KeyEventArgs;

internal partial class GridWindow
{
    private sealed record SeatRow(int Number, string Driver, string Car, string Status);
    private LanLobby? lobby;
    private CancellationTokenSource? search;
    private bool connecting, roomSync;
    private int lobbyPreviewGeneration;
    private async Task RefreshLobbyPreview()
    {
        int generation = ++lobbyPreviewGeneration;
        var course = lobby is { Closed: false } session ? session.Course : Servers.SelectedItem is DiscoveredLobby pick ? pick.Course : null;
        if (course == null || LanCar.SelectedItem is not Vehicle car) { NetworkPreview.Visibility = Visibility.Collapsed; return; }
        var photo = await pictures.Load(installed.Stage, GameContent.CaptureName(course, car.Id, (RaceStyle)LanStyle.SelectedIndex, installed.Version), 550);
        if (closed || generation != lobbyPreviewGeneration) return;
        LobbyPhoto.Source = photo;
        PreviewCaption.Text = GameContent.Course(course).Name + (photo == null ? " · Preview unavailable" : "");
        NetworkPreview.Visibility = Visibility.Visible;
    }

    private void HostActions(object sender, RoutedEventArgs e)
    {
        Force.ContextMenu.PlacementTarget = Force;
        Force.ContextMenu.Placement = System.Windows.Controls.Primitives.PlacementMode.Bottom;
        Force.ContextMenu.IsOpen = true;
    }

    private void WireLobby()
    {
        LanStyle.SelectionChanged += (_, _) => _ = RefreshLobbyPreview();
        Address.TextChanged += (_, _) => Join.IsEnabled = !connecting && !string.IsNullOrWhiteSpace(Address.Text);
        LanCar.SelectionChanged += async (_, _) =>
        {
            if (!roomSync) _ = RefreshLobbyPreview();
            if (!roomSync && lobby is { Closed: false, Racing: false } session && LanCar.SelectedItem is Vehicle car)
                await LobbyAction(() => session.SetDriver(car.Id, false));
        };
        LanCourse.SelectionChanged += async (_, _) =>
        {
            if (!roomSync && lobby is { Host: true, Closed: false, Racing: false } session && LanCourse.SelectedItem is GameCourse c)
                await LobbyAction(() => session.SelectCourse(c.Id));
        };
    }

    private async Task ScanAsync()
    {
        search?.Cancel();
        using var token = new CancellationTokenSource(); search = token;
        Scan.IsEnabled = false; ScanLabel.Text = "Searching the local network…";
        try
        {
            var found = await LanDiscovery.Scan(token.Token, settings.CustomContent);
            if (!token.IsCancellationRequested && !closed)
            {
                Servers.ItemsSource = found; Servers.Visibility = Show(found.Length > 0);
                ScanLabel.Text = found.Length == 0 ? "No lobbies found. Join by address or host a race." : $"{found.Length} available";
            }
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch (Exception ex) when (ex is SocketException or System.Net.NetworkInformation.NetworkInformationException) { if (!closed) ScanLabel.Text = ex.Message; }
        finally { if (search == token) { search = null; if (!closed) Scan.IsEnabled = true; } }
    }
    private async void ScanClick(object sender, RoutedEventArgs e) => await ScanAsync();
    private void ServerSelected(object sender, SelectionChangedEventArgs e) { if (Servers.SelectedItem is DiscoveredLobby found) Address.Text = found.Address.ToString(); _ = RefreshLobbyPreview(); }
    private async void HostClick(object sender, RoutedEventArgs e) => await Connect(true);
    private async void JoinClick(object sender, RoutedEventArgs e) => await Connect(false);
    private async Task Connect(bool host)
    {
        if (connecting || lobby is { Closed: false }) return;
        connecting = true; search?.Cancel(); Chat.Items.Clear();
        LanLobby? session = null;
        try
        {
            lobby?.Dispose();
            session = new LanLobby(host, DriverName.Text, ((Vehicle)LanCar.SelectedItem).Id, grid =>
            {
                var (snapshot, style) = Dispatcher.CheckAccess() ? LaunchSelection() : Dispatcher.Invoke(LaunchSelection);
                return RaceLaunch.Start(snapshot, grid, style);
            }, (int)Seats.SelectedItem, settings.CustomContent, settings.EnhancedPhysics, installed.Version >= GameContent.RacerPhysicsVersion);
            (LauncherSettings, RaceStyle) LaunchSelection() => (settings, (RaceStyle)LanStyle.SelectedIndex);
            lobby = session;
            session.Log += line => Dispatcher.InvokeAsync(() =>
            {
                if (closed || lobby != session) return;
                if (Chat.Items.Count >= 200) Chat.Items.RemoveAt(0);
                Chat.Items.Add(line); Chat.ScrollIntoView(line); if (session.Closed) Say(line);
            });
            session.Changed += () => Dispatcher.InvokeAsync(() => { if (!closed && lobby == session) UpdateRoom(); });
            UpdateRoom();
            if (host)
            {
                session.Listen();
                var addresses = await Dns.GetHostAddressesAsync(Dns.GetHostName());
                if (!closed && lobby == session) Chat.Items.Add("Host address   " + string.Join("  /  ", addresses.Where(a => a.AddressFamily == AddressFamily.InterNetwork)));
            }
            else await session.Join(Address.Text.Trim());
        }
        catch (Exception ex) { session?.Dispose(); if (!closed) Say(ex.Message); }
        finally { connecting = false; if (!closed) UpdateRoom(); }
    }
    private void UpdateRoom()
    {
        var session = lobby;
        bool active = session is { Closed: false }, host = active && session!.Host, racing = active && session!.Racing;
        roomSync = true;
        try
        {
            bool custom = settings.CustomContent && (!active || session!.CustomAllowed);
            var cars = Cars(custom);
            string selected = active ? session!.Local.Car : (LanCar.SelectedItem as Vehicle)?.Id ?? "4wd";
            if (LanCar.Items.Count != cars.Length) LanCar.ItemsSource = cars;
            LanCar.SelectedValuePath = nameof(Vehicle.Id); LanCar.SelectedValue = selected;
            if (LanCar.SelectedIndex < 0) LanCar.SelectedIndex = 0;
            if (LanCourse.Items.Count != Courses(custom).Length) LanCourse.ItemsSource = Courses(custom);
            if (active) LanCourse.SelectedItem = GameContent.Course(session!.Course);
            NetworkTitle.Text = active ? GameContent.Course(session!.Course).Name + (session.EnhancedPhysics ? " · Enhanced physics" : "") : "Find a lobby";
            FindCard.Visibility = HostCard.Visibility = Show(!active); ChatCard.Visibility = RosterCard.Visibility = Show(active);
            HostCourse.Visibility = Go.Visibility = Force.Visibility = Show(host);
            Host.IsEnabled = !connecting; Join.IsEnabled = !connecting && Address.Text.Trim().Length > 0;
            DriverName.IsEnabled = !active && !connecting; LanCar.IsEnabled = LanStyle.IsEnabled = !racing;
            LanCourse.IsEnabled = !racing;
            _ = RefreshLobbyPreview();
            if (!active) return;
            var seats = session!.Drivers.ToArray();
            Roster.ItemsSource = seats.Select((d, i) => d == null
                ? new SeatRow(i + 1, racing ? "CPU driver" : "Open seat", "", racing ? "Joinable" : "")
                : new SeatRow(i + 1, d.Name + (i == 0 ? "  ·  Host" : ""), GameContent.DisplayCar(d.Car).ToUpperInvariant(),
                    racing ? "Racing" : d.Ready ? "Ready" : "Not ready")).ToArray();
            Ready.Content = session.Local.Ready ? "Unready" : "Ready"; Ready.IsEnabled = session.Connected && !racing;
            Go.IsEnabled = session.Connected && session.AllReady && !racing; Force.IsEnabled = !racing;
            Message.IsEnabled = session.Connected;
            Waiting.Text = racing ? "Race in progress" : !session.Connected ? "Waiting for drivers" : session.AllReady ? "All drivers ready" :
                "Waiting for " + string.Join(", ", seats.Where(d => d is { Ready: false }).Select(d => d!.Name));
        }
        finally { roomSync = false; }
    }
    private async Task LobbyAction(Func<Task> action)
    {
        try { await action(); }
        catch (Exception ex) { if (!closed) Say(ex.Message); }
        finally { if (!closed) UpdateRoom(); }
    }
    private async void ReadyClick(object sender, RoutedEventArgs e) { if (lobby is { } session) await LobbyAction(() => session.SetDriver(session.Local.Car, !session.Local.Ready)); }
    private async void GoClick(object sender, RoutedEventArgs e) { if (lobby is { } session) await LobbyAction(() => session.Start(false)); }
    private async void ForceClick(object sender, RoutedEventArgs e) { if (lobby is { } session) await LobbyAction(() => session.Start(true)); }
    private void LeaveClick(object sender, RoutedEventArgs e) { lobby?.Dispose(); lobby = null; UpdateRoom(); _ = ScanAsync(); }
    private async Task Send()
    {
        if (lobby is not { Connected: true } session || string.IsNullOrWhiteSpace(Message.Text)) return;
        var text = Message.Text; Message.Clear(); await LobbyAction(() => session.Chat(text));
    }
    private async void SendClick(object sender, RoutedEventArgs e) => await Send();
    private async void ChatKey(object sender, KeyEventArgs e) { if (e.Key == Key.Enter) { e.Handled = true; await Send(); } }
}

