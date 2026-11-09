using System.Globalization;
using System.Text;

namespace Mtdrop.Protocol;

/// <summary>Request line and headers of one HTTP/1.1 request.</summary>
public sealed class HttpRequestHead
{
    public required string Method { get; init; }
    public required string Path { get; init; }
    public required Dictionary<string, string> Headers { get; init; }

    public string? Header(string name) => Headers.TryGetValue(name, out string? v) ? v : null;

    public bool IsChunked =>
        string.Equals(Header("Transfer-Encoding")?.Trim(), "chunked", StringComparison.OrdinalIgnoreCase);

    public bool ExpectsContinue =>
        string.Equals(Header("Expect")?.Trim(), "100-continue", StringComparison.OrdinalIgnoreCase);
}

/// <summary>
/// Buffered reader over a connection stream with an idle timeout per read.
/// Minimal by design: AirDrop senders omit the Host header and withhold the final
/// chunk, so a general-purpose HTTP server cannot be used (docs/PROTOCOL.md §6).
/// </summary>
public sealed class ConnectionReader(Stream stream, TimeSpan idleTimeout)
{
    public const int MaxHeaderBytes = 16 * 1024;
    private const int MaxHeaderCount = 64;
    private readonly byte[] _buf = new byte[64 * 1024];
    private int _start, _end;

    public TimeSpan IdleTimeout { get; set; } = idleTimeout;

    /// <summary>Fills the buffer if empty. Returns false on end of stream.</summary>
    private async ValueTask<bool> FillAsync(CancellationToken ct)
    {
        if (_start < _end) return true;
        _start = _end = 0;
        using var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        cts.CancelAfter(IdleTimeout);
        int n;
        try
        {
            n = await stream.ReadAsync(_buf, cts.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (!ct.IsCancellationRequested)
        {
            throw new TimeoutException("peer went quiet");
        }
        _end = n;
        return n > 0;
    }

    public async ValueTask<int> ReadAsync(Memory<byte> dest, CancellationToken ct)
    {
        if (dest.Length == 0 || !await FillAsync(ct).ConfigureAwait(false)) return 0;
        int n = Math.Min(dest.Length, _end - _start);
        _buf.AsMemory(_start, n).CopyTo(dest);
        _start += n;
        return n;
    }

    public async ValueTask ReadExactlyAsync(Memory<byte> dest, CancellationToken ct)
    {
        while (dest.Length > 0)
        {
            int n = await ReadAsync(dest, ct).ConfigureAwait(false);
            if (n == 0) throw new EndOfStreamException();
            dest = dest[n..];
        }
    }

    /// <summary>Reads one CRLF-terminated line (without the CRLF). Null on clean end of stream before any byte.</summary>
    public async ValueTask<string?> ReadLineAsync(int maxLength, CancellationToken ct)
    {
        var sb = new StringBuilder();
        while (true)
        {
            if (!await FillAsync(ct).ConfigureAwait(false))
                return sb.Length == 0 ? null : throw new EndOfStreamException();
            byte b = _buf[_start++];
            if (b == (byte)'\n')
            {
                if (sb.Length == 0 || sb[^1] != '\r') throw new ProtocolFormatException("bare LF in HTTP line");
                sb.Length--;
                return sb.ToString();
            }
            if (sb.Length >= maxLength) throw new ProtocolFormatException("HTTP line too long");
            sb.Append((char)b);
        }
    }

    /// <summary>Reads a request head. Returns null if the peer closed the connection between requests.</summary>
    public async ValueTask<HttpRequestHead?> ReadRequestHeadAsync(CancellationToken ct)
    {
        string? line = await ReadLineAsync(MaxHeaderBytes, ct).ConfigureAwait(false);
        if (line is null) return null;
        string[] parts = line.Split(' ');
        if (parts.Length != 3 || parts[2] is not ("HTTP/1.1" or "HTTP/1.0") || parts[0].Length == 0 || !parts[1].StartsWith('/'))
            throw new ProtocolFormatException("bad request line");
        var headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        int total = line.Length;
        while (true)
        {
            string h = await ReadLineAsync(MaxHeaderBytes, ct).ConfigureAwait(false) ?? throw new EndOfStreamException();
            if (h.Length == 0) break;
            total += h.Length;
            if (total > MaxHeaderBytes || headers.Count >= MaxHeaderCount) throw new ProtocolFormatException("request head too large");
            int colon = h.IndexOf(':');
            if (colon <= 0 || h[0] is ' ' or '\t') throw new ProtocolFormatException("bad header line");
            string name = h[..colon];
            if (name.AsSpan().IndexOfAny(' ', '\t') >= 0) throw new ProtocolFormatException("bad header name");
            if (!headers.TryAdd(name, h[(colon + 1)..].Trim()))
                throw new ProtocolFormatException("duplicate header");
        }
        var head = new HttpRequestHead { Method = parts[0], Path = parts[1], Headers = headers };
        if (headers.ContainsKey("Content-Length") && headers.ContainsKey("Transfer-Encoding"))
            throw new ProtocolFormatException("both Content-Length and Transfer-Encoding");
        return head;
    }

    /// <summary>Opens the request body as a stream (chunked, Content-Length, or empty).</summary>
    public Stream OpenBody(HttpRequestHead head)
    {
        if (head.Header("Transfer-Encoding") is { } te)
        {
            if (!head.IsChunked) throw new ProtocolFormatException($"unsupported transfer encoding '{te}'");
            return new ChunkedBodyStream(this);
        }
        if (head.Header("Content-Length") is { } cl)
        {
            if (!long.TryParse(cl, NumberStyles.None, CultureInfo.InvariantCulture, out long len))
                throw new ProtocolFormatException("bad Content-Length");
            return new FixedBodyStream(this, len);
        }
        return new FixedBodyStream(this, 0);
    }
}

/// <summary>Read-only helper base: only ReadAsync is meaningful on body streams.</summary>
public abstract class BodyStream : Stream
{
    public override bool CanRead => true;
    public override bool CanSeek => false;
    public override bool CanWrite => false;
    public override long Length => throw new NotSupportedException();
    public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
    public override void Flush() { }
    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    public override int Read(byte[] buffer, int offset, int count) =>
        ReadAsync(buffer.AsMemory(offset, count)).AsTask().GetAwaiter().GetResult();
    public override Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken ct) =>
        ReadAsync(buffer.AsMemory(offset, count), ct).AsTask();
}

public sealed class FixedBodyStream(ConnectionReader reader, long length) : BodyStream
{
    private long _left = length;

