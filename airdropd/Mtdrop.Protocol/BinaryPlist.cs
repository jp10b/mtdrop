using System.Buffers.Binary;
using System.Text;

namespace Mtdrop.Protocol;

/// <summary>Thrown for any input that violates a wire format we parse.</summary>
public sealed class ProtocolFormatException(string message) : Exception(message);

/// <summary>
/// Reader/writer for Apple binary property lists ("bplist00"), limited to the
/// object kinds AirDrop uses: dict, array, string, data, integer, real, bool, date.
/// Values map to: Dictionary&lt;string, object?&gt;, List&lt;object?&gt;, string,
/// byte[], long, double, bool, DateTime, null.
/// The reader treats its input as hostile: every offset and count is bounds-checked
/// and nesting depth and object count are limited.
/// </summary>
public static class BinaryPlist
{
    private const int MaxDepth = 32;
    private const int MaxObjects = 100_000;
    private static readonly byte[] Magic = "bplist00"u8.ToArray();
    private static readonly DateTime AppleEpoch = new(2001, 1, 1, 0, 0, 0, DateTimeKind.Utc);

    public static object? Read(ReadOnlySpan<byte> data) => new Reader(data.ToArray()).ReadRoot();

    public static Dictionary<string, object?> ReadDictionary(ReadOnlySpan<byte> data) =>
        Read(data) as Dictionary<string, object?> ?? throw new ProtocolFormatException("plist root is not a dictionary");

    public static byte[] Write(object? root) => new Writer().Write(root);

    private sealed class Reader(byte[] d)
    {
        private int _offsetSize, _refSize, _numObjects, _offsetTable;
        private int _budget = MaxObjects;

        public object? ReadRoot()
        {
            if (d.Length < Magic.Length + 32 || !d.AsSpan(0, 6).SequenceEqual(Magic.AsSpan(0, 6)))
                throw new ProtocolFormatException("not a binary plist");
            var t = d.AsSpan(d.Length - 32);
            _offsetSize = t[6];
            _refSize = t[7];
            ulong numObjects = BinaryPrimitives.ReadUInt64BigEndian(t[8..]);
            ulong top = BinaryPrimitives.ReadUInt64BigEndian(t[16..]);
            ulong table = BinaryPrimitives.ReadUInt64BigEndian(t[24..]);
            if (_offsetSize is < 1 or > 8 || _refSize is < 1 or > 8) throw new ProtocolFormatException("bad plist trailer");
            if (numObjects == 0 || numObjects > MaxObjects || top >= numObjects) throw new ProtocolFormatException("bad plist object count");
            if (table > (ulong)d.Length - 32 || table + numObjects * (ulong)_offsetSize > (ulong)d.Length - 32)
                throw new ProtocolFormatException("bad plist offset table");
            _numObjects = (int)numObjects;
            _offsetTable = (int)table;
            return ReadObject((int)top, 0);
        }

        private ulong ReadSized(int pos, int size)
        {
            if (pos < 0 || size < 1 || size > 8 || pos + size > d.Length) throw new ProtocolFormatException("plist read out of range");
            ulong v = 0;
            for (int i = 0; i < size; i++) v = (v << 8) | d[pos + i];
            return v;
        }

        private int ObjectOffset(int index)
        {
            if (index < 0 || index >= _numObjects) throw new ProtocolFormatException("plist object reference out of range");
            ulong off = ReadSized(_offsetTable + index * _offsetSize, _offsetSize);
            if (off >= (ulong)_offsetTable) throw new ProtocolFormatException("plist object offset out of range");
            return (int)off;
        }

        /// <summary>Reads the marker's length nibble, following the 0xF "int follows" escape.</summary>
        private int ReadLength(ref int pos, int nibble)
        {
            if (nibble != 0xF) return nibble;
            if (pos >= d.Length) throw new ProtocolFormatException("plist truncated");
            int m = d[pos++];
            if ((m & 0xF0) != 0x10) throw new ProtocolFormatException("plist length is not an integer");
            int size = 1 << (m & 0x0F);
            ulong v = ReadSized(pos, size);
            pos += size;
            if (v > int.MaxValue) throw new ProtocolFormatException("plist length too large");
            return (int)v;
        }

