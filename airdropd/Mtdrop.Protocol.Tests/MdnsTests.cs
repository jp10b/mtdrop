using System.Buffers.Binary;
using System.Net;
using System.Text;
using Mtdrop.Protocol;

namespace Mtdrop.Protocol.Tests;

public class MdnsTests
{
    private static readonly AirDropServiceRecords Records = new()
    {
        ServiceId = "0123456789ab",
        Address = IPAddress.Parse("fe80::1234:56ff:fe78:9abc"),
        Port = 8771,
    };

    private sealed record Rr(string Name, int Type, int Class, uint Ttl, byte[] Data);

    private static List<Rr> ParseAnswers(byte[] p)
    {
        Assert.Equal(0x8400, BinaryPrimitives.ReadUInt16BigEndian(p.AsSpan(2)));
        Assert.Equal(0, BinaryPrimitives.ReadUInt16BigEndian(p.AsSpan(4)));
        int count = BinaryPrimitives.ReadUInt16BigEndian(p.AsSpan(6));
        var list = new List<Rr>();
        int pos = 12;
        for (int i = 0; i < count; i++)
        {
            string name = AirDropServiceRecords.ReadName(p, ref pos);
            int type = BinaryPrimitives.ReadUInt16BigEndian(p.AsSpan(pos));
            int cls = BinaryPrimitives.ReadUInt16BigEndian(p.AsSpan(pos + 2));
            uint ttl = BinaryPrimitives.ReadUInt32BigEndian(p.AsSpan(pos + 4));
            int len = BinaryPrimitives.ReadUInt16BigEndian(p.AsSpan(pos + 8));
            list.Add(new Rr(name, type, cls, ttl, p.AsSpan(pos + 10, len).ToArray()));
            pos += 10 + len;
        }
        Assert.Equal(p.Length, pos);
        return list;
    }

    [Fact]
    public void Announcement_carries_the_record_set_the_phone_accepted()
    {
        List<Rr> rr = ParseAnswers(Records.BuildAnnouncement());
        Assert.Equal(5, rr.Count);

        Assert.Equal(("_airdrop._tcp.local", 12, 0x0001, 4500u), (rr[0].Name, rr[0].Type, rr[0].Class, rr[0].Ttl));
        int pos = 0;
        Assert.Equal("0123456789ab._airdrop._tcp.local", AirDropServiceRecords.ReadName(rr[0].Data, ref pos));

        Assert.Equal(("0123456789ab._airdrop._tcp.local", 33, 0x8001, 120u), (rr[1].Name, rr[1].Type, rr[1].Class, rr[1].Ttl));
        Assert.Equal(8771, BinaryPrimitives.ReadUInt16BigEndian(rr[1].Data.AsSpan(4)));
        pos = 6;
        Assert.Equal("0123456789ab.local", AirDropServiceRecords.ReadName(rr[1].Data, ref pos));

        Assert.Equal(("0123456789ab._airdrop._tcp.local", 16, 0x8001, 4500u), (rr[2].Name, rr[2].Type, rr[2].Class, rr[2].Ttl));
        Assert.Equal("\tflags=136", Encoding.ASCII.GetString(rr[2].Data));

        Assert.Equal(("0123456789ab.local", 28, 0x8001, 120u), (rr[3].Name, rr[3].Type, rr[3].Class, rr[3].Ttl));
        Assert.Equal(Records.Address, new IPAddress(rr[3].Data));

        Assert.Equal(("0123456789ab.local", 47), (rr[4].Name, rr[4].Type));
        Assert.Equal(new byte[] { 0x00, 0x06, 0x00, 0x00, 0x00, 0x08, 0x00, 0x01 }, rr[4].Data[^8..]);
    }

    [Fact]
    public void Goodbye_sets_every_ttl_to_zero()
    {
        Assert.All(ParseAnswers(Records.BuildAnnouncement(goodbye: true)), r => Assert.Equal(0u, r.Ttl));
    }

    private static byte[] Query(params (string Name, ushort Type)[] questions)
    {
        using var ms = new MemoryStream();
        byte[] header = new byte[12];
        BinaryPrimitives.WriteUInt16BigEndian(header.AsSpan(4), (ushort)questions.Length);
        ms.Write(header);
        foreach ((string name, ushort type) in questions)
        {
            ms.Write(AirDropServiceRecords.EncodeName(name));
            ms.Write([(byte)(type >> 8), (byte)type, 0, 1]);
        }
        return ms.ToArray();
    }

    [Theory]
    [InlineData("_airdrop._tcp.local", 12, true)]
    [InlineData("_AirDrop._TCP.local", 12, true)]
    [InlineData("0123456789ab._airdrop._tcp.local", 33, true)]
    [InlineData("0123456789ab._airdrop._tcp.local", 16, true)]
    [InlineData("0123456789ab.local", 28, true)]
    [InlineData("0123456789ab.local", 1, true)]
    [InlineData("_airdrop._tcp.local", 255, true)]
    [InlineData("_ipp._tcp.local", 12, false)]
    [InlineData("ffffffffffff._airdrop._tcp.local", 33, false)]
    [InlineData("_airdrop._tcp.local", 28, false)]
    public void Answers_only_questions_about_our_records(string name, ushort type, bool expected) =>
        Assert.Equal(expected, Records.IsQueryForUs(Query((name, type))));

    [Fact]
    public void Finds_our_question_among_others_like_the_phone_sends()
    {
        Assert.True(Records.IsQueryForUs(Query(
            ("_applicationServicePairing._tcp.local", 12), ("_appSvcPrePair._tcp.local", 12), ("_airdrop._tcp.local", 12))));
    }

    [Fact]
    public void Ignores_responses_and_malformed_packets()
    {
        Assert.False(Records.IsQueryForUs(Records.BuildAnnouncement()));
        Assert.False(Records.IsQueryForUs([1, 2, 3]));
        byte[] loop = [.. new byte[4], 0, 1, .. new byte[6], 0xC0, 12, 0, 12, 0, 1];   // name pointer to itself
        Assert.False(Records.IsQueryForUs(loop));
        byte[] good = Query(("_airdrop._tcp.local", 12));
        var rng = new Random(7);
        for (int i = 0; i < 2000; i++)
        {
            byte[] m = (byte[])good.Clone();
            m[rng.Next(m.Length)] = (byte)rng.Next(256);
            Records.IsQueryForUs(m.AsSpan(0, rng.Next(m.Length + 1)));
        }
    }

    [Fact]
    public void Service_id_is_twelve_lowercase_hex_characters() =>
        Assert.Matches("^[0-9a-f]{12}$", AirDropServiceRecords.NewServiceId());
}