    public override async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken ct = default)
    {
        if (_left == 0 || buffer.Length == 0) return 0;
        int n = await reader.ReadAsync(buffer[..(int)Math.Min(buffer.Length, _left)], ct).ConfigureAwait(false);
        if (n == 0) throw new EndOfStreamException("body shorter than Content-Length");
        _left -= n;
        return n;
    }
}

/// <summary>
/// Decodes HTTP chunked transfer coding. <see cref="Terminated"/> becomes true only
/// when the zero-length chunk and its trailing CRLF were read. iOS may never send it;
/// callers decide completeness from the content instead (docs/PROTOCOL.md §6.3).
/// </summary>
public sealed class ChunkedBodyStream(ConnectionReader reader) : BodyStream
{
    private const int MaxChunkLine = 32;
    private const long MaxChunk = 16 * 1024 * 1024;
    private long _left;
    private bool _first = true;

    public bool Terminated { get; private set; }

    public override async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken ct = default)
    {
        if (Terminated || buffer.Length == 0) return 0;
        if (_left == 0)
        {
            if (!_first)
            {
                string? crlf = await reader.ReadLineAsync(2, ct).ConfigureAwait(false);
                if (crlf is not "") throw new ProtocolFormatException("missing CRLF after chunk");
            }
            _first = false;
            string line = await reader.ReadLineAsync(MaxChunkLine, ct).ConfigureAwait(false) ?? throw new EndOfStreamException();
            int semi = line.IndexOf(';');
            ReadOnlySpan<char> hex = semi >= 0 ? line.AsSpan(0, semi) : line;
            if (hex.Length is 0 or > 8 || !long.TryParse(hex, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out long size) || size > MaxChunk)
                throw new ProtocolFormatException("bad chunk size");
            if (size == 0)
            {
                // Trailer section: we accept only an empty one.
                string? end = await reader.ReadLineAsync(MaxChunkLine, ct).ConfigureAwait(false);
                if (end is not "") throw new ProtocolFormatException("unexpected chunk trailer");
                Terminated = true;
                return 0;
            }
            _left = size;
        }
        int n = await reader.ReadAsync(buffer[..(int)Math.Min(buffer.Length, _left)], ct).ConfigureAwait(false);
        if (n == 0) throw new EndOfStreamException("connection closed inside a chunk");
        _left -= n;
        return n;
    }
}

public static class HttpResponse
{
    public static async Task WriteAsync(Stream stream, int status, string reason, ReadOnlyMemory<byte> body, bool close, CancellationToken ct)
    {
        var sb = new StringBuilder();
        sb.Append("HTTP/1.1 ").Append(status).Append(' ').Append(reason).Append("\r\n");
        sb.Append("Content-Length: ").Append(body.Length).Append("\r\n");
        if (close) sb.Append("Connection: close\r\n");
        sb.Append("\r\n");
        byte[] head = Encoding.ASCII.GetBytes(sb.ToString());
        byte[] all = new byte[head.Length + body.Length];
        head.CopyTo(all, 0);
        body.CopyTo(all.AsMemory(head.Length));
        await stream.WriteAsync(all, ct).ConfigureAwait(false);
        await stream.FlushAsync(ct).ConfigureAwait(false);
    }

    public static Task WriteContinueAsync(Stream stream, CancellationToken ct) =>
        stream.WriteAsync("HTTP/1.1 100 Continue\r\n\r\n"u8.ToArray(), ct).AsTask();
}
