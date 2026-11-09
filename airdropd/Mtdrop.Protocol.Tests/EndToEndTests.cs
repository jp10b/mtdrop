using System.Net;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using Mtdrop.Protocol;

namespace Mtdrop.Protocol.Tests;

/// <summary>
/// Full receive flow over real TLS on the loopback interface, with a client that
/// behaves like the phone in docs/FINDINGS.md F-002.
/// </summary>
public sealed class EndToEndTests : IAsyncLifetime
{
    private static readonly X509Certificate2 Certificate = ServerIdentity.Create();
    private readonly string _dir = Path.Combine(Path.GetTempPath(), "mtdrop-test-" + Guid.NewGuid().ToString("N"));
    private readonly List<AskRequest> _asks = [];
    private readonly List<TransferResult> _results = [];
    private readonly SemaphoreSlim _finished = new(0);
    private Func<AskRequest, CancellationToken, Task<bool>> _consent = (_, _) => Task.FromResult(true);
    private AirDropServer _server = null!;

    public Task InitializeAsync()
    {
        Directory.CreateDirectory(_dir);
        _server = NewServer();
        return Task.CompletedTask;
    }

    private AirDropServer NewServer(TimeSpan? uploadIdle = null, TimeSpan? consentTimeout = null)
    {
        var server = new AirDropServer(new AirDropServerOptions
        {
            ComputerName = "TEST-PC",
            DestinationDirectory = _dir,
            BindAddress = IPAddress.IPv6Loopback,
            Port = 0,
            Consent = (ask, ct) => { lock (_asks) _asks.Add(ask); return _consent(ask, ct); },
            UploadIdleTimeout = uploadIdle ?? TimeSpan.FromSeconds(10),
            ConsentTimeout = consentTimeout ?? TimeSpan.FromSeconds(10),
        }, Certificate);
        server.TransferFinished += r => { lock (_results) _results.Add(r); _finished.Release(); };
        server.Start();
        return server;
    }

    public async Task DisposeAsync()
    {
        await _server.DisposeAsync();
        try { Directory.Delete(_dir, recursive: true); } catch (IOException) { }
    }

    private string[] SavedFiles() => [.. Directory.GetFiles(_dir).Select(Path.GetFileName).Order()!];

    private async Task<TransferResult> WaitForResultAsync()
    {
        Assert.True(await _finished.WaitAsync(TimeSpan.FromSeconds(15)), "no TransferFinished event");
        lock (_results) return _results[^1];
    }

    private static string NewId() => Guid.NewGuid().ToString().ToUpperInvariant();

    private static byte[] Photo(int size = 2_374_534)
    {
        byte[] p = RandomNumberGenerator.GetBytes(size);
        p[0] = 0xFF; p[1] = 0xD8; p[^2] = 0xFF; p[^1] = 0xD9;
        return p;
    }

