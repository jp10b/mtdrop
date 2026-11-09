using System.Buffers.Binary;
using System.IO.Compression;
using System.Security.Cryptography;
using Mtdrop.Protocol;

namespace Mtdrop.Protocol.Tests;

public class BinaryPlistTests
{
    // Produced by CPython plistlib (FMT_BINARY) from a dict with nested containers,
    // integers of every width, booleans, a real, non-ASCII text and 300 bytes of data.
    private const string PythonPlist =
        "YnBsaXN0MDDbAQIDBAUGBwgJCgsMDxAREhMUFRYXHVpUcmFuc2ZlcklEU0JpZ1NOZWdVU21hbGxTTWlkVEZsYWdSTm9UTmFtZVREYXRhVExpc3RVRW1wdHnRDQ5SaWRfECQyRjBDNkQ1RS0wMDAwLTQwMDAtODAwMC0wMTIzNDU2Nzg5QUITAAABAAAAAAAT//////////sQBxIAARFwCQhvEBQAWgBhAXwA8wFCAQcAIABnARkBWwBsAQUAIABqAGEBegFEACDYPdz3TxEBLAABAgMEBQYHCAkKCwwNDg8QERITFBUWFxgZGhscHR4fICEiIyQlJicoKSorLC0uLzAxMjM0NTY3ODk6Ozw9Pj9AQUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVpbXF1eX2BhYmNkZWZnaGlqa2xtbm9wcXJzdHV2d3h5ent8fX5/gIGCg4SFhoeIiYqLjI2Oj5CRkpOUlZaXmJmam5ydnp+goaKjpKWmp6ipqqusra6vsLGys7S1tre4ubq7vL2+v8DBwsPExcbHyMnKy8zNzs/Q0dLT1NXW19jZ2tvc3d7f4OHi4+Tl5ufo6err7O3u7/Dx8vP09fb3+Pn6+/z9/v94eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eKQYGRocEAFTdHdvoRsjQAwAAAAAAADQUAAIAB8AKgAuADIAOAA8AEEARABJAE4AUwBZAFwAXwCGAI8AmACaAJ8AoAChAMwB/AIBAgMCBwIJAhICEwAAAAAAAAIBAAAAAAAAAB4AAAAAAAAAAAAAAAAAAAIU";

    [Fact]
    public void Reads_a_plist_written_by_another_implementation()
    {
        var d = BinaryPlist.ReadDictionary(Convert.FromBase64String(PythonPlist));
        Assert.Equal("2F0C6D5E-0000-4000-8000-0123456789AB", ((Dictionary<string, object?>)d["TransferID"]!)["id"]);
        Assert.Equal(1L << 40, d["Big"]);
        Assert.Equal(-5L, d["Neg"]);
        Assert.Equal(7L, d["Small"]);
        Assert.Equal(70000L, d["Mid"]);
        Assert.Equal(true, d["Flag"]);
        Assert.Equal(false, d["No"]);
        Assert.Equal("Zażółć gęślą jaźń 📷", d["Name"]);
        byte[] data = (byte[])d["Data"]!;
        Assert.Equal(300, data.Length);
        Assert.Equal(255, data[255]);
        var list = (List<object?>)d["List"]!;
        Assert.Equal(1L, list[0]);
        Assert.Equal("two", list[1]);
        Assert.Equal(3.5, ((List<object?>)list[2]!)[0]);
        Assert.Empty((Dictionary<string, object?>)list[3]!);
        Assert.Equal("", d["Empty"]);
    }

    [Fact]
    public void Round_trips_what_it_writes()
    {
        var original = new Dictionary<string, object?>
        {
            ["ReceiverComputerName"] = "Łaptop ✓",
            ["Bytes"] = RandomNumberGenerator.GetBytes(70_000),
            ["Numbers"] = new List<object?> { 0L, 255L, 256L, 65536L, 1L << 33, -1L },
            ["Nested"] = new Dictionary<string, object?> { ["a"] = true, ["b"] = null, ["c"] = 1.25 },
            ["Many"] = Enumerable.Range(0, 400).Select(i => (object?)$"item{i}").ToList(),
        };
        var back = BinaryPlist.ReadDictionary(BinaryPlist.Write(original));
        Assert.Equal("Łaptop ✓", back["ReceiverComputerName"]);
        Assert.Equal((byte[])original["Bytes"]!, (byte[])back["Bytes"]!);
        Assert.Equal(new List<object?> { 0L, 255L, 256L, 65536L, 1L << 33, -1L }, (List<object?>)back["Numbers"]!);
        var nested = (Dictionary<string, object?>)back["Nested"]!;
        Assert.Equal(true, nested["a"]);
        Assert.Null(nested["b"]);
        Assert.Equal(1.25, nested["c"]);
        Assert.Equal(400, ((List<object?>)back["Many"]!).Count);
    }

