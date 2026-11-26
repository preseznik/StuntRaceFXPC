using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;

namespace StuntRaceFX.Launcher;

internal sealed record DiscoveredLobby(IPAddress Address, string Host, string Course, bool Full, bool Racing, int Players, int Capacity)
{
    public override string ToString() =>
        $"{Host}   ·   {GameContent.Course(Course).Name}   ·   {(Full ? "Full" : Racing ? "Join in progress" : "Open")}   ·   {Address}";
}

internal static class LanDiscovery
{
    private static readonly byte[] Query = Encoding.ASCII.GetBytes($"{LanLobby.Protocol} DISCOVER");
    private sealed record Reply(string Protocol, Guid Id, string Host, string Course, bool Full, bool Racing, int Players, int Capacity);

    internal static async Task Advertise(LanLobby lobby, UdpClient udp, CancellationToken token, Action<string> warn)
    {
        try
        {
            var id = Guid.NewGuid();
            while (!token.IsCancellationRequested)
            {
                try
                {
                    var request = await udp.ReceiveAsync(token);
                    if (!request.Buffer.AsSpan().SequenceEqual(Query)) continue;
                    var reply = new Reply(LanLobby.Protocol, id, lobby.Local.Name, lobby.Course, !lobby.CanAcceptGuest, lobby.Racing, lobby.Drivers.Count(d => d != null), lobby.Drivers.Length);
                    await udp.SendAsync(JsonSerializer.SerializeToUtf8Bytes(reply), request.RemoteEndPoint, token);
                }
                catch (SocketException e) when (e.SocketErrorCode == SocketError.ConnectionReset) { }
            }
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch (ObjectDisposedException) when (token.IsCancellationRequested) { }
        catch (SocketException e)
        {
            if (!token.IsCancellationRequested) warn($"Lobby discovery is unavailable. Players can still join by IP. {e.Message}");
        }
    }

    private static IEnumerable<IPAddress> Broadcasts()
    {
        foreach (var adapter in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (adapter.OperationalStatus != OperationalStatus.Up || adapter.NetworkInterfaceType is
                NetworkInterfaceType.Loopback or NetworkInterfaceType.Tunnel or NetworkInterfaceType.Ppp) continue;
            foreach (var ip in adapter.GetIPProperties().UnicastAddresses)
            {
                if (ip.Address.AddressFamily != AddressFamily.InterNetwork || ip.PrefixLength is < 1 or > 30) continue;
                var bytes = ip.Address.GetAddressBytes();
                var mask = ip.IPv4Mask.GetAddressBytes();
                for (int i = 0; i < bytes.Length; i++) bytes[i] |= (byte)~mask[i];
                yield return new IPAddress(bytes);
            }
        }
    }

    internal static async Task<DiscoveredLobby[]> Scan(CancellationToken token)
    {
        using var udp = new UdpClient(0) { EnableBroadcast = true };
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(token);
        deadline.CancelAfter(TimeSpan.FromMilliseconds(1500));
        var found = new Dictionary<Guid, DiscoveredLobby>();
        try
        {
            foreach (var address in Broadcasts().Append(IPAddress.Broadcast).Append(IPAddress.Loopback).Distinct())
            {
                try { await udp.SendAsync(Query, new IPEndPoint(address, LanLobby.Port), deadline.Token); }
                catch (SocketException) when (!IPAddress.IsLoopback(address)) { } // Another adapter may still be reachable.
            }
            while (true)
            {
                deadline.Token.ThrowIfCancellationRequested();
                UdpReceiveResult packet;
                try { packet = await udp.ReceiveAsync(deadline.Token); }
                catch (SocketException e) when (e.SocketErrorCode == SocketError.ConnectionReset) { continue; }
                if (packet.RemoteEndPoint.Port != LanLobby.Port || packet.Buffer.Length > 1024) continue;
                Reply? reply;
                try { reply = JsonSerializer.Deserialize<Reply>(packet.Buffer); }
                catch (JsonException) { continue; }
                if (reply?.Protocol != LanLobby.Protocol || reply.Id == Guid.Empty || string.IsNullOrWhiteSpace(reply.Host)
                    || reply.Host.Length > 32 || reply.Host.Any(char.IsControl) || reply.Course == null
                    || reply.Capacity is < 2 or > LanLobby.MaxDrivers || reply.Players < 1 || reply.Players > reply.Capacity
                    || !GameContent.LanCourseVersions.ContainsKey(reply.Course)) continue;
                var ip = packet.RemoteEndPoint.Address;
                if (!found.TryGetValue(reply.Id, out var existing) || IPAddress.IsLoopback(existing.Address))
                    found[reply.Id] = new(ip, reply.Host, reply.Course, reply.Full, reply.Racing, reply.Players, reply.Capacity);
            }
        }
        catch (OperationCanceledException) when (!token.IsCancellationRequested) { }
        return found.Values.OrderBy(lobby => lobby.Host, StringComparer.OrdinalIgnoreCase).ThenBy(lobby => lobby.Address.ToString()).ToArray();
    }
}
