using GbaCap;

namespace GbaCap.Tests;

// USB ラインパケット v2 解析のテスト。
// フォーマット (docs/architecture.md):
//   ヘッダ4B: マジック 0xA5, フレーム番号 uint8, ライン番号 uint16(LE)
//   ペイロード: GPIOB 240×uint16(LE)=480B + GPIOC下位バイト 240×uint8=240B、計 724B
public class LinePacketTests
{
    private static byte[] MakePacket(byte frameNo = 0, ushort lineNo = 0,
        ushort[]? samples = null, byte[]? redLow = null)
    {
        samples ??= new ushort[240];
        redLow ??= new byte[240];
        var data = new byte[4 + 480 + 240];
        data[0] = 0xA5;
        data[1] = frameNo;
        data[2] = (byte)(lineNo & 0xFF);
        data[3] = (byte)(lineNo >> 8);
        for (var i = 0; i < samples.Length; i++)
        {
            data[4 + i * 2] = (byte)(samples[i] & 0xFF);
            data[5 + i * 2] = (byte)(samples[i] >> 8);
        }
        redLow.CopyTo(data, 484);
        return data;
    }

    [Fact]
    public void パケットサイズは724バイト()
    {
        Assert.Equal(724, LinePacket.PacketSize);
    }

    [Fact]
    public void ヘッダからフレーム番号とライン番号を取り出せる()
    {
        var pkt = LinePacket.Parse(MakePacket(frameNo: 7, lineNo: 159));
        Assert.NotNull(pkt);
        Assert.Equal((7, 159), ((int)pkt.FrameNo, (int)pkt.LineNo));
    }

    [Fact]
    public void サンプル240個と赤下位バイト240個が取り出せる()
    {
        var samples = Enumerable.Range(0, 240).Select(i => (ushort)i).ToArray();
        var redLow = Enumerable.Range(0, 240).Select(i => (byte)(i & 0x30)).ToArray();
        var pkt = LinePacket.Parse(MakePacket(samples: samples, redLow: redLow));
        Assert.NotNull(pkt);
        Assert.Equal(samples, pkt.Samples);
        Assert.Equal(redLow, pkt.RedLow);
    }

    [Fact]
    public void マジック不一致はnullを返す()
    {
        var data = MakePacket();
        data[0] = 0x5A;
        Assert.Null(LinePacket.Parse(data));
    }

    [Fact]
    public void サイズ不足は例外()
    {
        Assert.Throws<ArgumentException>(() => LinePacket.Parse(MakePacket()[..^1]));
    }
}