    [Fact]
    public void Rejects_garbage_and_truncation_without_crashing()
    {
        byte[] good = Convert.FromBase64String(PythonPlist);
        Assert.Throws<ProtocolFormatException>(() => BinaryPlist.Read("not a plist"u8));
        Assert.Throws<ProtocolFormatException>(() => BinaryPlist.Read(good.AsSpan(0, 40)));
        var rng = new Random(1234);
        for (int i = 0; i < 3000; i++)
        {
            byte[] mutated = (byte[])good.Clone();
            int flips = 1 + rng.Next(4);
            for (int f = 0; f < flips; f++) mutated[rng.Next(mutated.Length)] = (byte)rng.Next(256);
            try { BinaryPlist.Read(mutated); }
            catch (ProtocolFormatException) { }
        }
    }

    [Fact]
    public void Rejects_self_referencing_containers()
    {
        // One array object whose single element refers to itself.
        byte[] p = [.. "bplist00"u8, 0xA1, 0x00, 0x08, .. new byte[6], 1, 1, .. new byte[7], 1, .. new byte[8], .. new byte[7], 10];
        Assert.Throws<ProtocolFormatException>(() => BinaryPlist.Read(p));
    }
}

public class ArchiveTests
{
    private static async Task<byte[]> ReadAllAsync(Stream s)
    {
        using var ms = new MemoryStream();
        await s.CopyToAsync(ms);
        return ms.ToArray();
    }

    [Fact]
    public async Task Dvzip_decodes_mixed_deflated_and_stored_blocks()
    {
        byte[] raw = RandomNumberGenerator.GetBytes(131072 * 3 + 777);
        byte[] back = await ReadAllAsync(new DvzipStream(new MemoryStream(Wire.Dvzip(raw, storedFrom: 2))));
        Assert.Equal(raw, back);
    }

    [Fact]
    public async Task Dvzip_accepts_a_deflated_payload_longer_than_its_block()
    {
        byte[] raw = RandomNumberGenerator.GetBytes(131072);      // incompressible
        byte[] wire = Wire.Dvzip(raw);
        Assert.True(BinaryPrimitives.ReadUInt32BigEndian(wire) > 131072);
        Assert.Equal(raw, await ReadAllAsync(new DvzipStream(new MemoryStream(wire))));
    }

    [Fact]
    public async Task Dvzip_rejects_truncated_corrupt_and_oversized_blocks()
    {
        byte[] wire = Wire.Dvzip(RandomNumberGenerator.GetBytes(5000));
        await Assert.ThrowsAsync<EndOfStreamException>(() => ReadAllAsync(new DvzipStream(new MemoryStream(wire[..^10]))));
        byte[] corrupt = (byte[])wire.Clone();
        corrupt[10] ^= 0xFF;
        corrupt[11] ^= 0xFF;
        await Assert.ThrowsAsync<ProtocolFormatException>(() => ReadAllAsync(new DvzipStream(new MemoryStream(corrupt))));
        await Assert.ThrowsAsync<ProtocolFormatException>(() => ReadAllAsync(new DvzipStream(new MemoryStream([0x7F, 0xFF, 0xFF, 0xFF]))));
        await Assert.ThrowsAsync<ProtocolFormatException>(() => ReadAllAsync(new DvzipStream(new MemoryStream([0, 0, 0, 0]))));

        // A small block that inflates beyond the limit (decompression bomb).
        using var z = new MemoryStream();
        using (var zs = new ZLibStream(z, CompressionLevel.Optimal, leaveOpen: true)) zs.Write(new byte[8 * 1024 * 1024]);
        byte[] bomb = [0, 0, (byte)(z.Length >> 8), (byte)z.Length, .. z.ToArray()];
        await Assert.ThrowsAsync<ProtocolFormatException>(() => ReadAllAsync(new DvzipStream(new MemoryStream(bomb))));
    }