        private ReadOnlySpan<byte> Slice(int pos, long len)
        {
            if (len < 0 || pos < 0 || pos + len > _offsetTable) throw new ProtocolFormatException("plist object out of range");
            return d.AsSpan(pos, (int)len);
        }

        private object? ReadObject(int index, int depth)
        {
            if (depth > MaxDepth) throw new ProtocolFormatException("plist nested too deeply");
            if (--_budget < 0) throw new ProtocolFormatException("plist has too many objects");
            int pos = ObjectOffset(index);
            int marker = d[pos++];
            int kind = marker >> 4, low = marker & 0x0F;
            switch (kind)
            {
                case 0x0:
                    return low switch
                    {
                        0x0 => null,
                        0x8 => false,
                        0x9 => true,
                        _ => throw new ProtocolFormatException("unsupported plist singleton"),
                    };
                case 0x1:
                {
                    int size = 1 << low;
                    if (size > 16) throw new ProtocolFormatException("bad plist integer size");
                    if (size == 16) { Slice(pos, 16); return (long)ReadSized(pos + 8, 8); }
                    ulong v = ReadSized(pos, size);
                    return size == 8 ? unchecked((long)v) : (long)v;
                }
                case 0x2:
                    return low switch
                    {
                        2 => (double)BinaryPrimitives.ReadSingleBigEndian(Slice(pos, 4)),
                        3 => BinaryPrimitives.ReadDoubleBigEndian(Slice(pos, 8)),
                        _ => throw new ProtocolFormatException("bad plist real size"),
                    };
                case 0x3:
                {
                    if (low != 3) throw new ProtocolFormatException("bad plist date");
                    double s = BinaryPrimitives.ReadDoubleBigEndian(Slice(pos, 8));
                    if (double.IsNaN(s) || Math.Abs(s) > 1e11) throw new ProtocolFormatException("bad plist date value");
                    return AppleEpoch.AddSeconds(s);
                }
                case 0x4:
                {
                    int len = ReadLength(ref pos, low);
                    return Slice(pos, len).ToArray();
                }
                case 0x5:
                {
                    int len = ReadLength(ref pos, low);
                    return Encoding.ASCII.GetString(Slice(pos, len));
                }
                case 0x6:
                {
                    int len = ReadLength(ref pos, low);
                    return Encoding.BigEndianUnicode.GetString(Slice(pos, 2L * len));
                }
                case 0xA:
                {
                    int count = ReadLength(ref pos, low);
                    Slice(pos, (long)count * _refSize);
                    var list = new List<object?>(Math.Min(count, 1024));
                    for (int i = 0; i < count; i++)
                        list.Add(ReadObject((int)ReadSized(pos + i * _refSize, _refSize), depth + 1));
                    return list;
                }
                case 0xD:
                {
                    int count = ReadLength(ref pos, low);
                    Slice(pos, 2L * count * _refSize);
                    var dict = new Dictionary<string, object?>(Math.Min(count, 1024), StringComparer.Ordinal);
                    for (int i = 0; i < count; i++)
                    {
                        if (ReadObject((int)ReadSized(pos + i * _refSize, _refSize), depth + 1) is not string key)
                            throw new ProtocolFormatException("plist dictionary key is not a string");
                        dict[key] = ReadObject((int)ReadSized(pos + (count + i) * _refSize, _refSize), depth + 1);
                    }
                    return dict;
                }
                default:
                    throw new ProtocolFormatException($"unsupported plist object type 0x{marker:x2}");
            }
        }
    }

    private sealed class Writer
    {
        private readonly List<object?> _objects = [];
        private readonly List<int[]> _children = [];

        public byte[] Write(object? root)
        {
            Flatten(root, 0);
            int refSize = _objects.Count <= 0xFF ? 1 : _objects.Count <= 0xFFFF ? 2 : 4;
            using var ms = new MemoryStream();
            ms.Write(Magic);
            var offsets = new long[_objects.Count];
            for (int i = 0; i < _objects.Count; i++)
            {
                offsets[i] = ms.Position;
                WriteObject(ms, _objects[i], _children[i], refSize);
            }
            long table = ms.Position;
            int offsetSize = table <= 0xFF ? 1 : table <= 0xFFFF ? 2 : table <= 0xFFFFFF ? 3 : 4;
            foreach (long o in offsets) WriteSized(ms, (ulong)o, offsetSize);
            Span<byte> trailer = stackalloc byte[32];
            trailer[6] = (byte)offsetSize;
            trailer[7] = (byte)refSize;
            BinaryPrimitives.WriteUInt64BigEndian(trailer[8..], (ulong)_objects.Count);
            BinaryPrimitives.WriteUInt64BigEndian(trailer[16..], 0);
            BinaryPrimitives.WriteUInt64BigEndian(trailer[24..], (ulong)table);
            ms.Write(trailer);
            return ms.ToArray();
        }