    [Fact]
    public async Task Discover_returns_the_receiver_name()
    {
        await using var c = await FakeSender.ConnectAsync(_server.Port);
        Response r = await c.RoundTripAsync(Wire.DiscoverRequest());
        Assert.Equal(200, r.Status);
        var p = BinaryPlist.ReadDictionary(r.Body);
        Assert.Equal("TEST-PC", p["ReceiverComputerName"]);
        Assert.Equal("mtdrop", p["ReceiverModelName"]);
        Assert.Equal("{\"Version\": 1}", Encoding.UTF8.GetString((byte[])p["ReceiverMediaCapabilities"]!));
        Assert.False(p.ContainsKey("ReceiverRecordData"));
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]   // the phone may withhold the final 0-chunk
    public async Task Photo_is_received_like_from_the_phone(bool sendFinalChunk)
    {
        byte[] photo = Photo();
        string id = NewId();
        await using var c = await FakeSender.ConnectAsync(_server.Port);

        Response ask = await c.RoundTripAsync(Wire.AskRequest(id, ("IMG_0001.JPG", photo.Length)));
        Assert.Equal(200, ask.Status);
        Assert.Equal("TEST-PC", BinaryPlist.ReadDictionary(ask.Body)["ReceiverComputerName"]);

        byte[] dvzip = Wire.Dvzip(Wire.PhotoArchive("IMG_0001.JPG", photo), storedFrom: 12);
        Response up = await c.RoundTripAsync(Wire.UploadRequest(id, photo.Length, dvzip, terminate: sendFinalChunk));
        Assert.Equal(200, up.Status);
        Assert.Equal("close", up.Headers["Connection"]);

        TransferResult result = await WaitForResultAsync();
        Assert.True(result.Succeeded, result.Error);
        Assert.Equal(["IMG_0001.JPG"], SavedFiles());                 // no "._" sidecar, no temp file
        Assert.Equal(photo, await File.ReadAllBytesAsync(result.SavedFiles[0]));
        AskRequest seen = Assert.Single(_asks);
        Assert.Equal("Test Phone", seen.SenderName);
        Assert.Equal(new AnnouncedFile("IMG_0001.JPG", "public.jpeg", photo.Length, false), Assert.Single(seen.Files));
        if (OperatingSystem.IsWindows())
            Assert.Contains("ZoneId=3", await File.ReadAllTextAsync(result.SavedFiles[0] + ":Zone.Identifier"));
    }

    [Fact]
    public async Task Declined_ask_gets_the_decline_status_and_saves_nothing()
    {
        _consent = (_, _) => Task.FromResult(false);
        await using var c = await FakeSender.ConnectAsync(_server.Port);
        Response ask = await c.RoundTripAsync(Wire.AskRequest(NewId(), ("IMG_0001.JPG", 100)));
        Assert.Equal(403, ask.Status);
        Assert.Empty(SavedFiles());
    }

    [Fact]
    public async Task Unanswered_prompt_times_out_as_a_decline()
    {
        await _server.DisposeAsync();
        _server = NewServer(consentTimeout: TimeSpan.FromMilliseconds(300));
        _consent = async (_, ct) => { await Task.Delay(Timeout.Infinite, ct); return true; };
        await using var c = await FakeSender.ConnectAsync(_server.Port);
        Assert.Equal(403, (await c.RoundTripAsync(Wire.AskRequest(NewId(), ("a.jpg", 10)))).Status);
    }

    [Fact]
    public async Task Upload_without_an_accepted_ask_is_refused()
    {
        byte[] photo = Photo(5000);
        await using var c = await FakeSender.ConnectAsync(_server.Port);
        Response up = await c.RoundTripAsync(Wire.UploadRequest(NewId(), photo.Length, Wire.Dvzip(Wire.PhotoArchive("x.jpg", photo))));
        Assert.Equal(403, up.Status);
        Assert.Empty(SavedFiles());
        Assert.Empty(_asks);
    }

    [Fact]
    public async Task Upload_with_a_different_transfer_id_is_refused()
    {
        byte[] photo = Photo(5000);
        await using var c = await FakeSender.ConnectAsync(_server.Port);
        Assert.Equal(200, (await c.RoundTripAsync(Wire.AskRequest(NewId(), ("x.jpg", photo.Length)))).Status);
        Response up = await c.RoundTripAsync(Wire.UploadRequest(NewId(), photo.Length, Wire.Dvzip(Wire.PhotoArchive("x.jpg", photo))));
        Assert.Equal(403, up.Status);
        Assert.Empty(SavedFiles());
    }

    [Fact]
    public async Task Accepted_ask_on_one_connection_does_not_authorise_another()
    {
        byte[] photo = Photo(5000);
        string id = NewId();
        await using var a = await FakeSender.ConnectAsync(_server.Port);
        Assert.Equal(200, (await a.RoundTripAsync(Wire.AskRequest(id, ("x.jpg", photo.Length)))).Status);
        await using var b = await FakeSender.ConnectAsync(_server.Port);
        Response up = await b.RoundTripAsync(Wire.UploadRequest(id, photo.Length, Wire.Dvzip(Wire.PhotoArchive("x.jpg", photo))));
        Assert.Equal(403, up.Status);
        Assert.Empty(SavedFiles());
    }

