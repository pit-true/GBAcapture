using System.Buffers.Binary;

namespace GbaCap;

/// <summary>24bit 無圧縮 BMP エンコーダ（依存ライブラリなしで確実に画像保存するため）。</summary>
public static class BmpWriter
{
    public static byte[] Encode((byte R, byte G, byte B)[,] pixels)
    {
        var h = pixels.GetLength(0);
        var w = pixels.GetLength(1);
        var rowSize = (w * 3 + 3) & ~3; // 4バイト境界へパディング
        var dataSize = rowSize * h;
        var fileSize = 54 + dataSize;
        var data = new byte[fileSize];

        // BITMAPFILEHEADER
        data[0] = (byte)'B'; data[1] = (byte)'M';
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(2), (uint)fileSize);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(10), 54); // ピクセル部オフセット

        // BITMAPINFOHEADER
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(14), 40);
        BinaryPrimitives.WriteInt32LittleEndian(data.AsSpan(18), w);
        BinaryPrimitives.WriteInt32LittleEndian(data.AsSpan(22), h);
        BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(26), 1);  // planes
        BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(28), 24); // bpp
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(34), (uint)dataSize);

        // ピクセル (下から上、BGR)
        for (var y = 0; y < h; y++)
        {
            var row = 54 + (h - 1 - y) * rowSize;
            for (var x = 0; x < w; x++)
            {
                var (r, g, b) = pixels[y, x];
                data[row + x * 3 + 0] = b;
                data[row + x * 3 + 1] = g;
                data[row + x * 3 + 2] = r;
            }
        }
        return data;
    }
}
