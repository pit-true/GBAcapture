using GbaCap;

namespace GbaCap.Tests;

// 24bit BMP エンコードのテスト。依存ライブラリなしで確実に保存できる形式として採用。
public class BmpWriterTests
{
    private static (byte R, byte G, byte B)[,] SolidImage(int h, int w, (byte, byte, byte) c)
    {
        var px = new (byte R, byte G, byte B)[h, w];
        for (var y = 0; y < h; y++)
            for (var x = 0; x < w; x++)
                px[y, x] = c;
        return px;
    }

    [Fact]
    public void ヘッダはBMマジックで始まりファイルサイズが正しい()
    {
        // 2x2: ピクセル行 = 2px*3B = 6B → 4B境界へパディングで8B/行、計 54 + 16 = 70B
        var data = BmpWriter.Encode(SolidImage(2, 2, (0, 0, 0)));
        Assert.Equal((byte)'B', data[0]);
        Assert.Equal((byte)'M', data[1]);
        Assert.Equal(70, data.Length);
        Assert.Equal(70u, BitConverter.ToUInt32(data, 2));
    }

    [Fact]
    public void 幅と高さがヘッダに入る()
    {
        var data = BmpWriter.Encode(SolidImage(160, 240, (0, 0, 0)));
        Assert.Equal(240, BitConverter.ToInt32(data, 18));
        Assert.Equal(160, BitConverter.ToInt32(data, 22));
    }

    [Fact]
    public void 左上のピクセルはBGR順で最終行位置に書かれる()
    {
        // BMPは下から上へ格納。左上(0,0)=赤 は データ末尾行の先頭3バイト
        var px = SolidImage(2, 2, (0, 0, 0));
        px[0, 0] = (255, 10, 20); // R=255, G=10, B=20
        var data = BmpWriter.Encode(px);
        var lastRow = 54 + 8; // ピクセル部54開始、行8B、最終行(=画像の最上行)は54+8
        Assert.Equal(20, data[lastRow + 0]);  // B
        Assert.Equal(10, data[lastRow + 1]);  // G
        Assert.Equal(255, data[lastRow + 2]); // R
    }
}
