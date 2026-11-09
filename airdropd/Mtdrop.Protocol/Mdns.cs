using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Text;

namespace Mtdrop.Protocol;

/// <summary>The fixed set of DNS-SD records an AirDrop receiver publishes (docs/PROTOCOL.md §4.1).</summary>
public sealed class AirDropServiceRecords
{
    public const string ServiceType = "_airdrop._tcp.local";
    public const int DefaultFlags = 0x88;   // mixed types + supports /Discover; proven with iOS 26.5.2

    /// <summary>12 lowercase hex characters; keep stable across restarts.</summary>
    public required string ServiceId { get; init; }
    public required IPAddress Address { get; init; }
    public required int Port { get; init; }
    public int Flags { get; init; } = DefaultFlags;

    public string InstanceName => $"{ServiceId}.{ServiceType}";

    /// <summary>Host name derived from the service id, so the real machine name is not broadcast.</summary>
    public string HostName => $"{ServiceId}.local";

    public static string NewServiceId() => Convert.ToHexString(System.Security.Cryptography.RandomNumberGenerator.GetBytes(6)).ToLowerInvariant();

    private const ushort TypeA = 1, TypePtr = 12, TypeTxt = 16, TypeAaaa = 28, TypeSrv = 33, TypeNsec = 47, TypeAny = 255;
    private const ushort ClassIn = 1, CacheFlush = 0x8000;

    /// <summary>Unsolicited response carrying all records; with <paramref name="goodbye"/> every TTL is 0.</summary>
    public byte[] BuildAnnouncement(bool goodbye = false)
    {
        uint Ttl(uint v) => goodbye ? 0 : v;
        using var ms = new MemoryStream();
        Span<byte> header = stackalloc byte[12];
        BinaryPrimitives.WriteUInt16BigEndian(header[2..], 0x8400);   // response, authoritative
        BinaryPrimitives.WriteUInt16BigEndian(header[6..], 5);        // answers
        ms.Write(header);

        WriteRecord(ms, ServiceType, TypePtr, ClassIn, Ttl(4500), EncodeName(InstanceName));

        using (var srv = new MemoryStream())
        {
            srv.Write([0, 0, 0, 0, (byte)(Port >> 8), (byte)Port]);   // priority 0, weight 0, port
            srv.Write(EncodeName(HostName));
            WriteRecord(ms, InstanceName, TypeSrv, ClassIn | CacheFlush, Ttl(120), srv.ToArray());
        }

        byte[] txt = Encoding.ASCII.GetBytes($"flags={Flags}");
        WriteRecord(ms, InstanceName, TypeTxt, ClassIn | CacheFlush, Ttl(4500), [(byte)txt.Length, .. txt]);

        WriteRecord(ms, HostName, TypeAaaa, ClassIn | CacheFlush, Ttl(120), Address.GetAddressBytes());

        // NSEC: "this host has AAAA and NSEC only" so senders stop waiting for an A record.
        byte[] nsec = [.. EncodeName(HostName), 0x00, 0x06, 0x00, 0x00, 0x00, 0x08, 0x00, 0x01];
        WriteRecord(ms, HostName, TypeNsec, ClassIn | CacheFlush, Ttl(120), nsec);
        return ms.ToArray();
    }

    /// <summary>True if the packet is a query with at least one question our records answer.</summary>
    public bool IsQueryForUs(ReadOnlySpan<byte> packet)
    {
        try
        {
            if (packet.Length < 12) return false;
            ushort flags = BinaryPrimitives.ReadUInt16BigEndian(packet[2..]);
            if ((flags & 0x8000) != 0) return false;                  // a response
            int questions = BinaryPrimitives.ReadUInt16BigEndian(packet[4..]);
            if (questions > 64) return false;
            int pos = 12;
            for (int i = 0; i < questions; i++)
            {
                string name = ReadName(packet, ref pos);
                if (pos + 4 > packet.Length) return false;
                ushort type = BinaryPrimitives.ReadUInt16BigEndian(packet[pos..]);
                pos += 4;
                if (name.Equals(ServiceType, StringComparison.OrdinalIgnoreCase) && type is TypePtr or TypeAny) return true;
                if (name.Equals(InstanceName, StringComparison.OrdinalIgnoreCase) && type is TypeSrv or TypeTxt or TypeAny) return true;
                if (name.Equals(HostName, StringComparison.OrdinalIgnoreCase) && type is TypeAaaa or TypeA or TypeAny) return true;
            }
            return false;
        }
        catch (ProtocolFormatException)
        {
            return false;
        }
    }

    private static void WriteRecord(Stream s, string name, ushort type, int cls, uint ttl, byte[] rdata)
    {
        s.Write(EncodeName(name));
        Span<byte> fixedPart = stackalloc byte[10];
        BinaryPrimitives.WriteUInt16BigEndian(fixedPart, type);
        BinaryPrimitives.WriteUInt16BigEndian(fixedPart[2..], (ushort)cls);
        BinaryPrimitives.WriteUInt32BigEndian(fixedPart[4..], ttl);
        BinaryPrimitives.WriteUInt16BigEndian(fixedPart[8..], (ushort)rdata.Length);
        s.Write(fixedPart);
        s.Write(rdata);
    }

    public static byte[] EncodeName(string name)
    {
        using var ms = new MemoryStream();
        foreach (string label in name.Split('.', StringSplitOptions.RemoveEmptyEntries))
        {
            byte[] b = Encoding.UTF8.GetBytes(label);
            if (b.Length is 0 or > 63) throw new ArgumentException("bad DNS label");
            ms.WriteByte((byte)b.Length);
            ms.Write(b);
        }
        ms.WriteByte(0);
        return ms.ToArray();
    }

