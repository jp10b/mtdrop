using System.IO.Compression;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Text;
using Mtdrop.Protocol;

namespace Mtdrop.Protocol.Tests;

/// <summary>
/// Builds synthetic requests with the shape iOS 26.5.2 sends (docs/FINDINGS.md F-002).
/// All values are made up; nothing here comes from a capture.
/// </summary>
public static class Wire
{
    public const int PhoneChunk = 16371;

    public static byte[] Chunked(byte[] data, int chunkSize = PhoneChunk, bool terminate = true)
    {
        using var ms = new MemoryStream();
        for (int i = 0; i < data.Length; i += chunkSize)
        {
            int n = Math.Min(chunkSize, data.Length - i);
            ms.Write(Encoding.ASCII.GetBytes($"{n:x}\r\n"));
            ms.Write(data, i, n);
            ms.Write("\r\n"u8);
        }
        if (terminate) ms.Write("0\r\n\r\n"u8);
        return ms.ToArray();
    }

    /// <summary>dvzip with 128 KiB input blocks; blocks from <paramref name="storedFrom"/> on are stored.</summary>
    public static byte[] Dvzip(byte[] raw, int storedFrom = int.MaxValue, int blockSize = 131072)
    {
        using var ms = new MemoryStream();
        int index = 0;
        for (int i = 0; i < raw.Length; i += blockSize, index++)
        {
            ReadOnlySpan<byte> part = raw.AsSpan(i, Math.Min(blockSize, raw.Length - i));
            byte[] payload;
            uint flag = 0;
            if (index >= storedFrom) { payload = part.ToArray(); flag = 0x80000000; }
            else
            {
                using var z = new MemoryStream();
                using (var zs = new ZLibStream(z, CompressionLevel.Fastest, leaveOpen: true)) zs.Write(part);
                payload = z.ToArray();
            }
            byte[] h = new byte[4];
            System.Buffers.Binary.BinaryPrimitives.WriteUInt32BigEndian(h, flag | (uint)payload.Length);
            ms.Write(h);
            ms.Write(payload);
        }
        return ms.ToArray();
    }

    public sealed record Member(string Name, int Mode, byte[] Data, int Links = 1);

    public static byte[] Cpio(IEnumerable<Member> members, bool trailer = true, int padTo = 512)
    {
        using var ms = new MemoryStream();
        int ino = 0;
        void Header(string name, int mode, long size, int nlink)
        {
            byte[] nb = [.. Encoding.UTF8.GetBytes(name), 0];
            string h = "070707" + Oct(0, 6) + Oct(ino++, 6) + Oct(mode, 6) + Oct(0, 6) + Oct(0, 6) + Oct(nlink, 6)
                       + Oct(0, 6) + Oct(0, 11) + Oct(nb.Length, 6) + Oct(size, 11);
            Assert.Equal(76, h.Length);
            ms.Write(Encoding.ASCII.GetBytes(h));
            ms.Write(nb);
        }
        foreach (Member m in members)
        {
            Header(m.Name, m.Mode, m.Data.Length, m.Links);
            ms.Write(m.Data);
        }
        if (trailer)
        {
            Header("TRAILER!!!", 0, 0, 1);
            if (padTo > 0 && ms.Length % padTo != 0) ms.Write(new byte[padTo - ms.Length % padTo]);
        }
        return ms.ToArray();
    }

    private static string Oct(long v, int width) => Convert.ToString(v, 8).PadLeft(width, '0');

    public const int Dir = 0x41C0;      // 040700
    public const int File = 0x81A4;     // 100644
    public const int Symlink = 0xA1FF;  // 120777

    /// <summary>Archive laid out like the phone's: ".", "./name", "./._name", trailer.</summary>
    public static byte[] PhotoArchive(string name, byte[] photo) => Cpio(
    [
        new Member(".", Dir, [], 2),
        new Member("./" + name, File, photo),
        new Member("./._" + name, File, [0x00, 0x05, 0x16, 0x07, .. new byte[60]]),
    ]);

