using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;

namespace Mtdrop.Protocol;

/// <summary>One file announced in an /Ask request. Values come from the sender and are unverified.</summary>
public sealed record AnnouncedFile(string Name, string? Type, long Size, bool IsDirectory);

/// <summary>What the user is asked to accept. Everything here is claimed by the sender, not authenticated.</summary>
public sealed class AskRequest
{
    public required string TransferId { get; init; }
    public required string SenderName { get; init; }
    public string? SenderModel { get; init; }
    public required IReadOnlyList<AnnouncedFile> Files { get; init; }
    public required IPEndPoint Remote { get; init; }
    public long TotalSize => Files.Sum(f => f.Size);
}

public sealed class TransferResult
{
    public required AskRequest Ask { get; init; }
    public IReadOnlyList<string> SavedFiles { get; init; } = [];
    public string? Error { get; init; }
    public bool Succeeded => Error is null;
}

public sealed class AirDropServerOptions
{
    /// <summary>Name shown in the sender's share sheet.</summary>
    public required string ComputerName { get; init; }
    public string ModelName { get; init; } = "mtdrop";
    public required string DestinationDirectory { get; init; }
    public IPAddress BindAddress { get; init; } = IPAddress.IPv6Any;
    public int Port { get; init; } = 8771;

    /// <summary>If set, connections arriving on any other IPv6 interface are dropped.</summary>
    public long? RequiredScopeId { get; init; }

    /// <summary>Asks the user. Return true to accept. The token is cancelled on timeout or shutdown.</summary>
    public required Func<AskRequest, CancellationToken, Task<bool>> Consent { get; init; }

    public TimeSpan ConsentTimeout { get; init; } = TimeSpan.FromSeconds(60);
    public TimeSpan IdleTimeout { get; init; } = TimeSpan.FromSeconds(30);
    public TimeSpan UploadIdleTimeout { get; init; } = TimeSpan.FromSeconds(15);
    public TimeSpan AcceptedAskLifetime { get; init; } = TimeSpan.FromSeconds(120);
    public int MaxControlBodyBytes { get; init; } = 4 * 1024 * 1024;
    public int MaxConnections { get; init; } = 16;

    /// <summary>HTTP status for a declined /Ask. Open question, docs/PROTOCOL.md §10 item 1.</summary>
    public int DeclineStatus { get; init; } = 403;

    public Action<string>? Log { get; init; }
}

/// <summary>
/// AirDrop-compatible receiver endpoints (/Discover, /Ask, /Upload) for "Everyone" mode.
/// Implements docs/PROTOCOL.md §5–§7. Parses untrusted input: run unelevated.
/// </summary>
public sealed class AirDropServer : IAsyncDisposable
{
    private readonly AirDropServerOptions _o;
    private readonly X509Certificate2 _certificate;
    private readonly TcpListener _listener;
    private readonly CancellationTokenSource _stop = new();
    private readonly SemaphoreSlim _slots;
    private Task? _acceptLoop;
    private int _active;

    public event Action<TransferResult>? TransferFinished;

    public AirDropServer(AirDropServerOptions options, X509Certificate2 certificate)
    {
        _o = options;
        _certificate = certificate;
        _slots = new SemaphoreSlim(options.MaxConnections);
        _listener = new TcpListener(options.BindAddress, options.Port);
        if (options.BindAddress.Equals(IPAddress.IPv6Any)) _listener.Server.DualMode = false;
    }

    public int Port => ((IPEndPoint)_listener.LocalEndpoint).Port;

    public void Start()
    {
        _listener.Start(backlog: 16);
        _acceptLoop = Task.Run(AcceptLoopAsync);
    }

    private void Log(string message) => _o.Log?.Invoke(message);