    [Fact]
    public async Task Stalled_upload_leaves_no_file_and_the_retry_succeeds()
    {
        await _server.DisposeAsync();
        _server = NewServer(uploadIdle: TimeSpan.FromMilliseconds(500));
        byte[] photo = Photo(600_000);
        byte[] dvzip = Wire.Dvzip(Wire.PhotoArchive("IMG_0002.JPG", photo), storedFrom: 0);

        string id1 = NewId();
        await using (var c = await FakeSender.ConnectAsync(_server.Port))
        {
            Assert.Equal(200, (await c.RoundTripAsync(Wire.AskRequest(id1, ("IMG_0002.JPG", photo.Length)))).Status);
            byte[] request = Wire.UploadRequest(id1, photo.Length, dvzip, terminate: false);
            await c.SendAsync(request[..(request.Length / 2)]);          // then silence, like attempt 1 in F-001
            TransferResult failed = await WaitForResultAsync();
            Assert.False(failed.Succeeded);
            Assert.Equal("sender went quiet", failed.Error);
            Assert.Empty(SavedFiles());
        }

        // The phone retries by itself with a new TransferID.
        string id2 = NewId();
        await using var d = await FakeSender.ConnectAsync(_server.Port);
        Assert.Equal(200, (await d.RoundTripAsync(Wire.AskRequest(id2, ("IMG_0002.JPG", photo.Length)))).Status);
        Assert.Equal(200, (await d.RoundTripAsync(Wire.UploadRequest(id2, photo.Length, dvzip))).Status);
        Assert.True((await WaitForResultAsync()).Succeeded);
        Assert.Equal(["IMG_0002.JPG"], SavedFiles());
    }

    [Fact]
    public async Task Existing_files_are_never_overwritten()
    {
        await File.WriteAllTextAsync(Path.Combine(_dir, "IMG_0003.JPG"), "mine");
        byte[] photo = Photo(4000);
        for (int i = 0; i < 2; i++)
        {
            string id = NewId();
            await using var c = await FakeSender.ConnectAsync(_server.Port);
            await c.RoundTripAsync(Wire.AskRequest(id, ("IMG_0003.JPG", photo.Length)));
            Assert.Equal(200, (await c.RoundTripAsync(Wire.UploadRequest(id, photo.Length, Wire.Dvzip(Wire.PhotoArchive("IMG_0003.JPG", photo))))).Status);
            await WaitForResultAsync();
        }
        Assert.Equal(["IMG_0003 (1).JPG", "IMG_0003 (2).JPG", "IMG_0003.JPG"], SavedFiles());
        Assert.Equal("mine", await File.ReadAllTextAsync(Path.Combine(_dir, "IMG_0003.JPG")));
    }

    [Fact]
    public async Task Several_files_arrive_in_one_archive()
    {
        byte[] a = Photo(3000), b = Photo(7000);
        byte[] archive = Wire.Cpio(
        [
            new Wire.Member(".", Wire.Dir, [], 2),
            new Wire.Member("./NSIRD_AirDrop_abc", Wire.Dir, [], 2),
            new Wire.Member("./NSIRD_AirDrop_abc/A.JPG", Wire.File, a),
            new Wire.Member("./NSIRD_AirDrop_abc/._A.JPG", Wire.File, new byte[64]),
            new Wire.Member("./NSIRD_AirDrop_abc/B.JPG", Wire.File, b),
        ]);
        string id = NewId();
        await using var c = await FakeSender.ConnectAsync(_server.Port);
        await c.RoundTripAsync(Wire.AskRequest(id, ("A.JPG", a.Length), ("B.JPG", b.Length)));
        Assert.Equal(200, (await c.RoundTripAsync(Wire.UploadRequest(id, a.Length + b.Length, Wire.Dvzip(archive)))).Status);
        Assert.True((await WaitForResultAsync()).Succeeded);
        Assert.Equal(["A.JPG", "B.JPG"], SavedFiles());
    }

