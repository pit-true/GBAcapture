#!/usr/bin/env python3
"""基板の microSD にあるいちばん新しい REC_NNNN.mp4 を UART で PC へ取り出す。

P4 の USB-OTG はホスト(nano)と MSC の一個取りで、MSC で読むにはケーブルを PC へ
差し替える手作業が要る。これはコンソール(COM6)だけで mp4 を取り出す道。
基板側はコンソールの 'D' で、
    1) 115200 で "DUMP <名前> <バイト数>" を 1 行出す
    2) 300ms 待って 1,500,000 へ切り替える
    3) 中身を流し、最後に CRC32(LE 4 バイト)
    4) 115200 へ戻す
20MB で約 2 分半。

使い方:
    python p4pull.py <保存先フォルダ>
注意: COM6 を開くと基板が再起動する(CH343 の DTR/RTS)。起動と SD マウントを
待ってから 'D' を送るので、始まるまで 6 秒ほどかかる。"""
import os
import sys
import time
import zlib

import serial

PORT = "COM6"
BAUD = 115200
FAST = 1500000


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(outdir, exist_ok=True)
    s = serial.Serial(PORT, BAUD, timeout=0.2)
    s.dtr = False
    s.rts = False
    time.sleep(6.0)          # 再起動 → SD マウントまで待つ
    s.reset_input_buffer()
    s.write(b"D\r")
    s.flush()

    buf = b""
    deadline = time.time() + 15
    header = None
    while time.time() < deadline:
        buf += s.read(4096)
        i = buf.find(b"\nDUMP ")
        if i >= 0:
            j = buf.find(b"\n", i + 1)
            if j > 0:
                header = buf[i + 1:j].decode("ascii", "replace").strip()
                break
    if header is None:
        print("DUMP の見出しが来ない。ログ末尾:")
        print(buf[-1500:].decode("utf-8", "replace"))
        return 1
    _, name, size_s = header.split()
    size = int(size_s)
    print(f"{name} {size} バイトを受ける", flush=True)
    s.baudrate = FAST
    s.timeout = 0.5
    s.reset_input_buffer()   # 切り替え前に紛れ込んだログを捨てる

    data = bytearray()
    want = size + 4
    t0 = time.time()
    last = t0
    while len(data) < want:
        chunk = s.read(min(65536, want - len(data)))
        if chunk:
            data += chunk
            last = time.time()
            if int(time.time() - t0) % 10 == 0 and len(chunk) > 0:
                pass
        elif time.time() - last > 5.0:
            print(f"5 秒データが来ない。{len(data)}/{want} バイトで止まった")
            break
    dt = time.time() - t0
    s.baudrate = BAUD
    s.close()

    if len(data) < want:
        return 1
    body = bytes(data[:size])
    crc_rx = int.from_bytes(data[size:size + 4], "little")
    crc = zlib.crc32(body) & 0xFFFFFFFF
    path = os.path.join(outdir, name)
    with open(path, "wb") as f:
        f.write(body)
    print(f"保存 {path} ({size} バイト, {dt:.1f} 秒, {size / dt / 1024:.0f} KB/s)")
    if crc != crc_rx:
        print(f"CRC 不一致: 受け {crc_rx:08x} / 計算 {crc:08x}")
        return 1
    print(f"CRC 一致 {crc:08x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