    private async Task AcceptLoopAsync()
    {
        while (!_stop.IsCancellationRequested)
        {
            TcpClient client;
            try
            {
                client = await _listener.AcceptTcpClientAsync(_stop.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException) { break; }
            catch (SocketException) when (_stop.IsCancellationRequested) { break; }
            catch (SocketException e) { Log($"accept failed: {e.SocketErrorCode}"); continue; }

            if (!_slots.Wait(0))
            {
                client.Dispose();
                continue;
            }
            Interlocked.Increment(ref _active);
            _ = Task.Run(async () =>
            {
                try { await HandleConnectionAsync(client).ConfigureAwait(false); }
                catch (Exception e) { Log($"connection handler crashed: {e.GetType().Name}: {e.Message}"); }
                finally
                {
                    client.Dispose();
                    _slots.Release();
                    Interlocked.Decrement(ref _active);
                }
            });
        }
    }

    private sealed class ConnectionState
    {
        public AskRequest? AcceptedAsk;
        public DateTime AcceptedAt;
    }

    private async Task HandleConnectionAsync(TcpClient client)
    {
        CancellationToken ct = _stop.Token;
        var remote = (IPEndPoint)client.Client.RemoteEndPoint!;
        if (_o.RequiredScopeId is { } scope && remote.Address.ScopeId != scope)
        {
            Log("dropped a connection from another interface");
            return;
        }
        client.NoDelay = true;
        // Repeat our ACK while the sender is between its slots (docs/PROTOCOL.md §7.8; Foglio24 tail-loss fix).
        client.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.KeepAlive, true);
        try
        {
            client.Client.SetSocketOption(SocketOptionLevel.Tcp, SocketOptionName.TcpKeepAliveTime, 1);
            client.Client.SetSocketOption(SocketOptionLevel.Tcp, SocketOptionName.TcpKeepAliveInterval, 1);
        }
        catch (SocketException) { }

        await using var tls = new SslStream(client.GetStream(), leaveInnerStreamOpen: false);
        try
        {
            using var hs = CancellationTokenSource.CreateLinkedTokenSource(ct);
            hs.CancelAfter(_o.IdleTimeout);
            await tls.AuthenticateAsServerAsync(new SslServerAuthenticationOptions
            {
                ServerCertificate = _certificate,
                ClientCertificateRequired = false,
                EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13,
            }, hs.Token).ConfigureAwait(false);
        }
        catch (Exception e) when (e is AuthenticationException or IOException or OperationCanceledException or SocketException)
        {
            // Senders open spare connections and abandon some mid-handshake; not an error.
            return;
        }

        var reader = new ConnectionReader(tls, _o.IdleTimeout);
        var state = new ConnectionState();
        try
        {
            while (!ct.IsCancellationRequested)
            {
                HttpRequestHead? head = await reader.ReadRequestHeadAsync(ct).ConfigureAwait(false);
                if (head is null) return;
                bool keepOpen = await DispatchAsync(head, reader, tls, state, remote, ct).ConfigureAwait(false);
                if (!keepOpen) return;
            }
        }
        catch (ProtocolFormatException e)
        {
            Log($"bad request: {e.Message}");
            await TryRespondAsync(tls, 400, "Bad Request").ConfigureAwait(false);
        }
        catch (TimeoutException) { }
        catch (Exception e) when (e is IOException or SocketException or EndOfStreamException or OperationCanceledException)
        {
            // Senders end keep-alive sessions with a reset; normal.
        }
    }

