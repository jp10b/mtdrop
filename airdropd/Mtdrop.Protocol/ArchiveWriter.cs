using System.Buffers.Binary;
using System.Globalization;
using System.IO.Compression;
using System.Text;

namespace Mtdrop.Protocol;

/// <summary>
/// Streaming encoder for the "dvzip" container (docs/PROTOCOL.md §6.4), the inverse of <see cref="DvzipStream"/>:
/// input is cut into 128 KiB blocks, each written as [4-byte big-endian header][payload] with one zlib stream per
/// block, or stored (header bit 31) when compression does not make it smaller. Write-only;
/// <see cref="CompleteAsync"/> writes the last, partial block.
/// </summary>
public sealed class DvzipWriter(Stream dest) : Stream
{
    public const int BlockSize = 131072;
    private readonly byte[] _block = new byte[BlockSize];
    private int _len;
    private bool _completed;

    public override bool CanRead => false;
    public override bool CanSeek => false;
    public override bool CanWrite => true;
    public override long Length => throw new NotSupportedException();
    public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
    public override void SetLength(long value) => throw new NotSupportedException();
    public override int Read(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException("use WriteAsync");
    public override void Flush() { }
    public override Task FlushAsync(CancellationToken ct) => Task.CompletedTask;

    public override Task WriteAsync(byte[] buffer, int offset, int count, CancellationToken ct) =>
        WriteAsync(buffer.AsMemory(offset, count), ct).AsTask();

    public override async ValueTask WriteAsync(ReadOnlyMemory<byte> data, CancellationToken ct = default)
    {
        if (_completed) throw new InvalidOperationException("dvzip stream already completed");
        while (data.Length > 0)
        {
            int n = Math.Min(data.Length, BlockSize - _len);
            data[..n].CopyTo(_block.AsMemory(_len));
            _len += n;
            data = data[n..];
            if (_len == BlockSize) await EmitAsync(ct).ConfigureAwait(false);
        }
    }

    private async ValueTask EmitAsync(CancellationToken ct)
    {
        if (_len == 0) return;
        byte[] payload;
        uint flag = 0;
        using (var z = new MemoryStream(_len / 2 + 64))
        {
            using (var zs = new ZLibStream(z, CompressionLevel.Fastest, leaveOpen: true)) zs.Write(_block, 0, _len);
            payload = z.ToArray();
        }
        if (payload.Length >= _len)
        {
            payload = _block.AsSpan(0, _len).ToArray();     // photos rarely compress: send them as they are
            flag = 0x80000000;
        }
        byte[] header = new byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(header, flag | (uint)payload.Length);
        await dest.WriteAsync(header, ct).ConfigureAwait(false);
        await dest.WriteAsync(payload, ct).ConfigureAwait(false);
        _len = 0;
    }

    /// <summary>Writes the last, partial block. The destination stream is not completed or closed.</summary>
    public async Task CompleteAsync(CancellationToken ct)
    {
        if (_completed) return;
        await EmitAsync(ct).ConfigureAwait(false);
        _completed = true;
    }
}

/// <summary>
/// Writes an odc ("070707") cpio archive laid out like the iPhone's (docs/PROTOCOL.md §6.5): a "." directory,
/// then "./name" members, then the trailer, padded to 512 bytes. Inverse of <see cref="CpioOdcReader"/>.
/// </summary>
public sealed class CpioOdcWriter(Stream dest)
{
    public const int DirectoryMode = 0x41C0;    // 040700, as the phone writes "."
    public const int FileMode = 0x81A4;         // 100644
    private const string Trailer = "TRAILER!!!";
    private long _written;
    private int _ino;

    public long BytesWritten => _written;

    public Task DirectoryAsync(string name, CancellationToken ct) => HeaderAsync(name, DirectoryMode, 0, 2, 0, ct);

    /// <summary>
    /// Writes one file member with exactly <paramref name="size"/> bytes from <paramref name="data"/>.
    /// Throws <see cref="IOException"/> if the source ends early (the file changed while being sent).
    /// </summary>
    public async Task FileAsync(string name, Stream data, long size, DateTimeOffset modified, CancellationToken ct,
        IProgress<long>? progress = null)
    {
        await HeaderAsync(name, FileMode, size, 1, Math.Max(0, modified.ToUnixTimeSeconds()), ct).ConfigureAwait(false);
        byte[] buf = new byte[64 * 1024];
        long left = size;
        while (left > 0)
        {
            int n = await data.ReadAsync(buf.AsMemory(0, (int)Math.Min(buf.Length, left)), ct).ConfigureAwait(false);
            if (n == 0) throw new IOException($"{name} became shorter while it was being sent");
            await dest.WriteAsync(buf.AsMemory(0, n), ct).ConfigureAwait(false);
            _written += n;
            left -= n;
            progress?.Report(n);
        }
    }

    /// <summary>Trailer member and zero padding to a multiple of 512 bytes.</summary>
    public async Task FinishAsync(CancellationToken ct)
    {
        await HeaderAsync(Trailer, 0, 0, 1, 0, ct).ConfigureAwait(false);
        int pad = (int)((512 - _written % 512) % 512);
        if (pad > 0)
        {
            await dest.WriteAsync(new byte[pad], ct).ConfigureAwait(false);
            _written += pad;
        }
    }

    private async Task HeaderAsync(string name, int mode, long size, int links, long mtime, CancellationToken ct)
    {
        byte[] nameBytes = [.. Encoding.UTF8.GetBytes(name), 0];
        if (nameBytes.Length > 1024) throw new ArgumentException("cpio member name too long", nameof(name));
        if (size > 0x1FFFFFFFFL) throw new ArgumentException("file too large for odc cpio (8 GiB)", nameof(size));
        string header = "070707" + Oct(0, 6) + Oct(_ino++ & 0x3FFFF, 6) + Oct(mode, 6) + Oct(0, 6) + Oct(0, 6) + Oct(links, 6)
                        + Oct(0, 6) + Oct(mtime, 11) + Oct(nameBytes.Length, 6) + Oct(size, 11);
        byte[] h = Encoding.ASCII.GetBytes(header);
        await dest.WriteAsync(h, ct).ConfigureAwait(false);
        await dest.WriteAsync(nameBytes, ct).ConfigureAwait(false);
        _written += h.Length + nameBytes.Length;
    }

    private static string Oct(long v, int width)
    {
        string s = Convert.ToString(v, 8);
        if (s.Length > width) throw new ArgumentOutOfRangeException(nameof(v), v.ToString(CultureInfo.InvariantCulture));
        return s.PadLeft(width, '0');
    }
}
