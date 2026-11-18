using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Text;

namespace Mtdrop.Protocol;

/// <summary>An AirDrop receiver as far as DNS-SD tells: where to connect. Its display name comes from /Discover.</summary>
/// <param name="ServiceId">First label of the instance name (12 hex characters on Apple devices).</param>
public sealed record AirDropServiceEndpoint(string Instance, string ServiceId, IPEndPoint EndPoint, string? Txt);

/// <summary>
/// Collects DNS-SD records for <c>_airdrop._tcp.local</c> from mDNS responses and turns complete sets
/// (PTR → SRV → AAAA) into endpoints. Pure parsing; the socket lives in <see cref="MdnsBrowser"/>.
/// The iPhone's records reach <c>awdl0</c> through awdld's service bridge, which rewrites the service
/// response TLVs of its AWDL action frames into exactly these records (ADR-0005).
/// </summary>
public sealed class AirDropServiceCache
{
    private const ushort TypePtr = 12, TypeTxt = 16, TypeAaaa = 28, TypeSrv = 33;
    private const int MaxEntries = 64;
    private readonly HashSet<string> _instances = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, (string Host, int Port)> _srv = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, string> _txt = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, IPAddress> _aaaa = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>
    /// Adds the records of one packet. <paramref name="scopeId"/> is the interface index for link-local
    /// addresses. Returns the endpoints that became complete or changed with this packet.
    /// </summary>
    public IReadOnlyList<AirDropServiceEndpoint> Add(ReadOnlySpan<byte> packet, long scopeId)
    {
        var before = Snapshot(scopeId).ToDictionary(e => e.Instance, StringComparer.OrdinalIgnoreCase);
        try
        {
            Parse(packet, scopeId);
        }
        catch (ProtocolFormatException)
        {
            return [];
        }
        return Snapshot(scopeId)
            .Where(e => !before.TryGetValue(e.Instance, out AirDropServiceEndpoint? old) || !old.Equals(e))
            .ToList();
    }

    /// <summary>All complete endpoints known so far.</summary>
    public IReadOnlyList<AirDropServiceEndpoint> Snapshot(long scopeId)
    {
        var list = new List<AirDropServiceEndpoint>();
        foreach (string instance in _instances)
        {
            if (!_srv.TryGetValue(instance, out var srv) || !_aaaa.TryGetValue(srv.Host, out IPAddress? address)) continue;
            var scoped = new IPAddress(address.GetAddressBytes(), address.IsIPv6LinkLocal ? scopeId : 0);
            string id = instance.Split('.')[0];
            list.Add(new AirDropServiceEndpoint(instance, id, new IPEndPoint(scoped, srv.Port), _txt.GetValueOrDefault(instance)));
        }
        return list;
    }