    private async Task TryRespondAsync(Stream tls, int status, string reason)
    {
        try
        {
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));
            await HttpResponse.WriteAsync(tls, status, reason, ReadOnlyMemory<byte>.Empty, close: true, cts.Token).ConfigureAwait(false);
        }
        catch (Exception e) when (e is IOException or SocketException or OperationCanceledException or ObjectDisposedException) { }
    }

    /// <returns>true to keep the connection open for another request.</returns>
    private async Task<bool> DispatchAsync(HttpRequestHead head, ConnectionReader reader, Stream tls,
        ConnectionState state, IPEndPoint remote, CancellationToken ct)
    {
        if (head.Method != "POST")
        {
            await HttpResponse.WriteAsync(tls, 405, "Method Not Allowed", default, close: true, ct).ConfigureAwait(false);
            return false;
        }
        switch (head.Path)
        {
            case "/Discover":
            {
                await ReadControlBodyAsync(head, reader, tls, ct).ConfigureAwait(false);
                byte[] body = BinaryPlist.Write(new Dictionary<string, object?>
                {
                    ["ReceiverComputerName"] = _o.ComputerName,
                    ["ReceiverModelName"] = _o.ModelName,
                    ["ReceiverMediaCapabilities"] = Encoding.UTF8.GetBytes("{\"Version\": 1}"),
                });
                await HttpResponse.WriteAsync(tls, 200, "OK", body, close: false, ct).ConfigureAwait(false);
                return true;
            }
            case "/Ask":
                return await HandleAskAsync(head, reader, tls, state, remote, ct).ConfigureAwait(false);
            case "/Upload":
                await HandleUploadAsync(head, reader, tls, state, ct).ConfigureAwait(false);
                return false;
            default:
                await HttpResponse.WriteAsync(tls, 400, "Bad Request", default, close: true, ct).ConfigureAwait(false);
                return false;
        }
    }

    private async Task<byte[]> ReadControlBodyAsync(HttpRequestHead head, ConnectionReader reader, Stream tls, CancellationToken ct)
    {
        if (head.ExpectsContinue) await HttpResponse.WriteContinueAsync(tls, ct).ConfigureAwait(false);
        await using Stream body = reader.OpenBody(head);
        using var ms = new MemoryStream();
        byte[] buf = new byte[32 * 1024];
        int n;
        while ((n = await body.ReadAsync(buf, ct).ConfigureAwait(false)) > 0)
        {
            if (ms.Length + n > _o.MaxControlBodyBytes) throw new ProtocolFormatException("control body too large");
            ms.Write(buf, 0, n);
        }
        return ms.ToArray();
    }

    private static AskRequest ParseAsk(byte[] body, IPEndPoint remote)
    {
        Dictionary<string, object?> p = BinaryPlist.ReadDictionary(body);
        string? id = (p.GetValueOrDefault("TransferID") as Dictionary<string, object?>)?.GetValueOrDefault("id") as string;
        if (id is null || !Guid.TryParse(id, out _)) throw new ProtocolFormatException("missing or invalid TransferID");
        bool isLink = p.GetValueOrDefault("Items") is List<object?> { Count: > 0 }
                      || (p.GetValueOrDefault("TransferType") as Dictionary<string, object?>)?.ContainsKey("links") == true;
        if (isLink) throw new NotSupportedException("link shares are not supported");
        if (p.GetValueOrDefault("Files") is not List<object?> { Count: > 0 and <= 512 } files)
            throw new ProtocolFormatException("no files announced");
        var announced = new List<AnnouncedFile>();
        foreach (object? f in files)
        {
            if (f is not Dictionary<string, object?> fd || fd.GetValueOrDefault("FileName") is not string rawName)
                throw new ProtocolFormatException("bad file entry");
            string last = rawName.Replace('\\', '/').Split('/', StringSplitOptions.RemoveEmptyEntries).LastOrDefault() ?? "";
            string name = SafeName.Clean(last) ?? "file";
            long size = fd.GetValueOrDefault("FileSize") is long s and >= 0 ? s : 0;
            bool isDir = fd.GetValueOrDefault("FileIsDirectory") is true;
            announced.Add(new AnnouncedFile(name, fd.GetValueOrDefault("FileType") as string, size, isDir));
        }
        return new AskRequest
        {
            TransferId = id,
            SenderName = CleanDisplay(p.GetValueOrDefault("SenderComputerName") as string) ?? "Unknown device",
            SenderModel = CleanDisplay(p.GetValueOrDefault("SenderModelName") as string),
            Files = announced,
            Remote = remote,
        };
    }

    /// <summary>Sender-supplied text for display: no control or direction-override characters, bounded length.</summary>
    private static string? CleanDisplay(string? s)
    {
        if (string.IsNullOrWhiteSpace(s)) return null;
        var sb = new StringBuilder();
        foreach (char c in s)
        {
            if (char.IsControl(c) || c is (>= '​' and <= '‏') or (>= '‪' and <= '‮') or (>= '⁦' and <= '⁩')) continue;
            sb.Append(c);
            if (sb.Length >= 64) break;
        }
        return sb.Length == 0 ? null : sb.ToString();
    }

    private async Task<bool> HandleAskAsync(HttpRequestHead head, ConnectionReader reader, Stream tls,
        ConnectionState state, IPEndPoint remote, CancellationToken ct)
    {
        byte[] body = await ReadControlBodyAsync(head, reader, tls, ct).ConfigureAwait(false);
        AskRequest ask;
        try
        {
            ask = ParseAsk(body, remote);
        }
        catch (NotSupportedException e)
        {
            Log($"declined: {e.Message}");
            await HttpResponse.WriteAsync(tls, _o.DeclineStatus, "Declined", default, close: true, ct).ConfigureAwait(false);
            return false;
        }

        bool accepted = false;
        using (var cts = CancellationTokenSource.CreateLinkedTokenSource(ct))
        {
            cts.CancelAfter(_o.ConsentTimeout);
            try { accepted = await _o.Consent(ask, cts.Token).ConfigureAwait(false); }
            catch (OperationCanceledException) when (!ct.IsCancellationRequested) { accepted = false; }
        }
        if (!accepted)
        {
            await HttpResponse.WriteAsync(tls, _o.DeclineStatus, "Declined", default, close: true, ct).ConfigureAwait(false);
            return false;
        }
        state.AcceptedAsk = ask;
        state.AcceptedAt = DateTime.UtcNow;
        byte[] reply = BinaryPlist.Write(new Dictionary<string, object?>
        {
            ["ReceiverComputerName"] = _o.ComputerName,
            ["ReceiverModelName"] = _o.ModelName,
        });
        await HttpResponse.WriteAsync(tls, 200, "OK", reply, close: false, ct).ConfigureAwait(false);
        return true;
    }

    private async Task HandleUploadAsync(HttpRequestHead head, ConnectionReader reader, Stream tls,
        ConnectionState state, CancellationToken ct)
    {
        // Consent binding: one upload per accepted ask, same connection, same TransferID.
        AskRequest? ask = state.AcceptedAsk;
        state.AcceptedAsk = null;
        if (ask is null || DateTime.UtcNow - state.AcceptedAt > _o.AcceptedAskLifetime
            || !string.Equals(head.Header("TransferID"), ask.TransferId, StringComparison.OrdinalIgnoreCase))
        {
            Log("refused an upload without a matching accepted ask");
            await HttpResponse.WriteAsync(tls, 403, "Forbidden", default, close: true, ct).ConfigureAwait(false);
            return;
        }
        if (!string.Equals(head.Header("Content-Type"), "application/x-dvzip", StringComparison.OrdinalIgnoreCase))
        {
            await HttpResponse.WriteAsync(tls, 415, "Unsupported Media Type", default, close: true, ct).ConfigureAwait(false);
            Finish(ask, [], "unsupported upload format");
            return;
        }
        if (!head.IsChunked && head.Header("Content-Length") is null)
        {
            await HttpResponse.WriteAsync(tls, 411, "Length Required", default, close: true, ct).ConfigureAwait(false);
            Finish(ask, [], "upload without body framing");
            return;
        }

        // Budget: what the sender announced, plus room for sidecar members.
        long announced = Math.Max(ask.TotalSize, long.TryParse(head.Header("TotalBytes"), out long tb) && tb > 0 ? tb : 0);
        var limits = new UploadLimits
        {
            MaxTotalBytes = announced > 0 ? announced + 16L * 1024 * 1024 : new UploadLimits().MaxTotalBytes,
        };
        if (head.ExpectsContinue) await HttpResponse.WriteContinueAsync(tls, ct).ConfigureAwait(false);
        reader.IdleTimeout = _o.UploadIdleTimeout;
        try
        {
            IReadOnlyList<string> saved;
            await using (Stream body = reader.OpenBody(head))
                saved = await UploadReceiver.ReceiveAsync(body, _o.DestinationDirectory, limits, ct).ConfigureAwait(false);
            await HttpResponse.WriteAsync(tls, 200, "OK", default, close: true, ct).ConfigureAwait(false);
            Finish(ask, saved, null);
        }
        catch (Exception e) when (e is ProtocolFormatException or EndOfStreamException or TimeoutException or IOException or SocketException)
        {
            string reason = e switch
            {
                TimeoutException => "sender went quiet",
                ProtocolFormatException => "invalid data: " + e.Message,
                _ => "connection lost",
            };
            Finish(ask, [], reason);
            await TryRespondAsync(tls, e is ProtocolFormatException ? 422 : 408, "Upload Failed").ConfigureAwait(false);
        }
    }

    private void Finish(AskRequest ask, IReadOnlyList<string> saved, string? error)
    {
        Log(error is null ? $"received {saved.Count} file(s)" : $"transfer failed: {error}");
        try { TransferFinished?.Invoke(new TransferResult { Ask = ask, SavedFiles = saved, Error = error }); }
        catch (Exception e) { Log($"TransferFinished handler threw: {e.Message}"); }
    }

    public async ValueTask DisposeAsync()
    {
        _stop.Cancel();
        _listener.Stop();
        if (_acceptLoop is not null) await _acceptLoop.ConfigureAwait(false);
        for (int i = 0; i < 100 && Volatile.Read(ref _active) > 0; i++) await Task.Delay(20).ConfigureAwait(false);
        _stop.Dispose();
    }
}

