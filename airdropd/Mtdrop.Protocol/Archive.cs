using System.Buffers.Binary;
using System.IO.Compression;
using System.Text;

namespace Mtdrop.Protocol;

/// <summary>
/// Streaming decoder for the "dvzip" container: a run of blocks, each
/// [4-byte big-endian header][payload]; header bit 31 = payload stored as is,
/// otherwise one zlib stream; low 31 bits = payload length (docs/PROTOCOL.md §6.4).
/// A deflated payload may be longer than the 128 KiB it inflates to.
/// </summary>
public sealed class DvzipStream(Stream source) : BodyStream
{
    public const int MaxBlockWire = 1024 * 1024;
    public const int MaxBlockInflated = 1024 * 1024;
    private byte[] _block = [];
    private int _pos;
    private bool _eof;

    private async ValueTask<int> ReadSomeAsync(Memory<byte> dest, bool allowCleanEof, CancellationToken ct)
    {
        int got = 0;
        while (got < dest.Length)
        {
            int n = await source.ReadAsync(dest[got..], ct).ConfigureAwait(false);
            if (n == 0)
            {
                if (got == 0 && allowCleanEof) return 0;
                throw new EndOfStreamException("dvzip stream ended inside a block");
            }
            got += n;
        }
        return got;
    }

    public override async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken ct = default)
    {
        if (buffer.Length == 0) return 0;
        while (_pos == _block.Length)
        {
            if (_eof) return 0;
            byte[] header = new byte[4];
            if (await ReadSomeAsync(header, allowCleanEof: true, ct).ConfigureAwait(false) == 0)
            {
                _eof = true;
                return 0;
            }
            uint word = BinaryPrimitives.ReadUInt32BigEndian(header);
            int length = (int)(word & 0x7FFFFFFF);
            if (length == 0 || length > MaxBlockWire) throw new ProtocolFormatException("bad dvzip block length");
            byte[] payload = new byte[length];
            await ReadSomeAsync(payload, allowCleanEof: false, ct).ConfigureAwait(false);
            _block = (word & 0x80000000) != 0 ? payload : Inflate(payload);
            _pos = 0;
        }
        int count = Math.Min(buffer.Length, _block.Length - _pos);
        _block.AsMemory(_pos, count).CopyTo(buffer);
        _pos += count;
        return count;
    }

    private static byte[] Inflate(byte[] payload)
    {
        try
        {
            using var z = new ZLibStream(new MemoryStream(payload), CompressionMode.Decompress);
            using var output = new MemoryStream();
            byte[] tmp = new byte[81920];
            int n;
            while ((n = z.Read(tmp)) > 0)
            {
                if (output.Length + n > MaxBlockInflated) throw new ProtocolFormatException("dvzip block inflates too large");
                output.Write(tmp, 0, n);
            }
            if (output.Length == 0) throw new ProtocolFormatException("empty dvzip block");
            return output.ToArray();
        }
        catch (InvalidDataException e)
        {
            throw new ProtocolFormatException("corrupt dvzip block: " + e.Message);
        }
    }
}

public enum CpioEntryKind { RegularFile, Directory, Other }

public sealed record CpioEntry(string Name, CpioEntryKind Kind, long Size, int LinkCount);

/// <summary>
/// Forward-only reader for cpio "odc" archives (magic 070707, 76-byte ASCII-octal
/// header, no padding) as sent by iOS (docs/PROTOCOL.md §6.5).
/// Usage: call <see cref="NextAsync"/>; if it returns an entry, read exactly
/// <c>entry.Size</c> bytes through <see cref="ReadDataAsync"/> or call <see cref="SkipDataAsync"/>.
/// <see cref="NextAsync"/> returns null once the TRAILER!!! member was read; nothing
/// after the trailer is consumed, so a sender that withholds the final HTTP chunk does not block us.
/// </summary>
public sealed class CpioOdcReader(Stream source)
{
    public const string Trailer = "TRAILER!!!";
    public const int MaxNameBytes = 4096;
    private const int HeaderLength = 76;
    private long _dataLeft;

    public bool ReachedTrailer { get; private set; }

    private async ValueTask FillAsync(Memory<byte> dest, CancellationToken ct)
    {
        while (dest.Length > 0)
        {
            int n = await source.ReadAsync(dest, ct).ConfigureAwait(false);
            if (n == 0) throw new EndOfStreamException("archive ended before its trailer");
            dest = dest[n..];
        }
    }

    private static long Octal(ReadOnlySpan<byte> field)
    {
        long v = 0;
        foreach (byte b in field)
        {
            if (b is < (byte)'0' or > (byte)'7') throw new ProtocolFormatException("bad cpio header field");
            v = (v << 3) | (uint)(b - '0');
        }
        return v;
    }

    public async ValueTask<CpioEntry?> NextAsync(CancellationToken ct)
    {
        if (ReachedTrailer) return null;
        if (_dataLeft != 0) throw new InvalidOperationException("previous member data not consumed");
        byte[] h = new byte[HeaderLength];
        await FillAsync(h, ct).ConfigureAwait(false);
        if (!h.AsSpan(0, 6).SequenceEqual("070707"u8)) throw new ProtocolFormatException("bad cpio magic");
        long mode = Octal(h.AsSpan(18, 6));
        long nlink = Octal(h.AsSpan(36, 6));
        long nameSize = Octal(h.AsSpan(59, 6));
        long size = Octal(h.AsSpan(65, 11));
        if (nameSize is < 2 or > MaxNameBytes) throw new ProtocolFormatException("bad cpio name size");
        byte[] nameBytes = new byte[nameSize];
        await FillAsync(nameBytes, ct).ConfigureAwait(false);
        if (nameBytes[^1] != 0 || nameBytes.AsSpan(0, nameBytes.Length - 1).Contains((byte)0))
            throw new ProtocolFormatException("cpio name not NUL-terminated");
        string name;
        try
        {
            name = new UTF8Encoding(false, throwOnInvalidBytes: true).GetString(nameBytes, 0, nameBytes.Length - 1);
        }
        catch (DecoderFallbackException)
        {
            throw new ProtocolFormatException("cpio name is not valid UTF-8");
        }
        if (name == Trailer)
        {
            if (size != 0) throw new ProtocolFormatException("cpio trailer carries data");
            ReachedTrailer = true;
            return null;
        }
        var kind = (mode & 0xF000) switch
        {
            0x8000 => CpioEntryKind.RegularFile,
            0x4000 => CpioEntryKind.Directory,
            _ => CpioEntryKind.Other,
        };
        if (kind == CpioEntryKind.Directory && size != 0)
            throw new ProtocolFormatException("cpio directory carries data");
        _dataLeft = size;
        return new CpioEntry(name, kind, size, (int)Math.Min(nlink, int.MaxValue));
    }

    public async ValueTask<int> ReadDataAsync(Memory<byte> buffer, CancellationToken ct)
    {
        if (_dataLeft == 0 || buffer.Length == 0) return 0;
        int n = await source.ReadAsync(buffer[..(int)Math.Min(buffer.Length, _dataLeft)], ct).ConfigureAwait(false);
        if (n == 0) throw new EndOfStreamException("archive ended inside a member");
        _dataLeft -= n;
        return n;
    }

    public async ValueTask SkipDataAsync(CancellationToken ct)
    {
        byte[] tmp = new byte[81920];
        while (_dataLeft > 0) await ReadDataAsync(tmp, ct).ConfigureAwait(false);
    }
}
