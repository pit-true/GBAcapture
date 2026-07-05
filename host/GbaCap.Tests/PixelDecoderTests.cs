using GbaCap;

namespace GbaCap.Tests;

// GPIOB サンプル値 → RGB888 デコードのテスト。
// ビット割り当ては docs/wiring.md の GPIOB INDR 表が正:
//   bit0=R3 bit1=R4 bit10=R5 / bit11..15=G1..G5 / bit9=B1 bit8=B2 bit5=B3 bit4=B4 bit3=B5
// LDx1 を LSB と仮定（要実測確認）。GPIOB のみなので赤の下位2bit(R1,R2)は欠落し 0 扱い。
public class PixelDecoderTests
{
    [Fact]
    public void 全ビット0なら黒()
    {
        Assert.Equal((0, 0, 0), PixelDecoder.DecodeGpiobSample(0x0000));
    }

    [Fact]
    public void 緑の全ビットが立つと緑は255()
    {
        var (r, g, b) = PixelDecoder.DecodeGpiobSample(0b1111_1000_0000_0000);
        Assert.Equal(255, g);
        Assert.Equal((0, 0), (r, b));
    }

    [Fact]
    public void 青の全ビットが立つと青は255()
    {
        ushort sample = (1 << 9) | (1 << 8) | (1 << 5) | (1 << 4) | (1 << 3);
        Assert.Equal((0, 0, 255), PixelDecoder.DecodeGpiobSample(sample));
    }

    [Fact]
    public void 赤はR3toR5のみで下位2bitは0扱い()
    {
        // R3..R5 = bit0, bit1, bit10 → 5bit値 0b11100 = 28 → 8bit拡張 (28<<3)|(28>>2) = 231
        ushort sample = (1 << 0) | (1 << 1) | (1 << 10);
        Assert.Equal((231, 0, 0), PixelDecoder.DecodeGpiobSample(sample));
    }

    [Fact]
    public void 五bit中間値の8bit拡張()
    {
        // G=0b00001 (G1=bit11のみ) → (1<<3)|(1>>2) = 8
        Assert.Equal((0, 8, 0), PixelDecoder.DecodeGpiobSample(1 << 11));
    }

    [Fact]
    public void 無関係ビットは無視される()
    {
        // bit2, bit6, bit7 は未接続
        ushort sample = (1 << 2) | (1 << 6) | (1 << 7);
        Assert.Equal((0, 0, 0), PixelDecoder.DecodeGpiobSample(sample));
    }
}