    public static byte[] AskBody(string transferId, params (string Name, long Size)[] files) =>
        BinaryPlist.Write(new Dictionary<string, object?>
        {
            ["TransferID"] = new Dictionary<string, object?> { ["id"] = transferId },
            ["TransferType"] = new Dictionary<string, object?> { ["files"] = new Dictionary<string, object?>() },
            ["SenderRecordData"] = new byte[3802],
            ["SenderID"] = "0123456789ab",
            ["BundleID"] = "com.apple.mobileslideshow",
            ["SenderComputerName"] = "Test Phone",
            ["SenderModelName"] = "iPhone",
            ["Items"] = new List<object?>(),
            ["Files"] = files.Select(f => (object?)new Dictionary<string, object?>
            {
                ["FileName"] = f.Name,
                ["FileType"] = "public.jpeg",
                ["FileSize"] = f.Size,
                ["FileBomPath"] = "./" + f.Name,
                ["FileIsDirectory"] = false,
                ["ShouldConvertMediaFormats"] = false,
            }).ToList(),
            ["FileIcon"] = new byte[150_000],
            ["ConvertMediaFormats"] = false,
            ["SenderIdentityAuthTag"] = new byte[] { 1, 2, 3 },
        });

    public static byte[] Request(string path, string extraHeaders, string connection, byte[] chunkedBody) =>
    [
        .. Encoding.ASCII.GetBytes($"POST {path} HTTP/1.1\r\nUser-Agent: AirDrop/1.0\r\n{extraHeaders}Connection: {connection}\r\nTransfer-Encoding: chunked\r\n\r\n"),
        .. chunkedBody,
    ];

    public static byte[] DiscoverRequest() => Request("/Discover", "", "close",
        Chunked(BinaryPlist.Write(new Dictionary<string, object?>
        {
            ["SenderRecordData"] = new byte[3802],
            ["DeviceSupportFlags"] = 111611L,
        })));

    public static byte[] AskRequest(string transferId, params (string Name, long Size)[] files) =>
        Request("/Ask", "", "keep-alive", Chunked(AskBody(transferId, files), 131072));

    public static byte[] UploadRequest(string transferId, long totalBytes, byte[] dvzip, bool terminate = true,
        string contentType = "application/x-dvzip") =>
        Request("/Upload",
            $"TotalBytes: {totalBytes}\r\nContent-Type: {contentType}\r\nSenderPseudonym: pseud:AAAAAAAAAAAAAAAAAAAAAA\r\n"
            + $"SenderPushToken: {new string('0', 64)}\r\nTransferID: {transferId}\r\n",
            "keep-alive", Chunked(dvzip, terminate: terminate));
}

public sealed record Response(int Status, Dictionary<string, string> Headers, byte[] Body);

/// <summary>TLS client that sends raw bytes, like the phone: no Host header, self-signed server accepted.</summary>
public sealed class FakeSender : IAsyncDisposable
{
    private readonly TcpClient _tcp = new(AddressFamily.InterNetworkV6);
    private SslStream? _tls;

    public static async Task<FakeSender> ConnectAsync(int port)
    {
        var s = new FakeSender();
        await s._tcp.ConnectAsync(IPAddress.IPv6Loopback, port);
        s._tls = new SslStream(s._tcp.GetStream(), false, (_, _, _, _) => true);
        await s._tls.AuthenticateAsClientAsync(new SslClientAuthenticationOptions { TargetHost = "airdrop.invalid" });
        return s;
    }

    public SslStream Tls => _tls!;

    public Task SendAsync(byte[] data) => _tls!.WriteAsync(data).AsTask();

    public async Task<Response> ReadResponseAsync(int timeoutMs = 10_000)
    {
        using var cts = new CancellationTokenSource(timeoutMs);
        var head = new MemoryStream();
        byte[] one = new byte[1];
        while (true)
        {
            int n = await _tls!.ReadAsync(one, cts.Token);
            if (n == 0) throw new EndOfStreamException("connection closed before a response");
            head.WriteByte(one[0]);
            if (head.Length >= 4 && head.GetBuffer().AsSpan((int)head.Length - 4, 4).SequenceEqual("\r\n\r\n"u8)) break;
        }
        string[] lines = Encoding.ASCII.GetString(head.ToArray()).Split("\r\n", StringSplitOptions.RemoveEmptyEntries);
        int status = int.Parse(lines[0].Split(' ')[1]);
        var headers = lines.Skip(1).Select(l => l.Split(": ", 2)).ToDictionary(p => p[0], p => p[1], StringComparer.OrdinalIgnoreCase);
        byte[] body = new byte[headers.TryGetValue("Content-Length", out string? cl) ? int.Parse(cl) : 0];
        await _tls!.ReadExactlyAsync(body, cts.Token);
        return new Response(status, headers, body);
    }

    public async Task<Response> RoundTripAsync(byte[] request)
    {
        await SendAsync(request);
        return await ReadResponseAsync();
    }

    public async ValueTask DisposeAsync()
    {
        if (_tls is not null) await _tls.DisposeAsync();
        _tcp.Dispose();
    }
}
