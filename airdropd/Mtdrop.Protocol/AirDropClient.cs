using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography.X509Certificates;
using System.Text;

namespace Mtdrop.Protocol;

/// <summary>A file to send. <see cref="Name"/> is what the receiver sees (no directory parts).</summary>
public sealed record OutgoingFile(string Path, string Name, long Size, DateTimeOffset Modified)
{
    public static OutgoingFile FromPath(string path)
    {
        var info = new FileInfo(path);
        if (!info.Exists) throw new FileNotFoundException("file to send not found", path);
        return new OutgoingFile(info.FullName, info.Name, info.Length, info.LastWriteTimeUtc);
    }

    /// <summary>Uniform type identifier the receiver uses to pick the app that takes the file (winawdl's table).</summary>
    public string UniformType => System.IO.Path.GetExtension(Name).ToLowerInvariant() switch
    {
        ".jpg" or ".jpeg" => "public.jpeg",
        ".png" => "public.png",
        ".heic" => "public.heic",
        ".gif" => "com.compuserve.gif",
        ".mov" => "com.apple.quicktime-movie",
        ".mp4" => "public.mpeg-4",
        ".m4v" => "com.apple.m4v-video",
        ".mp3" => "public.mp3",
        ".pdf" => "com.adobe.pdf",
        ".txt" => "public.plain-text",
        ".zip" => "public.zip-archive",
        _ => "public.data",
    };
}

public enum SendOutcome
{
    /// <summary>The receiver accepted and answered the upload with 200.</summary>
    Delivered,
    /// <summary>The receiver's owner declined (or did not answer) the /Ask.</summary>
    Declined,
    /// <summary>Anything else: no connection, TLS refused, refused upload, timeout.</summary>
    Failed,
}

public sealed record SendResult(SendOutcome Outcome, string Detail);

public sealed class AirDropClientOptions
{
    /// <summary>Shown on the receiver as the sender (self-reported; the receiver cannot verify it).</summary>
    public required string SenderName { get; init; }
    public string SenderModel { get; init; } = "mtdrop";
    /// <summary>12 hex characters; the receiver id this machine also announces.</summary>
    public required string SenderId { get; init; }
    public TimeSpan ConnectTimeout { get; init; } = TimeSpan.FromSeconds(10);
    /// <summary>How long the receiver's owner has to accept (winawdl: 90 s).</summary>
    public TimeSpan AskTimeout { get; init; } = TimeSpan.FromSeconds(90);
    /// <summary>Upload aborted when the receiver accepts nothing for this long.</summary>
    public TimeSpan UploadIdleTimeout { get; init; } = TimeSpan.FromSeconds(30);
    public Action<string>? Log { get; init; }
}

/// <summary>
/// AirDrop sender requests (docs/PROTOCOL.md §9, ADR-0005), shaped like the iPhone's own: only
/// <c>User-Agent</c>, <c>Connection</c> and chunked bodies; <c>/Ask</c> announces a <c>TransferID</c> that
/// <c>/Upload</c> repeats on the same connection (jedbillyb/airdrop-mt7921 FINDINGS §33, §37). The receiver's
/// certificate is self-signed and not checked (it cannot be, in "Everyone" mode); ours is shown as a client
/// certificate, as Apple senders and winawdl do.
/// </summary>
public sealed class AirDropClient(AirDropClientOptions options, X509Certificate2 certificate)
{
    private const int MaxResponseBytes = 1024 * 1024;

    /// <summary>The receiver's display name and model from /Discover; null if it does not show itself to us.</summary>
    public async Task<(string Name, string? Model)?> DiscoverAsync(IPEndPoint receiver, CancellationToken ct)
    {
        await using Connection c = await ConnectAsync(receiver, ct).ConfigureAwait(false);
        byte[] body = BinaryPlist.Write(new Dictionary<string, object?>());
        HttpResponseHead head = await c.PostAsync("/Discover", "close", body, ct).ConfigureAwait(false);
        byte[] answer = await c.Reader.ReadResponseBodyAsync(head, MaxResponseBytes, ct).ConfigureAwait(false);
        if (head.Status != 200) return null;
        Dictionary<string, object?> plist = BinaryPlist.Read(answer) as Dictionary<string, object?>
                                            ?? throw new ProtocolFormatException("/Discover answer is not a dictionary");
        // Without ReceiverComputerName the receiver is not offering itself to us (opendrop/client.py).
        return plist.GetValueOrDefault("ReceiverComputerName") is string name && name.Length > 0
            ? (name, plist.GetValueOrDefault("ReceiverModelName") as string)
            : null;
    }

