using System.Net;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using Mtdrop.Protocol;

namespace Mtdrop.Protocol.Tests;

/// <summary>M8.1 (ADR-0005): the sender's encoders, the DNS-SD cache and the client against our own receiver.</summary>
public sealed class SenderTests : IAsyncLifetime
{
    private static readonly X509Certificate2 ServerCert = ServerIdentity.Create();
    private static readonly X509Certificate2 ClientCert = ServerIdentity.Create();
    private readonly string _root = Path.Combine(Path.GetTempPath(), "mtdrop-send-" + Guid.NewGuid().ToString("N"));
    private string Inbox => Path.Combine(_root, "inbox");
    private string Outbox => Path.Combine(_root, "outbox");
    private readonly List<TransferResult> _results = [];
    private readonly SemaphoreSlim _finished = new(0);
    private AskRequest? _lastAsk;
    private bool _accept = true;
    private AirDropServer _server = null!;

    public Task InitializeAsync()
    {
        Directory.CreateDirectory(Inbox);
        Directory.CreateDirectory(Outbox);
        _server = new AirDropServer(new AirDropServerOptions
        {
            ComputerName = "TEST-PHONE",
            ModelName = "iPhone",
            DestinationDirectory = Inbox,
            BindAddress = IPAddress.IPv6Loopback,
            Port = 0,
            Consent = (ask, _) => { _lastAsk = ask; return Task.FromResult(_accept); },
            UploadIdleTimeout = TimeSpan.FromSeconds(10),
            ConsentTimeout = TimeSpan.FromSeconds(10),
        }, ServerCert);
        _server.TransferFinished += r => { lock (_results) _results.Add(r); _finished.Release(); };
        _server.Start();
        return Task.CompletedTask;
    }

    public async Task DisposeAsync()
    {
        await _server.DisposeAsync();
        try { Directory.Delete(_root, recursive: true); } catch (IOException) { }
    }

    private IPEndPoint Receiver => new(IPAddress.IPv6Loopback, _server.Port);

    private static AirDropClient NewClient(List<string>? log = null) => new(new AirDropClientOptions
    {
        SenderName = "TEST-PC",
        SenderId = "0123456789ab",
        ConnectTimeout = TimeSpan.FromSeconds(5),
        AskTimeout = TimeSpan.FromSeconds(15),
        UploadIdleTimeout = TimeSpan.FromSeconds(10),
        Log = log is null ? null : m => { lock (log) log.Add(m); },
    }, ClientCert);

    private string NewFile(string name, byte[] content)
    {
        string path = Path.Combine(Outbox, name);
        File.WriteAllBytes(path, content);
        return path;
    }