    public static TheoryData<string, Wire.Member[]> HostileArchives => new()
    {
        { "path traversal", [new(".", Wire.Dir, [], 2), new("./ok.jpg", Wire.File, new byte[10]), new("../../evil.exe", Wire.File, new byte[10])] },
        { "absolute path", [new("/Windows/evil.exe", Wire.File, new byte[10])] },
        { "symlink", [new("./link", Wire.Symlink, "C:/Windows"u8.ToArray())] },
        { "hard link", [new("./twice", Wire.File, new byte[10], Links: 2)] },
        { "more data than announced", [new("./big.jpg", Wire.File, new byte[40 * 1024 * 1024])] },
        { "only a sidecar", [new(".", Wire.Dir, [], 2), new("./._x", Wire.File, new byte[10])] },
    };

    [Theory]
    [MemberData(nameof(HostileArchives))]
    public async Task Hostile_archives_are_rejected_and_leave_nothing_behind(string label, Wire.Member[] members)
    {
        string id = NewId();
        await using var c = await FakeSender.ConnectAsync(_server.Port);
        Assert.Equal(200, (await c.RoundTripAsync(Wire.AskRequest(id, ("ok.jpg", 10)))).Status);
        try
        {
            Response up = await c.RoundTripAsync(Wire.UploadRequest(id, 10, Wire.Dvzip(Wire.Cpio(members))));
            Assert.True(up.Status == 422, $"{label}: status {up.Status}");
        }
        catch (IOException)
        {
            // The server rejects as soon as it sees the problem and closes; a sender
            // still pushing a large body then gets a reset instead of the status.
        }
        Assert.False((await WaitForResultAsync()).Succeeded);
        Assert.Empty(Directory.GetFileSystemEntries(_dir));
        Assert.False(File.Exists(Path.Combine(Path.GetDirectoryName(_dir)!, "evil.exe")));
    }

    [Fact]
    public async Task Link_shares_and_unknown_formats_are_declined()
    {
        byte[] link = BinaryPlist.Write(new Dictionary<string, object?>
        {
            ["TransferID"] = new Dictionary<string, object?> { ["id"] = NewId() },
            ["TransferType"] = new Dictionary<string, object?> { ["links"] = new Dictionary<string, object?>() },
            ["Items"] = new List<object?> { "https://example.org/" },
            ["Files"] = new List<object?>(),
            ["SenderComputerName"] = "Test Phone",
        });
        await using (var c = await FakeSender.ConnectAsync(_server.Port))
            Assert.Equal(403, (await c.RoundTripAsync(Wire.Request("/Ask", "", "keep-alive", Wire.Chunked(link)))).Status);
        Assert.Empty(_asks);

        string id = NewId();
        await using var d = await FakeSender.ConnectAsync(_server.Port);
        await d.RoundTripAsync(Wire.AskRequest(id, ("x.jpg", 10)));
        Response up = await d.RoundTripAsync(Wire.UploadRequest(id, 10, new byte[100], contentType: "application/x-cpio"));
        Assert.Equal(415, up.Status);
    }

