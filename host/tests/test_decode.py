"""GPIOB サンプル値 → RGB888 デコードのテスト。

ビット割り当ては docs/wiring.md の GPIOB INDR 表が正:
  bit0=R3 bit1=R4 bit10=R5 / bit11..15=G1..G5 / bit9=B1 bit8=B2 bit5=B3 bit4=B4 bit3=B5
LDx1 を LSB と仮定（要実測確認）。GPIOB のみなので赤の下位2bit(R1,R2)は欠落し 0 扱い。
"""
from gbacap.decode import decode_gpiob_sample


class TestGPIOBサンプルのデコード:
    def test_全ビット0なら黒(self):
        assert decode_gpiob_sample(0x0000) == (0, 0, 0)

    def test_緑の全ビットが立つと緑は255(self):
        # G1..G5 = bit11..15
        r, g, b = decode_gpiob_sample(0b1111_1000_0000_0000)
        assert g == 255
        assert (r, b) == (0, 0)

    def test_青の全ビットが立つと青は255(self):
        # B1..B5 = bit9, bit8, bit5, bit4, bit3
        sample = (1 << 9) | (1 << 8) | (1 << 5) | (1 << 4) | (1 << 3)
        assert decode_gpiob_sample(sample) == (0, 0, 255)

    def test_赤はR3toR5のみで下位2bitは0扱い(self):
        # R3..R5 = bit0, bit1, bit10 → 5bit値 0b11100 = 28 → 8bit拡張 (28<<3)|(28>>2) = 231
        sample = (1 << 0) | (1 << 1) | (1 << 10)
        assert decode_gpiob_sample(sample) == (231, 0, 0)

    def test_5bit中間値の8bit拡張(self):
        # G=0b00001 (G1=bit11のみ) → (1<<3)|(1>>2) = 8
        assert decode_gpiob_sample(1 << 11) == (0, 8, 0)

    def test_無関係ビットは無視される(self):
        # bit2, bit6, bit7 は未接続
        sample = (1 << 2) | (1 << 6) | (1 << 7)
        assert decode_gpiob_sample(sample) == (0, 0, 0)
