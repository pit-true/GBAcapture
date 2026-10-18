using GbaCap;

namespace GbaCap.Tests;

// USB ラインパケット v3 解析のテスト。
//   ヘッダ4B: マジック 0xA5, フレーム番号 uint8, ライン番号 uint16(LE)
//   ペイロード: 240 サンプル × uint16(LE) = 480B、計 484B + 詰め物 28B = 512B(FW v0.34)
public class LinePacketTests
{
    private static byte[] MakePacket(byte frameNo = 0, ushort lineNo = 0, ushort[]? samples = null)
    {
        samples ??= new ushort[240];
        var data = new byte[LinePacket.PacketSize];
        data[0] = 0xA5;
        data[1] = frameNo;
        data[2] = (byte)(lineNo & 0xFF);
        data[3] = (byte)(lineNo >> 8);
        for (var i = 0; i < samples.Length; i++)
        {
            data[4 + i * 2] = (byte)(samples[i] & 0xFF);
            data[5 + i * 2] = (byte)(samples[i] >> 8);
        }
        return data;
    }

    [Fact]
    public void パケットサイズは512バイトで中身は484バイト()
    {
        Assert.Equal(512, LinePacket.PacketSize);
        Assert.Equal(484, LinePacket.PayloadSize);
    }

    [Fact]
    public void ヘッダからフレーム番号とライン番号を取り出せる()
    {
        var pkt = LinePacket.Parse(MakePacket(frameNo: 7, lineNo: 159));
        Assert.NotNull(pkt);
        Assert.Equal((7, 159), ((int)pkt.FrameNo, (int)pkt.LineNo));
    }

    [Fact]
    public void サンプル240個が取り出せる()
    {
        var samples = Enumerable.Range(0, 240).Select(i => (ushort)i).ToArray();
        var pkt = LinePacket.Parse(MakePacket(samples: samples));
        Assert.NotNull(pkt);
        Assert.Equal(samples, pkt.Samples);
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
