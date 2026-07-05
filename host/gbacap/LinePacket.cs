using System.Buffers.Binary;

namespace GbaCap;

/// <summary>
/// USB ラインパケット v3 (484B):
/// [0xA5][フレーム番号][ライン番号u16LE][サンプル240×u16LE]
/// サンプルは FW が赤下位2bitを bit2/bit6 に合成済みの15bit値。
/// </summary>
public sealed record LinePacket(byte FrameNo, ushort LineNo, ushort[] Samples)
{
    public const byte Magic = 0xA5;
    public const int SamplesPerLine = 240;
    public const int PacketSize = 4 + SamplesPerLine * 2; // 484

    /// <summary>484Bのラインパケットを解析する。マジック不一致は null。サイズ不正は例外。</summary>
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
        return new LinePacket(data[1], lineNo, samples);
    }
}