    /// <summary>Asks the receiver and, if accepted, uploads the files on the same connection.</summary>
    /// <param name="progress">Receives the number of file bytes sent since the last report.</param>
    public async Task<SendResult> SendAsync(IPEndPoint receiver, IReadOnlyList<OutgoingFile> files, IProgress<long>? progress,
        CancellationToken ct)
    {
        if (files.Count == 0) throw new ArgumentException("nothing to send", nameof(files));
        string transferId = Guid.NewGuid().ToString().ToUpperInvariant();
        long total = files.Sum(f => f.Size);
        try
        {
            await using Connection c = await ConnectAsync(receiver, ct).ConfigureAwait(false);

            // ---- /Ask: wait for the owner's decision ------------------------------------------------------
            c.Reader.IdleTimeout = options.AskTimeout;
            HttpResponseHead ask = await c.PostAsync("/Ask", "keep-alive", AskBody(transferId, files), ct).ConfigureAwait(false);
            await c.Reader.ReadResponseBodyAsync(ask, MaxResponseBytes, ct).ConfigureAwait(false);
            Log($"/Ask answered {ask.Status}");
            if (ask.Status is 401 or 403) return new SendResult(SendOutcome.Declined, $"declined ({ask.Status})");
            if (ask.Status != 200) return new SendResult(SendOutcome.Failed, $"/Ask answered {ask.Status} {ask.Reason}");
            if (ask.Closes) return new SendResult(SendOutcome.Failed, "the receiver closed the connection after accepting");

            // ---- /Upload: dvzip(cpio) streamed from disk, chunked -----------------------------------------
            c.Reader.IdleTimeout = options.UploadIdleTimeout;
            Log($"/Upload: {files.Count} file(s), {total} bytes");
            await c.WriteHeadAsync("/Upload", "keep-alive",
                $"TotalBytes: {total}\r\nContent-Type: application/x-dvzip\r\nTransferID: {transferId}\r\n", ct).ConfigureAwait(false);
            var chunked = new ChunkedWriteStream(c.Tls);
            var dvzip = new DvzipWriter(chunked);
            var cpio = new CpioOdcWriter(dvzip);
            using (var idle = new IdleGuard(options.UploadIdleTimeout, ct))
            {
                var counted = new ActionProgress(n => { idle.Touch(); progress?.Report(n); });
                await cpio.DirectoryAsync(".", idle.Token).ConfigureAwait(false);
                foreach (OutgoingFile f in files)
                {
                    await using FileStream source = new(f.Path, FileMode.Open, FileAccess.Read, FileShare.Read, 64 * 1024, useAsync: true);
                    if (source.Length != f.Size) throw new IOException($"{f.Name} changed since it was chosen");
                    await cpio.FileAsync("./" + f.Name, source, f.Size, f.Modified, idle.Token, counted).ConfigureAwait(false);
                }
                await cpio.FinishAsync(idle.Token).ConfigureAwait(false);
                await dvzip.CompleteAsync(idle.Token).ConfigureAwait(false);
                await chunked.CompleteAsync(idle.Token).ConfigureAwait(false);
            }
            HttpResponseHead upload = await c.Reader.ReadResponseHeadAsync(ct).ConfigureAwait(false);
            await c.Reader.ReadResponseBodyAsync(upload, MaxResponseBytes, ct).ConfigureAwait(false);
            Log($"/Upload answered {upload.Status}");
            return upload.Status == 200
                ? new SendResult(SendOutcome.Delivered, $"{files.Count} file(s), {total} bytes")
                : new SendResult(SendOutcome.Failed, $"the receiver refused the upload ({upload.Status} {upload.Reason})");
        }
        catch (OperationCanceledException) when (ct.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception e) when (e is IOException or SocketException or AuthenticationException or TimeoutException
                                  or ProtocolFormatException or OperationCanceledException)
        {
            Log($"send failed: {e.GetType().Name}: {e.Message}");
            return new SendResult(SendOutcome.Failed, e is AuthenticationException
                ? "the receiver refused the secure connection (is it set to Everyone?)"
                : e.Message);
        }
    }