    private void Parse(ReadOnlySpan<byte> packet, long scopeId)
    {
        if (packet.Length < 12) throw new ProtocolFormatException("short DNS packet");
        ushort flags = BinaryPrimitives.ReadUInt16BigEndian(packet[2..]);
        if ((flags & 0x8000) == 0) return;                          // a query, not a response
        int qd = BinaryPrimitives.ReadUInt16BigEndian(packet[4..]);
        int records = BinaryPrimitives.ReadUInt16BigEndian(packet[6..]) + BinaryPrimitives.ReadUInt16BigEndian(packet[8..])
                      + BinaryPrimitives.ReadUInt16BigEndian(packet[10..]);
        if (qd > 64 || records > 256) throw new ProtocolFormatException("too many DNS entries");
        int pos = 12;
        for (int i = 0; i < qd; i++)
        {
            AirDropServiceRecords.ReadName(packet, ref pos);
            pos += 4;
        }
        for (int i = 0; i < records; i++)
        {
            string name = AirDropServiceRecords.ReadName(packet, ref pos);
            if (pos + 10 > packet.Length) throw new ProtocolFormatException("truncated DNS record");
            ushort type = BinaryPrimitives.ReadUInt16BigEndian(packet[pos..]);
            uint ttl = BinaryPrimitives.ReadUInt32BigEndian(packet[(pos + 4)..]);
            int len = BinaryPrimitives.ReadUInt16BigEndian(packet[(pos + 8)..]);
            int data = pos + 10;
            if (data + len > packet.Length) throw new ProtocolFormatException("truncated DNS record data");
            pos = data + len;
            bool goodbye = ttl == 0;
            switch (type)
            {
                case TypePtr when name.Equals(AirDropServiceRecords.ServiceType, StringComparison.OrdinalIgnoreCase):
                {
                    int p = data;
                    string instance = AirDropServiceRecords.ReadName(packet, ref p);
                    if (goodbye) Forget(instance);
                    else if (_instances.Count < MaxEntries && instance.EndsWith("." + AirDropServiceRecords.ServiceType, StringComparison.OrdinalIgnoreCase))
                        _instances.Add(instance);
                    break;
                }
                case TypeSrv when len > 6 && name.EndsWith("." + AirDropServiceRecords.ServiceType, StringComparison.OrdinalIgnoreCase):
                {
                    int port = BinaryPrimitives.ReadUInt16BigEndian(packet[(data + 4)..]);
                    int p = data + 6;
                    string host = AirDropServiceRecords.ReadName(packet, ref p);
                    if (goodbye) _srv.Remove(name);
                    else if (port != 0 && _srv.Count < MaxEntries) _srv[name] = (host, port);
                    break;
                }
                case TypeTxt when name.EndsWith("." + AirDropServiceRecords.ServiceType, StringComparison.OrdinalIgnoreCase):
                    if (!goodbye && _txt.Count < MaxEntries) _txt[name] = ReadTxt(packet.Slice(data, len));
                    break;
                case TypeAaaa when len == 16:
                    if (goodbye) _aaaa.Remove(name);
                    else if (_aaaa.Count < MaxEntries) _aaaa[name] = new IPAddress(packet.Slice(data, 16));
                    break;
            }
        }
    }

    private void Forget(string instance)
    {
        _instances.Remove(instance);
        _srv.Remove(instance);
        _txt.Remove(instance);
    }

    private static string ReadTxt(ReadOnlySpan<byte> rdata)
    {
        var parts = new List<string>();
        int i = 0;
        while (i < rdata.Length)
        {
            int n = rdata[i++];
            if (i + n > rdata.Length) break;
            parts.Add(Encoding.UTF8.GetString(rdata.Slice(i, n)));
            i += n;
        }
        return string.Join(";", parts);
    }

    /// <summary>A standard PTR query for <c>_airdrop._tcp.local</c>.</summary>
    public static byte[] BuildQuery()
    {
        using var ms = new MemoryStream();
        ms.Write(new byte[] { 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0 });    // id 0, flags 0, one question
        ms.Write(AirDropServiceRecords.EncodeName(AirDropServiceRecords.ServiceType));
        ms.Write(new byte[] { 0, (byte)TypePtr, 0, 1 });              // PTR, IN (multicast answer)
        return ms.ToArray();
    }
}

/// <summary>
/// Browses for AirDrop receivers on one interface (awdl0): asks for <c>_airdrop._tcp.local</c> at a steady
/// interval rather than with exponential back-off – on AWDL a query or its answer is lost whenever the peer is
/// on another channel (jedbillyb/airdrop-mt7921 FINDINGS §19: 0 → 14 replies) – and reports endpoints as they
/// become complete.
/// </summary>
public sealed class MdnsBrowser : IAsyncDisposable
{
    private static readonly IPAddress Group = IPAddress.Parse("ff02::fb");
    private const int MdnsPort = 5353;
    private readonly int _interfaceIndex;
    private readonly TimeSpan _interval;
    private readonly Socket _socket;
    private readonly IPEndPoint _target;
    private readonly AirDropServiceCache _cache = new();
    private readonly CancellationTokenSource _stop = new();
    private readonly Action<string>? _log;
    private Task? _queryLoop, _receiveLoop;

