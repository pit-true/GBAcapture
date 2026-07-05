"""GPIO サンプル値から RGB888 へのデコード。

ビット割り当ての正は docs/wiring.md。LDx1=LSB 仮定（要実測確認）。
"""

# GPIOB->INDR のビット位置 → 各色5bit値内のビット位置
_RED_BITS = ((0, 2), (1, 3), (10, 4))  # R1,R2 は GPIOC のため欠落（0扱い）
_GREEN_BITS = ((11, 0), (12, 1), (13, 2), (14, 3), (15, 4))
_BLUE_BITS = ((9, 0), (8, 1), (5, 2), (4, 3), (3, 4))


def _extract5(sample: int, bit_map) -> int:
    value = 0
    for src, dst in bit_map:
        value |= ((sample >> src) & 1) << dst
    return value


def _expand5to8(value: int) -> int:
    return (value << 3) | (value >> 2)


def decode_gpiob_sample(sample: int) -> tuple[int, int, int]:
    """GPIOB->INDR の16bitサンプル1つを (R, G, B) 各8bitに変換する。"""
    return (
        _expand5to8(_extract5(sample, _RED_BITS)),
        _expand5to8(_extract5(sample, _GREEN_BITS)),
        _expand5to8(_extract5(sample, _BLUE_BITS)),
    )