/// <summary>Self-signed TLS identity for the receiver (docs/PROTOCOL.md §5).</summary>
public static class ServerIdentity
{
    private static readonly TimeSpan Validity = TimeSpan.FromDays(365);
    private static readonly TimeSpan RenewBefore = TimeSpan.FromDays(30);

    public static X509Certificate2 Create()
    {
        using RSA rsa = RSA.Create(2048);
        var req = new CertificateRequest("CN=mtdrop receiver", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        req.CertificateExtensions.Add(new X509BasicConstraintsExtension(false, false, 0, true));
        req.CertificateExtensions.Add(new X509KeyUsageExtension(
            X509KeyUsageFlags.DigitalSignature | X509KeyUsageFlags.KeyEncipherment, true));
        DateTimeOffset now = DateTimeOffset.UtcNow;
        using X509Certificate2 ephemeral = req.CreateSelfSigned(now.AddMinutes(-5), now.Add(Validity));
        // Re-import so the private key is usable by the Windows TLS stack.
        return X509CertificateLoader.LoadPkcs12(ephemeral.Export(X509ContentType.Pfx), null, X509KeyStorageFlags.Exportable);
    }

    /// <summary>Loads the identity from <paramref name="pfxPath"/>, creating or renewing it when missing, unreadable or near expiry.</summary>
    public static X509Certificate2 LoadOrCreate(string pfxPath)
    {
        if (File.Exists(pfxPath))
        {
            try
            {
                X509Certificate2 existing = X509CertificateLoader.LoadPkcs12FromFile(pfxPath, null, X509KeyStorageFlags.Exportable);
                if (existing.HasPrivateKey && existing.NotAfter.ToUniversalTime() - DateTime.UtcNow > RenewBefore) return existing;
                existing.Dispose();
            }
            catch (CryptographicException) { }
        }
        X509Certificate2 created = Create();
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(pfxPath))!);
        File.WriteAllBytes(pfxPath, created.Export(X509ContentType.Pfx));
        return created;
    }
}
