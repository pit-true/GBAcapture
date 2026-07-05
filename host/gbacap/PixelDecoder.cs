namespace GbaCap;

/// <summary>
/// GPIO サンプル値から RGB888 へのデコード。
/// ビット割り当ての正は docs/wiring.md。LDx1=LSB 仮定（要実測確認）。
/// </summary>
public static class PixelDecoder
{
    /// <summary>
    /// ラインDMAはSPLエッジでのアーム時に最初の約2DCKを取り逃すため、
    /// セルjには「ピクセル j+SampleOffset」が入っている。表示側はこの分ずらして読む。
    /// （末尾 SampleOffset セルは未転送の残骸なので使わない）
    /// </summary>
    public const int SampleOffset = 2;

    // GPIOB->INDR のビット位置 → 各色5bit値内のビット位置
    private static readonly (int Src, int Dst)[] RedBits = [(0, 2), (1, 3), (10, 4)]; // R1,R2 は GPIOC のため欠落（0扱い）
    private static readonly (int Src, int Dst)[] GreenBits = [(11, 0), (12, 1), (13, 2), (14, 3), (15, 4)];
    private static readonly (int Src, int Dst)[] BlueBits = [(9, 0), (8, 1), (5, 2), (4, 3), (3, 4)];

    /// <summary>GPIOB->INDR の16bitサンプル1つを (R, G, B) 各8bitに変換する（赤下位2bit欠け）。</summary>
    public static (byte R, byte G, byte B) DecodeGpiobSample(ushort sample) =>
        (Expand5To8(Extract5(sample, RedBits)),
         Expand5To8(Extract5(sample, GreenBits)),
         Expand5To8(Extract5(sample, BlueBits)));

    /// <summary>GPIOBサンプル + GPIOC下位バイト(bit4=赤bit0, bit5=赤bit1) から完全15bitデコード。</summary>
    public static (byte R, byte G, byte B) DecodeSample(ushort b, byte c)
    {
        var red5 = Extract5(b, RedBits) | ((c >> 4) & 1) | (((c >> 5) & 1) << 1);
        return (Expand5To8(red5),
                Expand5To8(Extract5(b, GreenBits)),
                Expand5To8(Extract5(b, BlueBits)));
    }

    private static int Extract5(ushort sample, (int Src, int Dst)[] bitMap)
    {
        var value = 0;
        foreach (var (src, dst) in bitMap)
            value |= ((sample >> src) & 1) << dst;
        return value;
    }

    private static byte Expand5To8(int value) => (byte)((value << 3) | (value >> 2));
}
