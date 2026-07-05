"""USB ラインパケット解析のテスト。

フォーマット (docs/architecture.md):
  ヘッダ4B: マジック 0xA5, フレーム番号 uint8, ライン番号 uint16(LE)
  ペイロード: 240 サンプル × uint16(LE) = 480B
  計 484B
"""
import pytest

from gbacap.packet import LINE_PACKET_SIZE, parse_line_packet


def make_packet(frame_no=0, line_no=0, samples=None):
    samples = samples if samples is not None else [0] * 240
    body = b"".join(s.to_bytes(2, "little") for s in samples)
    return bytes([0xA5, frame_no]) + line_no.to_bytes(2, "little") + body


class Testラインパケット解析:
    def test_パケットサイズは484バイト(self):
        assert LINE_PACKET_SIZE == 484

    def test_ヘッダからフレーム番号とライン番号を取り出せる(self):
        pkt = parse_line_packet(make_packet(frame_no=7, line_no=159))
        assert (pkt.frame_no, pkt.line_no) == (7, 159)

    def test_サンプル240個が取り出せる(self):
        samples = list(range(240))
        pkt = parse_line_packet(make_packet(samples=samples))
        assert list(pkt.samples) == samples

    def test_マジック不一致はNoneを返す(self):
        data = bytearray(make_packet())
        data[0] = 0x5A
        assert parse_line_packet(bytes(data)) is None

    def test_サイズ不足はValueError(self):
        with pytest.raises(ValueError):
            parse_line_packet(make_packet()[:-1])
