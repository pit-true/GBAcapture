"""ラインパケット列から 240×160 RGB888 フレームを組み立てる。"""
from dataclasses import dataclass

from gbacap.decode import decode_gpiob_sample
from gbacap.packet import SAMPLES_PER_LINE, LinePacket

LINES_PER_FRAME = 160


@dataclass(frozen=True)
class Frame:
    frame_no: int
    pixels: tuple  # pixels[y][x] = (r, g, b)


class FrameAssembler:
    """ラインを push し、フレームが完成したら Frame を返す（未完成なら None）。

    ライン欠落したフレームは破棄し dropped_frames に計上する。
    """

    def __init__(self):
        self._current_frame_no = None
        self._lines = {}
        self.dropped_frames = 0

    def push(self, pkt: LinePacket) -> Frame | None:
        if pkt.frame_no != self._current_frame_no:
            if self._lines:
                self.dropped_frames += 1
            self._current_frame_no = pkt.frame_no
            self._lines = {}

        self._lines[pkt.line_no] = tuple(
            decode_gpiob_sample(s) for s in pkt.samples[:SAMPLES_PER_LINE]
        )

        if len(self._lines) == LINES_PER_FRAME:
            frame = Frame(
                frame_no=pkt.frame_no,
                pixels=tuple(self._lines[y] for y in range(LINES_PER_FRAME)),
            )
            self._current_frame_no = None
            self._lines = {}
            return frame
        return None
