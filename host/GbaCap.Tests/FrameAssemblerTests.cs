using GbaCap;

namespace GbaCap.Tests;

// ラインパケット列 → フレーム(240×160 RGB888)復元のテスト。
public class FrameAssemblerTests
{
    private const int Width = 240;
    private const int Height = 160;
    private const ushort GreenMax = (1 << 11) | (1 << 12) | (1 << 13) | (1 << 14) | (1 << 15);

    private static LinePacket Line(byte frameNo, ushort lineNo, ushort fill = 0)
    {
        var samples = new ushort[Width];
        Array.Fill(samples, fill);
        return new LinePacket(frameNo, lineNo, samples);
    }

    [Fact]
    public void 百六十ライン揃うとフレームが完成する()
    {
        var asm = new FrameAssembler();
        var results = Enumerable.Range(0, Height)
            .Select(n => asm.Push(Line(0, (ushort)n))).ToArray();
        Assert.All(results[..^1], r => Assert.Null(r));
        var frame = results[^1];
        Assert.NotNull(frame);
        Assert.Equal(Height, frame.Pixels.GetLength(0));
        Assert.Equal(Width, frame.Pixels.GetLength(1));
    }

    [Fact]
    public void ピクセルはRGB888にデコードされる()
    {
        var asm = new FrameAssembler();
        Frame? frame = null;
        for (var n = 0; n < Height; n++)
            frame = asm.Push(Line(0, (ushort)n, GreenMax));
        Assert.NotNull(frame);
        Assert.Equal((0, 255, 0), frame.Pixels[0, 0]);
    }

    [Fact]
    public void ライン欠落したフレームは完成扱いにならず次フレームに進む()
    {
        var asm = new FrameAssembler();
        for (var n = 0; n < Height - 1; n++) // line 159 が欠落したままフレーム1へ
            asm.Push(Line(0, (ushort)n));
        Frame? frame = null;
        for (var n = 0; n < Height; n++)
            frame = asm.Push(Line(1, (ushort)n));
        Assert.NotNull(frame);
        Assert.Equal(1, frame.FrameNo);
        Assert.Equal(1, asm.DroppedFrames);
    }
}
