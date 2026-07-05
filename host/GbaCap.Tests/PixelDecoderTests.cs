using GbaCap;

namespace GbaCap.Tests;

// ラインパケットv3 サンプル値 → RGB888 デコードのテスト。
// ビット割り当て: bit2=R1 bit6=R2 (FWが合成) / bit0=R3 bit1=R4 bit10=R5
//   bit11..15=G1..G5 / bit9=B1 bit8=B2 bit5=B3 bit4=B4 bit3=B5 (docs/wiring.md)
public class PixelDecoderTests
{
    [Fact]
    public void 全ビット0なら黒()
    {
        Assert.Equal((0, 0, 0), PixelDecoder.DecodeSample(0x0000));
    }

    [Fact]
    public void 緑の全ビットが立つと緑は255()
    {
        var (r, g, b) = PixelDecoder.DecodeSample(0b1111_1000_0000_0000);
        Assert.Equal(255, g);
        Assert.Equal((0, 0), (r, b));
    }

    [Fact]
    public void 青の全ビットが立つと青は255()
    {
        ushort sample = (1 << 9) | (1 << 8) | (1 << 5) | (1 << 4) | (1 << 3);
        Assert.Equal((0, 0, 255), PixelDecoder.DecodeSample(sample));
    }

    [Fact]
    public void 赤の全5bitで255()
    {
        // R1=bit2, R2=bit6, R3=bit0, R4=bit1, R5=bit10
        ushort sample = (1 << 2) | (1 << 6) | (1 << 0) | (1 << 1) | (1 << 10);
        Assert.Equal((255, 0, 0), PixelDecoder.DecodeSample(sample));
    }

    [Fact]
    public void 赤下位2bitのみで24()
    {
        // R1,R2のみ → 5bit値 3 → (3<<3)|(3>>2) = 24
        Assert.Equal((24, 0, 0), PixelDecoder.DecodeSample((1 << 2) | (1 << 6)));
    }

    [Fact]
    public void 五bit中間値の8bit拡張()
    {
        // G=0b00001 (G1=bit11のみ) → (1<<3)|(1>>2) = 8
        Assert.Equal((0, 8, 0), PixelDecoder.DecodeSample(1 << 11));
    }

    [Fact]
    public void 未使用ビットは無視される()
    {
        Assert.Equal((0, 0, 0), PixelDecoder.DecodeSample(1 << 7)); // bit7 のみ未使用
    }
}
