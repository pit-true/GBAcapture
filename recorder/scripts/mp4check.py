#!/usr/bin/env python3
"""基板が書いた mp4 を、再生機を使わずに箱(ボックス)単位で検査する。

使い方:
    python mp4check.py REC_0001.mp4
    python mp4check.py REC_0001.mp4 --samples 8

見るもの
    - moov が末尾にあるか(電源断で失われる前提。停止できていないファイルは moov がない)
    - 映像トラックが avc1 で、SPS/PPS が avcC に入っているか
    - 映像トラックの幅・高さ(960x640 になっているか)
    - サンプル数と平均ビットレート(60fps で書けているか)
    - 同期フレーム(stss)の数。スライス切り替え直後に IDR が入っていれば 1 増える
"""

import struct
import sys

CONTAINERS = {b"moov", b"trak", b"mdia", b"minf", b"stbl", b"edts", b"udta", b"mvex"}


def boxes(buf, start, end):
    """buf[start:end] の並びから (type, body_start, body_end, box_end) を順に出す。"""
    p = start
    while p + 8 <= end:
        size = struct.unpack_from(">I", buf, p)[0]
        typ = bytes(buf[p + 4:p + 8])
        head = 8
        if size == 1:
            size = struct.unpack_from(">Q", buf, p + 8)[0]
            head = 16
        elif size == 0:
            size = end - p
        if size < head or p + size > end:
            break
        yield typ, p + head, p + size, p + size
        p += size


def find(buf, path, start=0, end=None):
    """moov/trak/tkhd のようなパスでボックスを探す。"""
    if end is None:
        end = len(buf)
    if not path:
        return [(start, end)]
    want = path[0]
    out = []
    for typ, bs, be, _ in boxes(buf, start, end):
        if typ == want:
            if len(path) == 1:
                out.append((bs, be))
            else:
                out.extend(find(buf, path[1:], bs, be))
    return out


def find_any(buf, name, start, end):
    """深さを気にせず、範囲内から名前のボックスを全部集める。"""
    out = []
    for typ, bs, be, _ in boxes(buf, start, end):
        if typ == name:
            out.append((bs, be))
        out.extend(find_any(buf, name, bs, be))
    return out


def read_full_header(buf, pos):
    ver = buf[pos]
    flags = buf[pos + 1:pos + 4]
    return ver, flags, pos + 4


