namespace GbaCap;

/// <summary>完成した 240×160 RGB888 フレーム。Pixels[y, x] = (R, G, B)。</summary>
public sealed record Frame(byte FrameNo, (byte R, byte G, byte B)[,] Pixels);

/// <summary>
/// ラインパケットを Push し、フレームが完成したら Frame を返す（未完成なら null）。
/// ライン欠落したフレームは破棄し DroppedFrames に計上する。
/// </summary>
public sealed class FrameAssembler
{
    public const int LinesPerFrame = 160;

    private readonly Dictionary<int, (byte R, byte G, byte B)[]> _lines = new();
    private int? _currentFrameNo;

    public int DroppedFrames { get; private set; }

    public Frame? Push(LinePacket pkt)
    {
        if (pkt.FrameNo != _currentFrameNo)
        {
            if (_lines.Count > 0)
                DroppedFrames++;
            _currentFrameNo = pkt.FrameNo;
            _lines.Clear();
        }

        var line = new (byte R, byte G, byte B)[LinePacket.SamplesPerLine];
        for (var x = 0; x < LinePacket.SamplesPerLine; x++)
            line[x] = PixelDecoder.DecodeGpiobSample(pkt.Samples[x]);
        _lines[pkt.LineNo] = line;

        if (_lines.Count != LinesPerFrame)
            return null;

        var pixels = new (byte R, byte G, byte B)[LinesPerFrame, LinePacket.SamplesPerLine];
        for (var y = 0; y < LinesPerFrame; y++)
        {
            var row = _lines[y];
            for (var x = 0; x < LinePacket.SamplesPerLine; x++)
                pixels[y, x] = row[x];
        }
        _currentFrameNo = null;
        _lines.Clear();
        return new Frame(pkt.FrameNo, pixels);
    }
}
