using System.Buffers.Binary;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text.Json;
using System.Threading.Channels;

namespace StuntRaceFX.Launcher;

internal sealed record LanDriver(string Name, string Car, bool Ready = false);
internal sealed record LanGrid(string Course, string[] Cars, int Seat, bool DropIn = false, int Round = 0);
internal sealed record LanMessage(string Kind, string Text = "", LanDriver? Driver = null, string? Course = null,
    int Round = 0, int Seat = 0, LanDriver?[]? Roster = null);

internal sealed class LanLobby : IDisposable
{
    internal const int Port = 48276, MaxDrivers = 4;
    internal const string Protocol = "SRF-LAN/5";
    internal static readonly string[] Cars = ["4wd", "coupe", "f-type"];
    private const int MaxReplayTicks = 1_000_000;
    private sealed class Connection(TcpClient socket)
    {
        internal readonly TcpClient Socket = socket;
        internal readonly SemaphoreSlim Writer = new(1);
        internal bool Joined, Live;
        internal Channel<string> Inbox = NewInbox();
        internal static Channel<string> NewInbox() => Channel.CreateBounded<string>(4);
    }

    private readonly Func<LanGrid, Process> launch;
    private readonly CancellationTokenSource lifetime = new();
    private readonly Connection?[] links = new Connection?[MaxDrivers];
    private TcpListener? listener;
    private UdpClient? discovery;
    private bool disposed, acceptingTakeover;
    private volatile LanGrid? grid;
    private int nextRound, seat;
    private CancellationTokenSource? raceCancellation;
    internal int SharedLaps { get; private set; }
    internal event Action<string>? Handoff;
    internal bool Host { get; }
    internal string Course { get; private set; } = GameContent.DefaultCourse;
    internal bool Racing => grid != null;
    internal bool Connected => Peer != null;
    internal bool CanAcceptGuest => links.Skip(1).Take(Drivers.Length-1).Any(p => p == null) && (!Racing || acceptingTakeover);
    internal bool Closed => disposed;
    internal LanDriver Local { get; private set; }
    internal LanDriver?[] Drivers { get; private set; }
    internal LanDriver? Peer => Drivers.Where((d,i) => i != seat && d != null).FirstOrDefault();
    internal bool AllReady => Connected && Drivers.All(d => d == null || d.Ready);
    internal event Action? Changed;
    internal event Action<string>? Log;
    internal event Action<string>? Finished;
    internal int ListeningPort => ((IPEndPoint)listener!.LocalEndpoint).Port;

    internal LanLobby(bool host, string name, string car, Func<LanGrid, Process> launch, int capacity = MaxDrivers)
    {
        if (capacity is < 2 or > MaxDrivers) throw new ArgumentOutOfRangeException(nameof(capacity));
        Host = host;
        seat = host ? 0 : -1;
        Local = new(name.Trim(), car);
        Validate(Local);
        Drivers = new LanDriver?[capacity];
        if (Host) Drivers[0] = Local;
        this.launch = launch;
    }

    internal void Listen(int port = Port, IPAddress? bind = null)
    {
        listener = new TcpListener(bind ?? IPAddress.Any, port);
        listener.Start();
        _ = Accept();
        try
        {
            discovery = new UdpClient((IPEndPoint)listener.LocalEndpoint);
            _ = LanDiscovery.Advertise(this, discovery, lifetime.Token, text => Log?.Invoke(text));
        }
        catch (SocketException e) { Log?.Invoke($"Lobby discovery is unavailable. Players can still join by IP. {e.Message}"); }
    }

