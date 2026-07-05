using System.Buffers.Binary;

namespace GbaCap;

/// <summary>
/// USB ラインパケット v2 (724B):
/// [0xA5][フレーム番号][ライン番号u16LE][GPIOBサンプル240×u16LE][GPIOC下位バイト240×u8]
/// GPIOC バイトは bit4=LDR1(赤bit0), bit5=LDR2(赤bit1)。フォーマットの正は docs/architecture.md。
/// </summary>
public sealed record LinePacket(byte FrameNo, ushort LineNo, ushort[] Samples, byte[] RedLow)
{
    public const byte Magic = 0xA5;
    public const int SamplesPerLine = 240;
    public const int PacketSize = 4 + SamplesPerLine * 2 + SamplesPerLine; // 724

    /// <summary>724Bのラインパケットを解析する。マジック不一致は null。サイズ不正は例外。</summary>
    public static LinePacket? Parse(ReadOnlySpan<byte> data)
    {
        if (data.Length != PacketSize)
            throw new ArgumentException($"packet size {data.Length} != {PacketSize}");
        if (data[0] != Magic)
            return null;

        var lineNo = BinaryPrimitives.ReadUInt16LittleEndian(data[2..4]);
        var samples = new ushort[SamplesPerLine];
        for (var i = 0; i < SamplesPerLine; i++)
            samples[i] = BinaryPrimitives.ReadUInt16LittleEndian(data.Slice(4 + i * 2, 2));
        var redLow = data.Slice(4 + SamplesPerLine * 2, SamplesPerLine).ToArray();
        return new LinePacket(data[1], lineNo, samples, redLow);
    }
}
