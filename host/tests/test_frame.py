"""ラインパケット列 → フレーム(240×160 RGB888)復元のテスト。"""
from gbacap.frame import FrameAssembler
from gbacap.packet import LinePacket

WIDTH, HEIGHT = 240, 160
GREEN_MAX = (1 << 11) | (1 << 12) | (1 << 13) | (1 << 14) | (1 << 15)


def line(frame_no, line_no, fill=0):
    return LinePacket(frame_no=frame_no, line_no=line_no, samples=(fill,) * WIDTH)


class Testフレーム復元:
    def test_160ライン揃うとフレームが完成する(self):
        asm = FrameAssembler()
        done = [asm.push(line(0, n)) for n in range(HEIGHT)]
        assert done[:-1] == [None] * (HEIGHT - 1)
        frame = done[-1]
        assert frame is not None
        assert len(frame.pixels) == HEIGHT and len(frame.pixels[0]) == WIDTH

    def test_ピクセルはRGB888にデコードされる(self):
        asm = FrameAssembler()
        frame = None
        for n in range(HEIGHT):
            frame = asm.push(line(0, n, fill=GREEN_MAX))
        assert frame.pixels[0][0] == (0, 255, 0)

    def test_ライン欠落したフレームは完成扱いにならず次フレームに進む(self):
        asm = FrameAssembler()
        for n in range(HEIGHT - 1):  # line 159 が欠落したままフレーム1へ
            asm.push(line(0, n))
        frame = None
        for n in range(HEIGHT):
            frame = asm.push(line(1, n))
        assert frame is not None
        assert frame.frame_no == 1
        assert asm.dropped_frames == 1