    /// <summary>Raised on the receive thread for each endpoint that appeared or changed.</summary>
    public event Action<AirDropServiceEndpoint>? Found;

    public MdnsBrowser(int interfaceIndex, TimeSpan? queryInterval = null, Action<string>? log = null, int port = MdnsPort)
    {
        _interfaceIndex = interfaceIndex;
        _interval = queryInterval ?? TimeSpan.FromSeconds(1.5);
        _log = log;
        _target = new IPEndPoint(new IPAddress(Group.GetAddressBytes(), interfaceIndex), port);
        _socket = new Socket(AddressFamily.InterNetworkV6, SocketType.Dgram, ProtocolType.Udp);
        try
        {
            _socket.ExclusiveAddressUse = false;
            _socket.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            _socket.Bind(new IPEndPoint(IPAddress.IPv6Any, port));
            _socket.SetSocketOption(SocketOptionLevel.IPv6, SocketOptionName.AddMembership, new IPv6MulticastOption(Group, interfaceIndex));
            _socket.SetSocketOption(SocketOptionLevel.IPv6, SocketOptionName.MulticastInterface, interfaceIndex);
            _socket.SetSocketOption(SocketOptionLevel.IPv6, SocketOptionName.MulticastTimeToLive, 255);
            _socket.SetSocketOption(SocketOptionLevel.IPv6, SocketOptionName.MulticastLoopback, true);
        }
        catch
        {
            _socket.Dispose();
            throw;
        }
    }

    public IReadOnlyList<AirDropServiceEndpoint> Current
    {
        get { lock (_cache) return _cache.Snapshot(_interfaceIndex); }
    }

    public void Start()
    {
        _queryLoop = Task.Run(QueryLoopAsync);
        _receiveLoop = Task.Run(ReceiveLoopAsync);
    }

    private async Task QueryLoopAsync()
    {
        byte[] query = AirDropServiceCache.BuildQuery();
        try
        {
            while (!_stop.IsCancellationRequested)
            {
                try { await _socket.SendToAsync(query, SocketFlags.None, _target).ConfigureAwait(false); }
                catch (SocketException e) { _log?.Invoke($"mDNS query failed: {e.SocketErrorCode}"); }
                await Task.Delay(_interval, _stop.Token).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) { }
    }

    private async Task ReceiveLoopAsync()
    {
        byte[] buf = new byte[9000];
        EndPoint any = new IPEndPoint(IPAddress.IPv6Any, 0);
        while (!_stop.IsCancellationRequested)
        {
            try
            {
                SocketReceiveFromResult r = await _socket.ReceiveFromAsync(buf, SocketFlags.None, any, _stop.Token).ConfigureAwait(false);
                IReadOnlyList<AirDropServiceEndpoint> news;
                lock (_cache) news = _cache.Add(buf.AsSpan(0, r.ReceivedBytes), _interfaceIndex);
                foreach (AirDropServiceEndpoint e in news) Found?.Invoke(e);
            }
            catch (OperationCanceledException) { break; }
            catch (ObjectDisposedException) { break; }
            catch (SocketException e)
            {
                if (_stop.IsCancellationRequested) break;
                _log?.Invoke($"mDNS receive failed: {e.SocketErrorCode}");
                try { await Task.Delay(200, _stop.Token).ConfigureAwait(false); } catch (OperationCanceledException) { break; }
            }
        }
    }

    public async ValueTask DisposeAsync()
    {
        _stop.Cancel();
        if (_queryLoop is not null) await _queryLoop.ConfigureAwait(false);
        _socket.Dispose();
        if (_receiveLoop is not null) await _receiveLoop.ConfigureAwait(false);
        _stop.Dispose();
    }
}