    [Fact]
    public async Task Cpio_lists_members_and_stops_at_the_trailer_without_reading_further()
    {
        byte[] photo = RandomNumberGenerator.GetBytes(1000);
        byte[] archive = Wire.PhotoArchive("IMG_0001.JPG", photo);
        Assert.Equal(0, archive.Length % 512);
        var ms = new MemoryStream(archive);
        var cpio = new CpioOdcReader(ms);
        var seen = new List<CpioEntry>();
        while (await cpio.NextAsync(default) is { } e)
        {
            seen.Add(e);
            await cpio.SkipDataAsync(default);
        }
        Assert.True(cpio.ReachedTrailer);
        Assert.Equal([".", "./IMG_0001.JPG", "./._IMG_0001.JPG"], seen.Select(e => e.Name));
        Assert.Equal([CpioEntryKind.Directory, CpioEntryKind.RegularFile, CpioEntryKind.RegularFile], seen.Select(e => e.Kind));
        Assert.Equal(1000, seen[1].Size);
        Assert.True(ms.Position < archive.Length, "must not consume the padding after the trailer");
    }

    [Fact]
    public async Task Cpio_rejects_bad_headers_and_truncation()
    {
        await Assert.ThrowsAsync<ProtocolFormatException>(async () => await new CpioOdcReader(new MemoryStream(new byte[100])).NextAsync(default));
        byte[] archive = Wire.Cpio([new Wire.Member("./a", Wire.File, new byte[50])]);
        byte[] minus = (byte[])archive.Clone();
        minus[65] = (byte)'-';                                  // negative size field
        await Assert.ThrowsAsync<ProtocolFormatException>(async () => await new CpioOdcReader(new MemoryStream(minus)).NextAsync(default));
        var cut = new CpioOdcReader(new MemoryStream(archive[..100]));
        Assert.NotNull(await cut.NextAsync(default));
        await Assert.ThrowsAsync<EndOfStreamException>(async () => await cut.SkipDataAsync(default));
    }
}

public class SafeNameTests
{
    [Theory]
    [InlineData("./IMG_0001.JPG", "IMG_0001.JPG")]
    [InlineData("IMG_0001.JPG", "IMG_0001.JPG")]
    [InlineData("./NSIRD_AirDrop_x/IMG_0001.JPG", "IMG_0001.JPG")]
    [InlineData("./IMG_0001.pvt/IMG_0001.MOV", "IMG_0001.MOV")]
    [InlineData("./a/NUL.txt", "_NUL.txt")]
    [InlineData("./com1", "_com1")]
    [InlineData("./trailing. ", "trailing")]
    [InlineData("./...hidden", "hidden")]
    [InlineData("./photo\u202Egpj.exe", "photogpj.exe")]
    [InlineData("./a<b>c|d?e*f\"g.txt", "abcdefg.txt")]
    public void Maps_member_names_to_plain_file_names(string member, string expected) =>
        Assert.Equal(expected, SafeName.FromArchiveMember(member));

    [Theory]
    [InlineData(".")]
    [InlineData("./")]
    [InlineData("./._IMG_0001.JPG")]
    [InlineData("./dir/._x")]
    [InlineData("./IMG_0001.pvt/metadata.plist")]
    [InlineData("./...")]
    public void Skips_members_that_are_not_user_content(string member) =>
        Assert.Null(SafeName.FromArchiveMember(member));

    [Theory]
    [InlineData("../evil")]
    [InlineData("./a/../../evil")]
    [InlineData("/etc/passwd")]
    [InlineData("C:/Windows/x")]
    [InlineData("a\\b")]
    [InlineData("./file.txt:stream")]
    [InlineData("./bad\u0001name")]
    [InlineData("")]
    public void Rejects_names_that_try_to_escape(string member) =>
        Assert.Throws<ProtocolFormatException>(() => SafeName.FromArchiveMember(member));

    [Fact]
    public void Caps_length_and_keeps_the_extension()
    {
        string name = SafeName.Clean(new string('a', 500) + ".jpeg")!;
        Assert.Equal(SafeName.MaxLength, name.Length);
        Assert.EndsWith(".jpeg", name);
    }
}