    /// <summary>Reads a possibly compressed DNS name. Bounded against pointer loops.</summary>
    public static string ReadName(ReadOnlySpan<byte> packet, ref int pos)
    {
        var sb = new StringBuilder();
        int p = pos, jumps = 0;
        bool jumped = false;
        while (true)
        {
            if (p >= packet.Length) throw new ProtocolFormatException("DNS name out of range");
            int len = packet[p];
            if (len == 0)
            {
                if (!jumped) pos = p + 1;
                return sb.ToString();
            }
            if ((len & 0xC0) == 0xC0)
            {
                if (p + 1 >= packet.Length || ++jumps > 16) throw new ProtocolFormatException("bad DNS name pointer");
                if (!jumped) pos = p + 2;
                jumped = true;
                p = ((len & 0x3F) << 8) | packet[p + 1];
                continue;
            }
            if ((len & 0xC0) != 0 || p + 1 + len > packet.Length || sb.Length > 255) throw new ProtocolFormatException("bad DNS label");
            if (sb.Length > 0) sb.Append('.');
            sb.Append(Encoding.UTF8.GetString(packet.Slice(p + 1, len)));
            p += 1 + len;
        }
    }
}

/// <summary>
/// Minimal mDNS responder for one AirDrop service on one interface: periodic
/// unsolicited announcements plus rate-limited answers to matching queries.
/// Bound by interface index, never by address (docs/PROTOCOL.md §4.3).
/// </summary>
public sealed class MdnsResponder : IAsyncDisposable
{
    private static readonly IPAddress Group = IPAddress.Parse("ff02::fb");
    private const int MdnsPort = 5353;
    private readonly AirDropServiceRecords _records;
    private readonly Socket _socket;
    private readonly IPEndPoint _target;
    private readonly TimeSpan _interval;
    private readonly CancellationTokenSource _stop = new();
    private readonly Action<string>? _log;
    private Task? _announceLoop, _receiveLoop;
    private long _lastAnswerTicks;

    public MdnsResponder(AirDropServiceRecords records, int interfaceIndex, TimeSpan? announceInterval = null, Action<string>? log = null)
    {
        _records = records;
        _interval = announceInterval ?? TimeSpan.FromSeconds(2);
        _log = log;
        _target = new IPEndPoint(new IPAddress(Group.GetAddressBytes(), interfaceIndex), MdnsPort);
        _socket = new Socket(AddressFamily.InterNetworkV6, SocketType.Dgram, ProtocolType.Udp);
        try
        {
            // The operating system's own mDNS responder also listens on 5353.
            _socket.ExclusiveAddressUse = false;
            _socket.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            _socket.Bind(new IPEndPoint(IPAddress.IPv6Any, MdnsPort));
            _socket.SetSocketOption(SocketOptionLevel.IPv6, SocketOptionName.AddMembership, new IPv6MulticastOption(Group, interfaceIndex));
            _socket.SetSocketOption(SocketOptionLevel.IPv6, SocketOptionName.MulticastInterface, interfaceIndex);
            _socket.SetSocketOption(SocketOptionLevel.IPv6, SocketOptionName.MulticastTimeToLive, 255);
            // On Windows this option acts on the receiving side. Left enabled so that local tools on the same
            // machine can query us; our own announcements come back too and are ignored (they are responses).
            _socket.SetSocketOption(SocketOptionLevel.IPv6, SocketOptionName.MulticastLoopback, true);
        }
        catch
        {
            _socket.Dispose();
            throw;
        }
    }

    public void Start()
    {
        _announceLoop = Task.Run(AnnounceLoopAsync);
        _receiveLoop = Task.Run(ReceiveLoopAsync);
    }

    private async Task SendAsync(byte[] packet)
    {
        try
        {
            await _socket.SendToAsync(packet, SocketFlags.None, _target).ConfigureAwait(false);
        }
        catch (SocketException e)
        {
            _log?.Invoke($"mDNS send failed: {e.SocketErrorCode}");
        }
    }

    private async Task AnnounceLoopAsync()
    {
        byte[] packet = _records.BuildAnnouncement();
        try
        {
            while (!_stop.IsCancellationRequested)
            {
                await SendAsync(packet).ConfigureAwait(false);
                await Task.Delay(_interval, _stop.Token).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) { }
    }

    private async Task ReceiveLoopAsync()
    {
        byte[] buf = new byte[9000];
        byte[] answer = _records.BuildAnnouncement();
        EndPoint any = new IPEndPoint(IPAddress.IPv6Any, 0);
        while (!_stop.IsCancellationRequested)
        {
            try
            {
                SocketReceiveFromResult r = await _socket.ReceiveFromAsync(buf, SocketFlags.None, any, _stop.Token).ConfigureAwait(false);
                if (!_records.IsQueryForUs(buf.AsSpan(0, r.ReceivedBytes))) continue;
                long now = Environment.TickCount64;
                if (now - Interlocked.Read(ref _lastAnswerTicks) < 100) continue;   // senders repeat queries in bursts
                Interlocked.Exchange(ref _lastAnswerTicks, now);
                await SendAsync(answer).ConfigureAwait(false);
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
        if (_announceLoop is not null) await _announceLoop.ConfigureAwait(false);
        await SendAsync(_records.BuildAnnouncement(goodbye: true)).ConfigureAwait(false);
        _socket.Dispose();
        if (_receiveLoop is not null) await _receiveLoop.ConfigureAwait(false);
        _stop.Dispose();
    }
}
