namespace GbaCap;

/// <summary>
/// ラインパケットv3のサンプル値から RGB888 へのデコード。
/// FW が GPIOB 実サンプル(13bit)の空きビットに赤下位2bitを合成済み:
///   bit2=赤bit0(LDR1), bit6=赤bit1(LDR2)。その他は docs/wiring.md の GPIOB INDR 表。
/// </summary>
public static class PixelDecoder
{
    /// <summary>
    /// ラインDMAはSPLエッジでのアーム時に最初の約2DCKを取り逃すため、
    /// セルjには「ピクセル j+SampleOffset」が入っている。表示側はこの分ずらして読む。
    /// （末尾 SampleOffset セルは未転送の残骸なので使わない）
    /// </summary>
    public const int SampleOffset = 2;

    // サンプル値のビット位置 → 各色5bit値内のビット位置
    private static readonly (int Src, int Dst)[] RedBits = [(2, 0), (6, 1), (0, 2), (1, 3), (10, 4)];
    private static readonly (int Src, int Dst)[] GreenBits = [(11, 0), (12, 1), (13, 2), (14, 3), (15, 4)];
    private static readonly (int Src, int Dst)[] BlueBits = [(9, 0), (8, 1), (5, 2), (4, 3), (3, 4)];

    /// <summary>16bitサンプル1つを (R, G, B) 各8bitに変換する。</summary>
    public static (byte R, byte G, byte B) DecodeSample(ushort sample) =>
        (Expand5To8(Extract5(sample, RedBits)),
         Expand5To8(Extract5(sample, GreenBits)),
         Expand5To8(Extract5(sample, BlueBits)));

    private static int Extract5(ushort sample, (int Src, int Dst)[] bitMap)
    {
        var value = 0;
        foreach (var (src, dst) in bitMap)
            value |= ((sample >> src) & 1) << dst;
        return value;
    }

    private static byte Expand5To8(int value) => (byte)((value << 3) | (value >> 2));
}
