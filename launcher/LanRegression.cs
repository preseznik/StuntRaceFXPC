namespace StuntRaceFX.Launcher;

// Each invocation is one independent launcher and one offscreen native game.
internal static class LanRegression
{
    internal static async Task<int> Run(string output, string capture, string role, int port, bool force, bool dropIn, string course, string? discoveryHost, string? discoveredAddress)
    {
        bool hosting = role switch { "host" => true, "guest" => false, _ => throw new ArgumentException("Unknown role.") };
        var style = hosting ? RaceStyle.Original : RaceStyle.Remastered;
        var finish = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
        var chat = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var acknowledged = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var lobby = new LanLobby(hosting, hosting ? discoveryHost ?? role : role, !hosting && dropIn ? "f-type" : "4wd", grid =>
        {
            if (grid.Course != course) throw new InvalidDataException($"Expected {course}, got {grid.Course}.");
            return BuildPipeline.Launch(output, width: 640, height: 360, style: style, course: grid.Course, lan: grid, lanCapture: capture, lanCaptureHandoff: dropIn);
        },capacity:2);
        lobby.Handoff += state => Console.WriteLine("HANDOFF " + state);
        lobby.Finished += result => finish.TrySetResult(result);
        lobby.Log += text =>
        {
            Console.WriteLine(text);
            if (text == "guest: On the grid") chat.TrySetResult();
            if (text == "guest: Back in lobby") acknowledged.TrySetResult();
            if (lobby.Closed) finish.TrySetException(new IOException(text));
        };
        using var limit = new CancellationTokenSource(TimeSpan.FromMinutes(4));
        async Task Until(Func<bool> predicate)
        {
            while (!predicate())
            {
                if (lobby.Closed) throw new IOException("Lobby closed during setup.");
                await Task.Delay(10, limit.Token);
            }
        }
        if (hosting)
        {
            if (dropIn) await lobby.SelectCourse(course);
            lobby.Listen(discoveryHost == null ? port : LanLobby.Port, System.Net.IPAddress.Loopback);
            if (dropIn)
            {
                await lobby.Start(true);
                if(course != "night-cruise") await Until(() => lobby.SharedLaps >= 1);
            }
            Console.WriteLine($"PORT {lobby.ListeningPort}");
            Console.Out.Flush();
        }
        else await lobby.Join(discoveredAddress ?? "127.0.0.1", port);
        await Until(() => lobby.Connected);
        if (!dropIn && hosting)
        {
            try { await lobby.Start(false); throw new InvalidDataException("Started without two ready players."); }
            catch (InvalidOperationException) { }
            if (course != GameContent.DefaultCourse)
            {
                await lobby.SetDriver("4wd", true);
                await Until(() => lobby.Peer?.Ready == true);
                await lobby.SelectCourse(course);
                if (lobby.Local.Ready)
                    throw new InvalidDataException("Changing course kept the host ready.");
            }
            await chat.Task.WaitAsync(limit.Token);
            await Until(() => lobby.Peer?.Car == "coupe" && (force || lobby.Peer.Ready));
            await lobby.SetDriver("4wd", true);
            await lobby.Start(force);
            using var late = new LanLobby(false, "late", "4wd", _ => throw new InvalidOperationException("Late entrant launched."));
            var busy = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            late.Log += line =>
            {
                if (line == "Race in progress.") busy.TrySetResult();
                else if (late.Closed) busy.TrySetException(new IOException(line));
            };
            await late.Join("127.0.0.1", lobby.ListeningPort);
            await busy.Task.WaitAsync(TimeSpan.FromSeconds(8));
            Console.WriteLine("Late entrant received race-in-progress status.");
        }
        else if (!dropIn)
        {
            if (course != GameContent.DefaultCourse)
            {
                await Until(() => lobby.Peer?.Ready == true);
                await lobby.SetDriver("4wd", true);
                await Until(() => lobby.Course == course);
                if (lobby.Local.Ready || lobby.Peer!.Ready)
                    throw new InvalidDataException("Guest did not reset readiness on course change.");
                Console.WriteLine("Guest confirmed the selected course and both ready states cleared.");
            }
            await lobby.SetDriver("coupe", false);
            if (!force) await lobby.SetDriver("coupe", true);
            await lobby.Chat("On the grid");
        }
        string result = await finish.Task.WaitAsync(limit.Token);
        if (lobby.Local.Ready || lobby.Peer?.Ready != false || lobby.Racing)
            throw new InvalidDataException("Race failed to return to an unready lobby.");
        Console.WriteLine($"{style} {result}");
        if (hosting) await acknowledged.Task.WaitAsync(limit.Token);
        else await lobby.Chat("Back in lobby");
        return 0;
    }
}