    private async Task Reject(TcpClient next)
    {
        using (next)
        using (var timeout = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token))
        {
            timeout.CancelAfter(TimeSpan.FromSeconds(3));
            try
            {
                var stream = next.GetStream();
                await ReadPacket(stream, timeout);
                await WritePacket(stream, new("busy", Racing ? "Race in progress." : "Lobby is full."), timeout.Token);
            }
            catch (Exception e) when (e is IOException or OperationCanceledException or JsonException or InvalidDataException or ObjectDisposedException) { }
        }
    }

    private async Task Accept()
    {
        try
        {
            while (!disposed)
            {
                var next = await listener!.AcceptTcpClientAsync(lifetime.Token);
                int index = Array.FindIndex(links, 1, Drivers.Length-1, p => p == null);
                if (index < 0 || (Racing && !acceptingTakeover)) { _ = Reject(next); continue; }
                next.NoDelay = true;
                var connection = links[index] = new(next);
                _ = Receive(index, connection);
            }
        }
        catch (Exception e) { Close(e.Message); }
    }

    internal async Task Join(string address, int port = Port)
    {
        var connection = links[0] = new(new TcpClient { NoDelay = true });
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
        timeout.CancelAfter(TimeSpan.FromSeconds(8));
        await connection.Socket.ConnectAsync(address.Trim(), port, timeout.Token);
        await Send(connection, new("join", Protocol, Local));
        _ = Receive(0, connection);
    }

    private static void Validate(LanDriver? d)
    {
        if (d == null || string.IsNullOrWhiteSpace(d.Name) || d.Name.Length > 32 ||
            d.Name.Any(char.IsControl) || !Cars.Contains(d.Car)) throw new InvalidDataException("Invalid LAN driver.");
    }

    private Task Send(LanMessage message) => Send(links[0] ?? throw new IOException("Host disconnected."), message);
    private async Task Send(Connection link, LanMessage message)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
        timeout.CancelAfter(TimeSpan.FromSeconds(10));
        await link.Writer.WaitAsync(timeout.Token);
        try { await WritePacket(link.Socket.GetStream(), message, timeout.Token); }
        finally { link.Writer.Release(); }
    }
    private async Task SendGuest(int index, Connection link, LanMessage message)
    {
        try { await Send(link,message); }
        catch (Exception e) when (e is IOException or ObjectDisposedException or SocketException or OperationCanceledException) { DropPeer(index,link,e.Message); }
    }
    private Task Broadcast(LanMessage message) => Task.WhenAll(links.Select((p,i) =>
        p is { Joined: true } ? SendGuest(i,p,message) : Task.CompletedTask));
    private Task PublishRoster() => Broadcast(new("roster", Course: Course, Roster: [.. Drivers]));

    private static async Task WritePacket(NetworkStream stream, LanMessage message, CancellationToken token)
    {
        byte[] data = JsonSerializer.SerializeToUtf8Bytes(message);
        byte[] packet = new byte[4 + data.Length];
        BinaryPrimitives.WriteInt32BigEndian(packet, data.Length);
        data.CopyTo(packet, 4);
        await stream.WriteAsync(packet, token);
    }

    private static async Task<LanMessage> ReadPacket(NetworkStream stream, CancellationTokenSource timeout)
    {
        byte[] header = new byte[4];
        await stream.ReadExactlyAsync(header, timeout.Token);
        int size = BinaryPrimitives.ReadInt32BigEndian(header);
        if (size is < 1 or > 4096) throw new InvalidDataException("Invalid LAN packet length.");
        byte[] payload = new byte[size];
        timeout.CancelAfter(TimeSpan.FromSeconds(10));
        await stream.ReadExactlyAsync(payload, timeout.Token);
        return JsonSerializer.Deserialize<LanMessage>(payload) ?? throw new InvalidDataException("Empty LAN message.");
    }


    private async Task Receive(int index, Connection link)
    {
        try
        {
            var stream = link.Socket.GetStream();
            while (!disposed)
            {
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
                if (!link.Joined) timeout.CancelAfter(TimeSpan.FromSeconds(10));
                var msg = await ReadPacket(stream,timeout);
                if (!link.Joined)
                {
                    if (!Host && msg.Kind == "busy") throw new IOException(msg.Text == "Race in progress." ? msg.Text : "Lobby is full.");
                    if (msg.Kind != (Host ? "join" : "welcome") || msg.Text != Protocol)
                        throw new InvalidDataException("Incompatible LAN protocol. Use the same launcher build.");
                    if (Host)
                    {
                        Validate(msg.Driver);
                        var active = grid;
                        Drivers[index] = msg.Driver! with { Ready = false, Car = active?.Cars[index] ?? msg.Driver.Car };
                        var welcome = Send(link,new("welcome",Protocol,Course:Course,Seat:index,Roster:[.. Drivers]));
                        link.Joined = true;
                        var start = active == null ? Task.CompletedTask : Send(link,StartMessage(active,true));
                        await welcome;
                        await start;
                        await PublishRoster();
                    }
                    else
                    {
                        seat = msg.Seat;
                        ReadRoster(msg);
                        link.Joined = true;
                    }
                    Log?.Invoke($"{Drivers[index]?.Name} connected.");
                }
                else switch (msg.Kind)
                {
                    case "roster" when !Host:
                        ReadRoster(msg);
                        break;
                    case "driver" when Host:
                        if (Racing) break;
                        Validate(msg.Driver);
                        Drivers[index] = msg.Driver! with { Ready = msg.Course == Course && msg.Driver.Ready };
                        await PublishRoster();
                        break;
                    case "chat" when msg.Text is { Length: > 0 and <= 256 } && !msg.Text.Any(char.IsControl):
                        int from = Host ? index : msg.Seat;
                        if (from < 0 || from >= Drivers.Length || Drivers[from] == null) throw new InvalidDataException("Invalid chat sender.");
                        Log?.Invoke($"{Drivers[from]!.Name}: {msg.Text}");
                        if (Host) await Broadcast(new("chat",msg.Text,Seat:from));
                        break;
                    case "start" or "dropin" when !Host && !Racing:
                        var cars = msg.Text.Split(' ');
                        if (msg.Course != Course || cars.Length != Drivers.Length || cars.Any(c => !Cars.Contains(c)))
                            throw new InvalidDataException("Invalid starting grid.");
                        Drivers[seat] = Local = Local with { Car = cars[seat] };
                        if (msg.Kind == "dropin") Log?.Invoke($"Joining {GameContent.Course(Course).Name} in the {Local.Car}.");
                        Begin(new(Course,cars,seat,msg.Kind == "dropin",msg.Round));
                        break;
                    case "race" or "abandon" or "ended" when grid == null || msg.Round != grid.Round:
                        continue;
                    case "abandon":
                        var active = grid!;
                        if (Host) await AbandonRace(active,$"{Drivers[index]?.Name} ended the race. Back in lobby.");
                        else { ResetRace(); Log?.Invoke("Race ended. Back in lobby."); }
                        break;
                    case "race":
                        if (msg.Text == null || msg.Text.Length > (!Host && msg.Text.StartsWith("HISTORY ") ? 3200 : 180) ||
                            msg.Text.Any(char.IsControl)) throw new InvalidDataException("Invalid race stream.");
                        if (Host)
                        {
                            if (!link.Inbox.Writer.TryWrite(msg.Text)) throw new InvalidDataException("Excess guest input.");
                        }
                        else await link.Inbox.Writer.WriteAsync(msg.Text,lifetime.Token);
                        continue;
                    case "ended" when !Host:
                        ResetRace();
                        Log?.Invoke("The race finished before the handoff.");
                        break;
                    default: throw new InvalidDataException("Unexpected lobby message.");
                }
                Changed?.Invoke();
            }
        }
        catch (Exception e) { DropPeer(index,link,e is EndOfStreamException ? "Peer left the lobby." : e.Message); }
    }

    private void ReadRoster(LanMessage msg)
    {
        if (msg.Roster is not { Length: >= 2 and <= MaxDrivers } roster || seat <= 0 || seat >= roster.Length ||
            roster[0] == null || roster[seat] == null) throw new InvalidDataException("Invalid lobby seats.");
        foreach (var d in roster) if (d != null) Validate(d);
        Course = ValidCourse(msg.Course);
        Drivers = roster;
        Local = roster[seat]!;
    }
    private static string ValidCourse(string? course) => course != null && GameContent.LanCourseVersions.ContainsKey(course)
        ? course : throw new InvalidDataException("Unsupported LAN course.");

    internal async Task SetDriver(string car, bool ready)
    {
        if (Racing) return;
        var updated = Local with { Car = car, Ready = car == Local.Car && ready };
        Validate(updated);
        Local = updated;
        if (seat >= 0) Drivers[seat] = updated;
        if (Host) await PublishRoster();
        else if (Connected) await Send(new("driver",Driver:updated,Course:Course));
        Changed?.Invoke();
    }

    internal async Task Chat(string text)
    {
        text = text.Trim();
        if (!Connected || text.Length is 0 or > 256 || text.Any(char.IsControl)) return;
        if (Host) { Log?.Invoke($"{Local.Name}: {text}"); await Broadcast(new("chat",text)); }
        else await Send(new("chat",text));
    }

    internal async Task SelectCourse(string course)
    {
        if (!Host || Racing || course == Course) return;
        Course = ValidCourse(course);
        ClearReady();
        await PublishRoster();
        Changed?.Invoke();
    }

    private static LanMessage StartMessage(LanGrid active,bool drop) =>
        new(drop ? "dropin" : "start", string.Join(" ",active.Cars),Course:active.Course,Round:active.Round);

    internal async Task Start(bool force)
    {
        if (!Host || Racing || (!force && !AllReady)) throw new InvalidOperationException("Ready all connected drivers or use Force Start.");
        var cars = Drivers.Select((d,i) => d?.Car ?? Cars[i % Cars.Length]).ToArray();
        var active = new LanGrid(Course,cars,0,Round:++nextRound);
        var announcing = Broadcast(StartMessage(active,false));
        Begin(active);
        await announcing;
    }

    private void Begin(LanGrid active)
    {
        grid = active;
        acceptingTakeover = true;
        SharedLaps = 0;
        foreach (var p in links) if (p != null) p.Live = p.Joined;
        Changed?.Invoke();
        _ = Drive(active);
    }

    private sealed record DriverInput(string Controls, string[] Cpu, int[] Laps, string State);
    private static DriverInput ParseInput(string value,int tick,int count)
    {
        var p = value.Split(' ');
        if (p.Length != 4+3*count || p[0] != "INPUT" || !int.TryParse(p[1],out var seq) || seq != tick ||
            !ushort.TryParse(p[2],out _) || !int.TryParse(p[3],out var axis) || axis is < -32768 or > 32768)
            throw new InvalidDataException("Invalid driver input.");
        var cpu = new string[count];
        var laps = new int[count];
        int state = 3+count;
        for (int i=1;i<count;i++)
        {
            if (!ushort.TryParse(p[3+i],out var buttons)) throw new InvalidDataException("Invalid CPU input.");
            cpu[i] = $"{buttons} 32768";
        }
        for (int i=0;i<count;i++)
            if (!ushort.TryParse(p[state+2*i],out _) || !int.TryParse(p[state+2*i+1],out laps[i]) || laps[i] is < 0 or > 3)
                throw new InvalidDataException("Invalid vehicle state.");
        if (!uint.TryParse(p[^1],out _)) throw new InvalidDataException("Invalid state digest.");
        return new($"{p[2]} {axis}",cpu,laps,string.Join(" ",p[state..]));
    }
    private async Task<string> HostFrame(LanGrid active,List<string> history,int tick,DriverInput input,CancellationToken token)
    {
        var controls = input.Cpu;
        controls[0] = input.Controls;
        var participants = new List<(int Seat,Connection Link,bool Takeover)>();
        for (int i=1;i<controls.Length;i++)
        {
            var p = links[i];
            if (p is not { Joined: true }) continue;
            var inbox = p.Inbox.Reader;
            using var wait = CancellationTokenSource.CreateLinkedTokenSource(token);
            wait.CancelAfter(TimeSpan.FromSeconds(30));
            try
            {
                bool takeover = !p.Live;
                if (takeover)
                {
                    if (!inbox.TryRead(out var loaded)) continue;
                    if (loaded != "LOADED" || history.Count != tick) throw new InvalidDataException("Invalid takeover request.");
                    for (int j=0;j<history.Count;j+=32)
                    {
                        var batch = string.Join("|",history.GetRange(j,Math.Min(32,history.Count-j)));
                        await Send(p,new("race","HISTORY "+batch,Round:active.Round)).WaitAsync(wait.Token);
                    }
                    await Send(p,new("race",$"SYNC {tick}",Round:active.Round)).WaitAsync(wait.Token);
                }
                var guest = ParseInput(await inbox.ReadAsync(wait.Token),tick,controls.Length);
                if (guest.State != input.State) throw new InvalidDataException($"Vehicle states disagree at tick {tick}.");
                controls[i] = guest.Controls;
                participants.Add((i,p,takeover));
            }
            catch (Exception e) when (e is IOException or ChannelClosedException or InvalidDataException or OperationCanceledException or ObjectDisposedException)
            {
                token.ThrowIfCancellationRequested();
                DropPeer(i,p,e.Message);
            }
        }
        var body = $"{tick} {string.Join(" ",controls)}";
        foreach (var (index,link,takeover) in participants)
        {
            await SendGuest(index,link,new("race",(takeover ? "TAKE " : "FRAME ")+body,Round:active.Round));
            if (takeover && ReferenceEquals(links[index],link))
            {
                link.Live = true;
                Handoff?.Invoke($"{tick} {input.State}");
                Log?.Invoke($"{Drivers[index]?.Name} took over the {active.Cars[index]}.");
            }
        }
        if (history.Count < MaxReplayTicks) history.Add(body);
        else acceptingTakeover = false;
        return (participants.Any(driver => driver.Takeover) ? "TAKE " : "FRAME ")+body;
    }

    private async Task Drive(LanGrid active)
    {
        Process? child = null;
        var history = new List<string>();
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
        raceCancellation = timeout;
        try
        {
            child = launch(active);
            var remote = Host ? null : links[0]!.Inbox.Reader;
            int tick = active.DropIn ? await RestoreGuest(child,remote!,timeout,active.Round) : 0;
            int count = active.Cars.Length;
            string result;
            for (;;tick++)
            {
                timeout.CancelAfter(TimeSpan.FromSeconds(45));
                var line = await child.StandardOutput.ReadLineAsync(timeout.Token) ?? throw new IOException("Native race exited before the finish.");
                if (tick == 0 && line.StartsWith("Remastered MSAA samples: "))
                    line = await child.StandardOutput.ReadLineAsync(timeout.Token) ?? throw new IOException("Renderer exited.");
                if (line == "QUIT") { await AbandonRace(active,"Race ended. Back in lobby."); return; }
                if (line.StartsWith("FINISH "))
                {
                    var parts = line.Split(' ');
                    if (parts.Length != count+3 || !int.TryParse(parts[1],out var end) || end != tick ||
                        !int.TryParse(parts[2],out var winner) || winner < 0 || winner > count ||
                        parts.Skip(3).Any(s => !int.TryParse(s,out var lap) || lap is < 0 or > 3))
                        throw new InvalidDataException("Invalid finish.");
                    result = line;
                    break;
                }
                var input = ParseInput(line,tick,count);
                string frame;
                if (Host)
                {
                    SharedLaps = input.Laps.Min();
                    frame = await HostFrame(active,history,tick,input,timeout.Token);
                }
                else
                {
                    await Send(new("race",line,Round:active.Round));
                    frame = await remote!.ReadAsync(timeout.Token);
                    var parts = frame.Split(' ');
                    if (parts.Length != 2+count*2 || parts[0] is not ("FRAME" or "TAKE") || parts[1] != tick.ToString() ||
                        parts[2+2*seat]+" "+parts[3+2*seat] != input.Controls) throw new InvalidDataException("Invalid host frame.");
                    if (parts[0] == "TAKE") Handoff?.Invoke($"{tick} {input.State}");
                }
                await child.StandardInput.WriteLineAsync(frame.AsMemory(),timeout.Token);
            }
            acceptingTakeover = false;
            await child.WaitForExitAsync(timeout.Token);
            if (child.ExitCode != 0) throw new IOException("Native race failed.");
            if (Host)
            {
                // Collect every result before acknowledging the round to any driver.
                foreach (var p in links.ToArray()) if (p is { Live: true })
                {
                    try { if (await p.Inbox.Reader.ReadAsync(timeout.Token) != result) throw new InvalidDataException("Race results disagree."); }
                    catch (ChannelClosedException) when (!links.Contains(p)) { }
                }
                await Task.WhenAll(links.Select((p,i) => p is { Joined: true }
                    ? SendGuest(i,p,new(p.Live ? "race" : "ended",result,Round:active.Round)) : Task.CompletedTask));
            }
            else
            {
                await Send(new("race",result,Round:active.Round));
                if (await remote!.ReadAsync(timeout.Token) != result) throw new InvalidDataException("Race results disagree.");
            }
            if (!ReferenceEquals(grid,active)) return;
            ResetRace();
            var finish = result.Split(' ');
            int winningSeat = int.Parse(finish[2]);
            var name = GameContent.Course(active.Course).Name;
            Log?.Invoke(winningSeat == count ? $"{name} finished in a draw." : $"{Drivers[winningSeat]?.Name ?? "CPU"} wins {name}.");
            Log?.Invoke("Laps at finish — "+string.Join(", ",finish.Skip(3).Select((laps,i) => $"{Drivers[i]?.Name ?? "CPU"} {laps}/3"))+".");
            Finished?.Invoke(result);
            Changed?.Invoke();
        }
        catch (Exception e) { await AbandonRace(active,$"Race interrupted: {e.Message}"); }
        finally
        {
            if (ReferenceEquals(raceCancellation,timeout)) raceCancellation = null;
            if (child != null)
            {
                try { if (!child.HasExited) child.Kill(); } catch (InvalidOperationException) { }
                child.Dispose();
            }
        }
    }
    private async Task<int> RestoreGuest(Process child, ChannelReader<string> remote, CancellationTokenSource timeout, int round)
    {
        timeout.CancelAfter(TimeSpan.FromSeconds(45));
        string? loaded = await child.StandardOutput.ReadLineAsync(timeout.Token);
        if (loaded?.StartsWith("Remastered MSAA samples: ") == true)
            loaded = await child.StandardOutput.ReadLineAsync(timeout.Token);
        if (loaded != "LOADED") throw new IOException("Guest could not load the race.");
        await Send(new("race", "LOADED", Round: round));
        int tick = 0;
        while (true)
        {
            var message = await remote.ReadAsync(timeout.Token);
            if (message == $"SYNC {tick}")
            {
                await child.StandardInput.WriteLineAsync(message.AsMemory(), timeout.Token);
                return tick;
            }
            if (!message.StartsWith("HISTORY ", StringComparison.Ordinal)) throw new InvalidDataException("Expected race history.");
            foreach (var frame in message[8..].Split('|'))
            {
                if (tick >= MaxReplayTicks || !frame.StartsWith($"{tick} ", StringComparison.Ordinal))
                    throw new InvalidDataException("Out-of-order race history.");
                await child.StandardInput.WriteLineAsync(("FRAME " + frame).AsMemory(), timeout.Token);
                tick++;
            }
        }
    }


    private void ClearReady()
    {
        for (int i=0;i<Drivers.Length;i++) if (Drivers[i] is {} d) Drivers[i]=d with { Ready=false };
        if (seat>=0 && Drivers[seat] is {} local) Local=local;
    }

    private void ResetRace()
    {
        grid = null;
        raceCancellation?.Cancel();
        foreach (var p in links) if (p != null)
        {
            p.Live = false;
            p.Inbox.Writer.TryComplete();
            p.Inbox = Connection.NewInbox();
        }
        ClearReady();
    }

    private async Task AbandonRace(LanGrid active,string reason)
    {
        if (disposed || !ReferenceEquals(grid,active)) return;
        var msg = new LanMessage("abandon",Round:active.Round);
        var notify = Host ? Broadcast(msg) : Send(msg);
        ResetRace();
        Log?.Invoke(reason);
        Changed?.Invoke();
        try { await notify; } catch (Exception e) when (!Host) { Close(e.Message); }
    }

    private void DropPeer(int index,Connection link,string reason)
    {
        if (disposed || !ReferenceEquals(links[index],link)) return;
        if (!Host) { Close(reason); return; }
        Drivers[index] = null;
        links[index] = null;
        link.Socket.Dispose();
        link.Inbox.Writer.TryComplete();
        if (Racing) Log?.Invoke($"CPU driving the {grid!.Cars[index]}. The seat is open.");
        _ = PublishRoster();
        Log?.Invoke(reason);
        Changed?.Invoke();
    }

    internal void Close(string reason)
    {
        if (disposed) return;
        disposed = true;
        lifetime.Cancel();
        listener?.Stop();
        discovery?.Dispose();
        raceCancellation?.Cancel();
        foreach (var p in links) if (p != null) { p.Socket.Dispose(); p.Inbox.Writer.TryComplete(); }
        grid = null;
        Array.Clear(Drivers);
        Log?.Invoke(reason);
        Changed?.Invoke();
    }
    public void Dispose() => Close("Left the lobby.");
}