        private int Flatten(object? o, int depth)
        {
            if (depth > MaxDepth) throw new ArgumentException("object graph too deep");
            int index = _objects.Count;
            _objects.Add(o);
            _children.Add([]);
            switch (o)
            {
                case IReadOnlyDictionary<string, object?> dict:
                {
                    var refs = new int[dict.Count * 2];
                    int i = 0;
                    foreach (var kv in dict)
                    {
                        refs[i] = Flatten(kv.Key, depth + 1);
                        refs[dict.Count + i] = Flatten(kv.Value, depth + 1);
                        i++;
                    }
                    _children[index] = refs;
                    break;
                }
                case string or byte[]:
                    break;
                case System.Collections.IEnumerable seq:
                {
                    var refs = new List<int>();
                    foreach (object? item in seq) refs.Add(Flatten(item, depth + 1));
                    _children[index] = [.. refs];
                    break;
                }
            }
            return index;
        }

        private static void WriteSized(Stream s, ulong v, int size)
        {
            for (int i = size - 1; i >= 0; i--) s.WriteByte((byte)(v >> (8 * i)));
        }

        private static void WriteMarker(Stream s, int kind, int length)
        {
            if (length < 15) { s.WriteByte((byte)((kind << 4) | length)); return; }
            s.WriteByte((byte)((kind << 4) | 0xF));
            WriteInt(s, length);
        }

        private static void WriteInt(Stream s, long v)
        {
            if (v is >= 0 and <= 0xFF) { s.WriteByte(0x10); WriteSized(s, (ulong)v, 1); }
            else if (v is >= 0 and <= 0xFFFF) { s.WriteByte(0x11); WriteSized(s, (ulong)v, 2); }
            else if (v is >= 0 and <= 0xFFFFFFFF) { s.WriteByte(0x12); WriteSized(s, (ulong)v, 4); }
            else { s.WriteByte(0x13); WriteSized(s, unchecked((ulong)v), 8); }
        }

        private static void WriteObject(Stream s, object? o, int[] refs, int refSize)
        {
            switch (o)
            {
                case null: s.WriteByte(0x00); break;
                case bool b: s.WriteByte(b ? (byte)0x09 : (byte)0x08); break;
                case int i: WriteInt(s, i); break;
                case long l: WriteInt(s, l); break;
                case double dbl:
                {
                    s.WriteByte(0x23);
                    Span<byte> buf = stackalloc byte[8];
                    BinaryPrimitives.WriteDoubleBigEndian(buf, dbl);
                    s.Write(buf);
                    break;
                }
                case DateTime dt:
                {
                    s.WriteByte(0x33);
                    Span<byte> buf = stackalloc byte[8];
                    BinaryPrimitives.WriteDoubleBigEndian(buf, (dt.ToUniversalTime() - AppleEpoch).TotalSeconds);
                    s.Write(buf);
                    break;
                }
                case byte[] data:
                    WriteMarker(s, 0x4, data.Length);
                    s.Write(data);
                    break;
                case string str:
                    if (Ascii.IsValid(str))
                    {
                        WriteMarker(s, 0x5, str.Length);
                        s.Write(Encoding.ASCII.GetBytes(str));
                    }
                    else
                    {
                        WriteMarker(s, 0x6, str.Length);
                        s.Write(Encoding.BigEndianUnicode.GetBytes(str));
                    }
                    break;
                case IReadOnlyDictionary<string, object?> dict:
                    WriteMarker(s, 0xD, dict.Count);
                    foreach (int r in refs) WriteSized(s, (ulong)r, refSize);
                    break;
                case System.Collections.IEnumerable:
                    WriteMarker(s, 0xA, refs.Length);
                    foreach (int r in refs) WriteSized(s, (ulong)r, refSize);
                    break;
                default:
                    throw new ArgumentException($"cannot encode {o.GetType()} in a plist");
            }
        }
    }
}