    // ---- encoders --------------------------------------------------------------------------------------

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(131072)]          // exactly one dvzip block
    [InlineData(400_000)]         // spans blocks
    public async Task Cpio_in_dvzip_reads_back_with_our_receiver_decoders(int size)
    {
        byte[] random = RandomNumberGenerator.GetBytes(size);                   // stored blocks
        byte[] text = Encoding.ASCII.GetBytes(string.Concat(Enumerable.Repeat("compressible ", size / 13 + 1)))[..size];   // zlib blocks
        using var wire = new MemoryStream();
        var dvzip = new DvzipWriter(wire);
        var cpio = new CpioOdcWriter(dvzip);
        await cpio.DirectoryAsync(".", default);
        await cpio.FileAsync("./a.jpg", new MemoryStream(random), random.Length, DateTimeOffset.FromUnixTimeSeconds(1_700_000_000), default);
        await cpio.FileAsync("./b.txt", new MemoryStream(text), text.Length, DateTimeOffset.UnixEpoch, default);
        await cpio.FinishAsync(default);
        await dvzip.CompleteAsync(default);
        Assert.Equal(0, cpio.BytesWritten % 512);

        wire.Position = 0;
        var reader = new CpioOdcReader(new DvzipStream(wire));
        var members = new List<(CpioEntry Entry, byte[] Data)>();
        while (await reader.NextAsync(default) is { } entry)
        {
            using var ms = new MemoryStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = await reader.ReadDataAsync(buf, default)) > 0) ms.Write(buf, 0, n);
            members.Add((entry, ms.ToArray()));
        }
        Assert.Equal([".", "./a.jpg", "./b.txt"], members.Select(m => m.Entry.Name));
        Assert.Equal(CpioEntryKind.Directory, members[0].Entry.Kind);
        Assert.Equal(random, members[1].Data);
        Assert.Equal(text, members[2].Data);
    }

    [Fact]
    public async Task Cpio_writer_refuses_a_source_shorter_than_announced()
    {
        var cpio = new CpioOdcWriter(new MemoryStream());
        await Assert.ThrowsAsync<IOException>(() => cpio.FileAsync("./x", new MemoryStream(new byte[10]), 20, DateTimeOffset.UnixEpoch, default));
    }

    [Fact]
    public async Task Chunked_writer_round_trips_through_our_chunked_reader()
    {
        byte[] data = RandomNumberGenerator.GetBytes(ChunkedWriteStream.ChunkSize * 2 + 123);
        using var wire = new MemoryStream();
        var w = new ChunkedWriteStream(wire);
        await w.WriteAsync(data.AsMemory(0, 7));
        await w.WriteAsync(data.AsMemory(7));
        await w.CompleteAsync(default);
        wire.Position = 0;
        var body = new ChunkedBodyStream(new ConnectionReader(wire, TimeSpan.FromSeconds(5)));
        using var back = new MemoryStream();
        await body.CopyToAsync(back);
        Assert.Equal(data, back.ToArray());
        Assert.True(body.Terminated);
    }

    // ---- DNS-SD ----------------------------------------------------------------------------------------

    [Fact]
    public void Service_cache_builds_an_endpoint_from_a_receiver_announcement()
    {
        var records = new AirDropServiceRecords { ServiceId = "a1b2c3d4e5f6", Address = IPAddress.Parse("fe80::1234"), Port = 8770 };
        var cache = new AirDropServiceCache();

        IReadOnlyList<AirDropServiceEndpoint> found = cache.Add(records.BuildAnnouncement(), scopeId: 58);

        AirDropServiceEndpoint e = Assert.Single(found);
        Assert.Equal("a1b2c3d4e5f6", e.ServiceId);
        Assert.Equal(8770, e.EndPoint.Port);
        Assert.Equal(58, e.EndPoint.Address.ScopeId);
        Assert.Equal(IPAddress.Parse("fe80::1234"), new IPAddress(e.EndPoint.Address.GetAddressBytes()));
        Assert.Equal("flags=136", e.Txt);
        Assert.Empty(cache.Add(records.BuildAnnouncement(), 58));               // nothing new the second time

        cache.Add(records.BuildAnnouncement(goodbye: true), 58);
        Assert.Empty(cache.Snapshot(58));
    }

    [Fact]
    public void Our_query_is_one_our_responder_answers_and_the_cache_ignores()
    {
        byte[] query = AirDropServiceCache.BuildQuery();
        var records = new AirDropServiceRecords { ServiceId = "a1b2c3d4e5f6", Address = IPAddress.IPv6Loopback, Port = 8770 };
        Assert.True(records.IsQueryForUs(query));
        Assert.Empty(new AirDropServiceCache().Add(query, 1));
    }

    [Theory]
    [InlineData(new byte[] { 0x00, 0x00, 0x84, 0x00, 0x00, 0x00, 0x00, 0x05 })]                  // short
    [InlineData(new byte[] { 0x00, 0x00, 0x84, 0x00, 0x00, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0xC0, 0x0C })]  // pointer loop
    public void Service_cache_ignores_malformed_packets(byte[] packet)
    {
        Assert.Empty(new AirDropServiceCache().Add(packet, 1));
    }

    // ---- client against our own receiver -----------------------------------------------------------------

    [Fact]
    public async Task Discover_returns_the_receiver_name()
    {
        (string Name, string? Model)? who = await NewClient().DiscoverAsync(Receiver, default);
        Assert.Equal(("TEST-PHONE", "iPhone"), who);
    }

    [Fact]
    public async Task Sends_a_photo_that_arrives_intact()
    {
        byte[] photo = RandomNumberGenerator.GetBytes(300_000);
        string path = NewFile("IMG_0001.JPG", photo);
        long reported = 0;
        var log = new List<string>();

        SendResult result = await NewClient(log).SendAsync(Receiver, [OutgoingFile.FromPath(path)],
            new SyncProgress(n => Interlocked.Add(ref reported, n)), default);

        Assert.Equal(SendOutcome.Delivered, result.Outcome);
        Assert.Equal(photo.Length, Interlocked.Read(ref reported));
        Assert.True(await _finished.WaitAsync(TimeSpan.FromSeconds(10)));
        Assert.Equal(photo, File.ReadAllBytes(Path.Combine(Inbox, "IMG_0001.JPG")));
        Assert.Equal("TEST-PC", _lastAsk!.SenderName);
        AnnouncedFile announced = Assert.Single(_lastAsk.Files);
        Assert.Equal(("IMG_0001.JPG", "public.jpeg", 300_000L), (announced.Name, announced.Type, announced.Size));
        Assert.Contains("/Ask answered 200", log);
        Assert.Contains("/Upload answered 200", log);
    }

    [Fact]
    public async Task Sends_several_files_in_one_transfer()
    {
        string a = NewFile("a.png", RandomNumberGenerator.GetBytes(1000));
        string b = NewFile("b.pdf", RandomNumberGenerator.GetBytes(200_000));

        SendResult result = await NewClient().SendAsync(Receiver, [OutgoingFile.FromPath(a), OutgoingFile.FromPath(b)], null, default);

        Assert.Equal(SendOutcome.Delivered, result.Outcome);
        Assert.True(await _finished.WaitAsync(TimeSpan.FromSeconds(10)));
        Assert.Equal(["a.png", "b.pdf"], Directory.GetFiles(Inbox).Select(Path.GetFileName).Order());
    }

    [Fact]
    public async Task A_declined_ask_uploads_nothing()
    {
        _accept = false;
        string path = NewFile("x.jpg", new byte[100]);

        SendResult result = await NewClient().SendAsync(Receiver, [OutgoingFile.FromPath(path)], null, default);

        Assert.Equal(SendOutcome.Declined, result.Outcome);
        Assert.Empty(Directory.GetFiles(Inbox));
    }

    [Fact]
    public async Task No_receiver_is_a_failure_not_an_exception()
    {
        var nobody = new IPEndPoint(IPAddress.IPv6Loopback, 1);     // nothing listens on port 1
        SendResult result = await NewClient().SendAsync(nobody, [OutgoingFile.FromPath(NewFile("y.jpg", new byte[10]))], null, default);
        Assert.Equal(SendOutcome.Failed, result.Outcome);
    }

    [Theory]
    [InlineData("IMG_1.JPG", "public.jpeg")]
    [InlineData("clip.MOV", "com.apple.quicktime-movie")]
    [InlineData("notes", "public.data")]
    public void Uniform_type_follows_the_extension(string name, string type) =>
        Assert.Equal(type, new OutgoingFile("p", name, 1, DateTimeOffset.UnixEpoch).UniformType);

    private sealed class SyncProgress(Action<long> report) : IProgress<long>
    {
        public void Report(long value) => report(value);
    }
}