def describe_trak(buf, trak_start, trak_end, show_samples):
    tk = find_any(buf, b"tkhd", trak_start, trak_end)
    if tk:
        s = tk[0][0]
        ver, _, p = read_full_header(buf, s)
        if ver == 1:
            p += 8 + 8 + 4 + 4 + 8
        else:
            p += 4 + 4 + 4 + 4 + 4
        p += 8 + 8 + 36  # 予約 + layer/alt/vol/予約 + 変換行列
        w = struct.unpack_from(">I", buf, p)[0] >> 16
        h = struct.unpack_from(">I", buf, p + 4)[0] >> 16
        print(f"    映像/音声の大きさ {w}x{h}")

    md = find_any(buf, b"mdhd", trak_start, trak_end)
    if md:
        s = md[0][0]
        ver, _, p = read_full_header(buf, s)
        if ver == 1:
            p += 16
            dur = struct.unpack_from(">Q", buf, p + 8)[0]
            ts = struct.unpack_from(">I", buf, p)[0]
        else:
            p += 8
            dur = struct.unpack_from(">I", buf, p + 4)[0]
            ts = struct.unpack_from(">I", buf, p)[0]
        print(f"    タイムスケール {ts}  長さ {dur} -> {dur / max(ts, 1):.2f} 秒")

    hl = find_any(buf, b"hdlr", trak_start, trak_end)
    kind = b"????"
    if hl:
        kind = bytes(buf[hl[0][0] + 8:hl[0][0] + 12])
    print(f"    ハンドラ {kind.decode('ascii', 'replace')}")

    sd = find_any(buf, b"stsd", trak_start, trak_end)
    if sd:
        p = sd[0][0] + 8  # full header + entry count
        size = struct.unpack_from(">I", buf, p)[0]
        fourcc = bytes(buf[p + 4:p + 8])
        print(f"    コーデック {fourcc.decode('ascii', 'replace')}")
        if fourcc == b"avc1":
            # avcC をボディから直接探す(オフセット計算より確実)
            body_s, body_e = sd[0][0], sd[0][1]
            i = buf.find(b"avcC", body_s, body_e)
            if i > 0:
                q = i + 4
                sps_n = buf[q + 5] & 0x1F
                off = q + 6
                for k in range(sps_n):
                    ln = struct.unpack_from(">H", buf, off)[0]
                    sps = bytes(buf[off + 2:off + 2 + ln])
                    print(f"    SPS[{k}] {ln} バイト {sps[:8].hex()}")
                    off += 2 + ln
                pps_n = buf[off]
                off += 1
                for k in range(pps_n):
                    ln = struct.unpack_from(">H", buf, off)[0]
                    pps = bytes(buf[off + 2:off + 2 + ln])
                    print(f"    PPS[{k}] {ln} バイト {pps[:8].hex()}")
                    off += 2 + ln

    st = find_any(buf, b"stsz", trak_start, trak_end)
    total = cnt = 0
    if st:
        p = st[0][0]
        _, _, p = read_full_header(buf, p)
        sample_size = struct.unpack_from(">I", buf, p)[0]
        cnt = struct.unpack_from(">I", buf, p + 4)[0]
        if sample_size:
            total = sample_size * cnt
        else:
            total = sum(struct.unpack_from(">I", buf, p + 8 + 4 * k)[0]
                        for k in range(cnt))
        print(f"    サンプル {cnt} 個  データ {total} バイト")

    ts = find_any(buf, b"stts", trak_start, trak_end)
    if ts and cnt:
        p = ts[0][0]
        _, _, p = read_full_header(buf, p)
        n = struct.unpack_from(">I", buf, p)[0]
        first = struct.unpack_from(">II", buf, p + 4)
        print(f"    stts エントリ {n}  先頭 {first[0]} 個 × 間隔 {first[1]}")

    ks = find_any(buf, b"stss", trak_start, trak_end)
    if ks:
        p = ks[0][0]
        _, _, p = read_full_header(buf, p)
        n = struct.unpack_from(">I", buf, p)[0]
        nums = [struct.unpack_from(">I", buf, p + 4 + 4 * k)[0] for k in range(min(n, 12))]
        print(f"    同期フレーム {n} 個  先頭 {nums}")

    if show_samples and st:
        p = st[0][0]
        _, _, p = read_full_header(buf, p)
        if struct.unpack_from(">I", buf, p)[0] == 0:
            sizes = [struct.unpack_from(">I", buf, p + 8 + 4 * k)
                     [0] for k in range(min(cnt, show_samples))]
            print(f"    先頭 {len(sizes)} サンプルのサイズ {sizes}")


def main():
    if len(sys.argv) < 2:
        print("使い方: python mp4check.py <mp4> [--samples N]")
        return 1
    path = sys.argv[1]
    show = 0
    if "--samples" in sys.argv:
        show = int(sys.argv[sys.argv.index("--samples") + 1])
    with open(path, "rb") as f:
        buf = f.read()
    print(f"{path}  {len(buf)} バイト")

    top = [(t, bs, be) for t, bs, be, _ in boxes(buf, 0, len(buf))]
    print("トップレベル: " + " ".join(
        f"{t.decode('ascii', 'replace')}({be - bs})" for t, bs, be in top))

    moov = find(buf, [b"moov"])
    if not moov:
        print("moov がない = この mp4 は途中で切れている(停止できていない)")
        return 1
    ms, me = moov[0]
    traks = find_any(buf, b"trak", ms, me)
    print(f"トラック数 {len(traks)}")
    for i, (s, e) in enumerate(traks):
        print(f"  trak[{i}]")
        describe_trak(buf, s, e, show)
    return 0


if __name__ == "__main__":
    sys.exit(main())
