#!/usr/bin/env python3
"""書き込み済みのアプリに COM6 経由で 1 文字コマンドを送り、ログを集める。

アプリ側(app_main.c)が UART0 の受信で次を受け付ける。
    r = 再起動 / i = 状態表示 / b = エンコーダベンチ / t = 録画 ON・OFF
コマンドはメインループが 1 秒ごとに拾うので、反映まで最大 1 秒かかる。
b は 600 枚で 10 秒ほど止まるので、ログ秒数は長めに取る。

使い方:
    python p4cmd.py i              # 状態表示を 6 秒集める
    python p4cmd.py b 40 out.log   # ベンチを走らせて 40 秒集める
    python p4cmd.py t 20           # 録画を開始して 20 秒集める
注意: COM6 は同時に 1 つしか開けない。書き込み待ちスクリプトと重ねないこと。
"""

import sys
import time

import serial

PORT = "COM6"
BAUD = 115200


def main():
    if len(sys.argv) < 2:
        print("使い方: python p4cmd.py <コマンド文字列> [ログ秒数] [出力ファイル]")
        return 1
    cmds = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0
    out = sys.argv[3] if len(sys.argv) > 3 else None

    s = serial.Serial(PORT, BAUD, timeout=0)
    s.dtr = False
    s.rts = False
    time.sleep(0.2)
    s.reset_input_buffer()

    for c in cmds:
        s.write(c.encode("ascii"))
        s.flush()
        time.sleep(0.05)
    print(f"コマンド {cmds!r} を送った。{secs} 秒集める", flush=True)

    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < secs:
        n = s.in_waiting
        if n:
            buf += s.read(n)
        else:
            time.sleep(0.01)
    s.close()

    if out:
        with open(out, "wb") as f:
            f.write(bytes(buf))
        print(f"saved {len(buf)} bytes -> {out}", flush=True)
    text = bytes(buf).decode("utf-8", "replace")
    print(text[-4000:], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
