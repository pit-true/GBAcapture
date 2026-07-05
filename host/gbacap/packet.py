"""USB ラインパケットの解析。フォーマットの正は docs/architecture.md。"""
import struct
from dataclasses import dataclass

MAGIC = 0xA5
SAMPLES_PER_LINE = 240
LINE_PACKET_SIZE = 4 + SAMPLES_PER_LINE * 2  # ヘッダ4B + uint16×240


@dataclass(frozen=True)
class LinePacket:
    frame_no: int
    line_no: int
    samples: tuple


def parse_line_packet(data: bytes) -> LinePacket | None:
    """484Bのラインパケットを解析する。マジック不一致は None。サイズ不正は ValueError。"""
    if len(data) != LINE_PACKET_SIZE:
        raise ValueError(f"packet size {len(data)} != {LINE_PACKET_SIZE}")
    magic, frame_no, line_no = struct.unpack_from("<BBH", data)
    if magic != MAGIC:
        return None
    samples = struct.unpack_from(f"<{SAMPLES_PER_LINE}H", data, 4)
    return LinePacket(frame_no=frame_no, line_no=line_no, samples=samples)