    private byte[] AskBody(string transferId, IReadOnlyList<OutgoingFile> files) =>
        BinaryPlist.Write(new Dictionary<string, object?>
        {
            ["TransferID"] = new Dictionary<string, object?> { ["id"] = transferId },
            ["TransferType"] = new Dictionary<string, object?> { ["files"] = new Dictionary<string, object?>() },
            ["SenderID"] = options.SenderId,
            ["BundleID"] = "com.apple.finder",
            ["SenderComputerName"] = options.SenderName,
            ["SenderModelName"] = options.SenderModel,
            ["Items"] = new List<object?>(),
            ["Files"] = files.Select(f => (object?)new Dictionary<string, object?>
            {
                ["FileName"] = f.Name,
                ["FileType"] = f.UniformType,
                ["FileSize"] = f.Size,
                ["FileBomPath"] = "./" + f.Name,
                ["FileIsDirectory"] = false,
                ["ConvertMediaFormats"] = false,
            }).ToList(),
            ["ConvertMediaFormats"] = false,
        });

    private async Task<Connection> ConnectAsync(IPEndPoint receiver, CancellationToken ct)
    {
        var socket = new Socket(receiver.AddressFamily, SocketType.Stream, ProtocolType.Tcp) { NoDelay = true };
        try
        {
            using (var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct))
            {
                timeout.CancelAfter(options.ConnectTimeout);
                try { await socket.ConnectAsync(receiver, timeout.Token).ConfigureAwait(false); }
                catch (OperationCanceledException) when (!ct.IsCancellationRequested) { throw new TimeoutException("the receiver did not answer the connection"); }
            }
            var tls = new SslStream(new NetworkStream(socket, ownsSocket: true), leaveInnerStreamOpen: false);
            using (var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct))
            {
                timeout.CancelAfter(options.ConnectTimeout);
                await tls.AuthenticateAsClientAsync(new SslClientAuthenticationOptions
                {
                    TargetHost = "",
                    ClientCertificates = [certificate],
                    // Self-signed, nothing to check it against in "Everyone" mode (PROTOCOL §5).
                    RemoteCertificateValidationCallback = (_, _, _, _) => true,
                    EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13,
                }, timeout.Token).ConfigureAwait(false);
            }
            return new Connection(tls, new ConnectionReader(tls, options.ConnectTimeout));
        }
        catch
        {
            socket.Dispose();
            throw;
        }
    }

    private void Log(string message) => options.Log?.Invoke(message);

    private sealed class Connection(SslStream tls, ConnectionReader reader) : IAsyncDisposable
    {
        public SslStream Tls { get; } = tls;
        public ConnectionReader Reader { get; } = reader;

        public async Task WriteHeadAsync(string path, string connection, string extraHeaders, CancellationToken ct)
        {
            byte[] head = Encoding.ASCII.GetBytes(
                $"POST {path} HTTP/1.1\r\nUser-Agent: AirDrop/1.0\r\n{extraHeaders}Connection: {connection}\r\nTransfer-Encoding: chunked\r\n\r\n");
            await Tls.WriteAsync(head, ct).ConfigureAwait(false);
        }

        public async Task<HttpResponseHead> PostAsync(string path, string connection, byte[] body, CancellationToken ct)
        {
            await WriteHeadAsync(path, connection, "", ct).ConfigureAwait(false);
            var chunked = new ChunkedWriteStream(Tls);
            await chunked.WriteAsync(body, ct).ConfigureAwait(false);
            await chunked.CompleteAsync(ct).ConfigureAwait(false);
            return await Reader.ReadResponseHeadAsync(ct).ConfigureAwait(false);
        }

        public async ValueTask DisposeAsync() => await Tls.DisposeAsync().ConfigureAwait(false);
    }

    /// <summary>Cancels when nothing moved for the idle time, or when the outer token cancels.</summary>
    private sealed class IdleGuard : IDisposable
    {
        private readonly CancellationTokenSource _cts;
        private readonly TimeSpan _idle;

        public IdleGuard(TimeSpan idle, CancellationToken outer)
        {
            _idle = idle;
            _cts = CancellationTokenSource.CreateLinkedTokenSource(outer);
            _cts.CancelAfter(idle);
        }

        public CancellationToken Token => _cts.Token;

        public void Touch()
        {
            try { _cts.CancelAfter(_idle); } catch (ObjectDisposedException) { }
        }

        public void Dispose() => _cts.Dispose();
    }

    /// <summary>Reports synchronously on the caller's thread (Progress&lt;T&gt; would post to a sync context).</summary>
    private sealed class ActionProgress(Action<long> report) : IProgress<long>
    {
        public void Report(long value) => report(value);
    }
}