    [Theory]
    [InlineData("GET /Discover HTTP/1.1\r\n\r\n", 405)]
    [InlineData("POST /Nope HTTP/1.1\r\nContent-Length: 0\r\n\r\n", 400)]
    [InlineData("POST /Ask HTTP/1.1\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n", 400)]
    [InlineData("POST /Ask HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", 400)]
    [InlineData("POST /Ask HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n", 400)]
    [InlineData("garbage\r\n\r\n", 400)]
    public async Task Malformed_requests_get_an_error_not_a_crash(string raw, int expected)
    {
        await using var c = await FakeSender.ConnectAsync(_server.Port);
        Assert.Equal(expected, (await c.RoundTripAsync(Encoding.ASCII.GetBytes(raw))).Status);
        // The server is still alive afterwards.
        await using var again = await FakeSender.ConnectAsync(_server.Port);
        Assert.Equal(200, (await again.RoundTripAsync(Wire.DiscoverRequest())).Status);
    }

    [Fact]
    public async Task Idle_keep_alive_connections_do_not_block_other_senders()
    {
        var idle = new List<FakeSender>();
        for (int i = 0; i < 5; i++) idle.Add(await FakeSender.ConnectAsync(_server.Port));
        await using var c = await FakeSender.ConnectAsync(_server.Port);
        Assert.Equal(200, (await c.RoundTripAsync(Wire.DiscoverRequest())).Status);
        foreach (FakeSender s in idle) await s.DisposeAsync();
    }

    /// <summary>
    /// Replays real decrypted sender bytes (output of tools/capture/tls13_decrypt.py) when the
    /// developer points MTDROP_PRIVATE_CAPTURE at them. Such data is private and never in the
    /// repository, so this is a no-op everywhere else.
    /// </summary>
    [Fact]
    public async Task Replays_a_private_capture_when_available()
    {
        string? dir = Environment.GetEnvironmentVariable("MTDROP_PRIVATE_CAPTURE");
        if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir)) return;
        int uploads = 0;
        foreach (string file in Directory.GetFiles(dir, "flow*-c2s.bin").Order())
        {
            byte[] bytes = await File.ReadAllBytesAsync(file);
            if (bytes.Length == 0) continue;
            string head = Encoding.ASCII.GetString(bytes, 0, Math.Min(bytes.Length, 16));
            await using var c = await FakeSender.ConnectAsync(_server.Port);
            await c.SendAsync(bytes);
            Response first;
            try { first = await c.ReadResponseAsync(3000); }
            catch (Exception e) when (e is OperationCanceledException or EndOfStreamException or IOException)
            {
                continue;                                         // capture ended inside a request body
            }
            if (head.StartsWith("POST /Discover"))
            {
                Assert.Equal(200, first.Status);
                continue;
            }
            if (!head.StartsWith("POST /Ask")) continue;
            Assert.Equal(200, first.Status);
            TransferResult result = await WaitForResultAsync();
            if (!result.Succeeded) continue;                      // a stalled attempt in the capture
            uploads++;
            Assert.Equal(200, (await c.ReadResponseAsync()).Status);
            string expected = Path.Combine(dir, "..", result.Ask.Files[0].Name);
            if (File.Exists(expected))
                Assert.Equal(await File.ReadAllBytesAsync(expected), await File.ReadAllBytesAsync(result.SavedFiles[0]));
            Assert.Equal(result.Ask.Files[0].Size, new FileInfo(result.SavedFiles[0]).Length);
        }
        Assert.True(uploads > 0, "capture contained no complete upload");
        Assert.DoesNotContain(SavedFiles(), f => f.StartsWith("._") || f.EndsWith(".part"));
    }

    [Fact]
    public void Identity_is_rsa_2048_and_persists()
    {
        string pfx = Path.Combine(_dir, "id", "identity.pfx");
        using X509Certificate2 first = ServerIdentity.LoadOrCreate(pfx);
        using X509Certificate2 second = ServerIdentity.LoadOrCreate(pfx);
        Assert.Equal(first.Thumbprint, second.Thumbprint);
        Assert.Equal(2048, first.GetRSAPublicKey()!.KeySize);
        Assert.True(first.HasPrivateKey);
        Assert.InRange((first.NotAfter - DateTime.Now).TotalDays, 360, 366);
        File.Delete(pfx);
    }
}
