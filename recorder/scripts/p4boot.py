#!/usr/bin/env python3
"""ESP32-P4-Pico をダウンロードループから起こしてアプリを起動し、コンソールログを取る。

このボードには DTR/RTS の自動リセットがない。そのため書き込み後にリセットが効かず、
ROM が前回合意したボーレート(多くの場合 460800)のダウンロードループで沈黙することがある。
esptool は --baud を初期同期に反映しない(--before no_reset_no_sync のときだけ反映)ので、
通常オプションではその ROM に繋がらない。このスクリプトは esptool の Python API で
ボーを総当たりして接続し、115200 に統一してから RUN を送る。

使い方:
    python p4boot.py [ログ秒数] [出力ファイル]
"""

import sys
import time

import serial
from esptool.cmds import detect_chip

PORT = "COM6"
BAUDS = (115200, 460800, 921600, 230400, 1500000)
TARGET_BAUD = 115200


def connect():
    for baud in BAUDS:
        try:
            esp = detect_chip(
                port=PORT,
                baud=baud,
                connect_mode="no_reset_no_sync",
                connect_attempts=2,
            )
        except BaseException as exc:  # noqa: BLE001 - 接続試行なので何でも拾う
            print(f"  @{baud}: {exc}", flush=True)
            continue
        print(f"connected @{baud}", flush=True)
        return esp
    return None


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
    out = sys.argv[2] if len(sys.argv) > 2 else "applog.log"

    esp = connect()
    if esp is None:
        print("ROM が応答しない。BOOT+RST でダウンロードモードに入れてから実行すること。")
        return 1

    try:
        if esp._port.baudrate != TARGET_BAUD:
            esp.change_baud(TARGET_BAUD)
        esp.run(reboot=True)
        print("RUN を送った。チップはフラッシュから起動するはず。", flush=True)
    except BaseException as exc:  # noqa: BLE001
        print("RUN 失敗:", exc, flush=True)
    finally:
        try:
            esp._port.close()
        except Exception:
            pass

    time.sleep(0.2)
    s = serial.Serial(PORT, TARGET_BAUD, timeout=0)
    s.dtr = False
    s.rts = False
    time.sleep(0.1)
    s.reset_input_buffer()
    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < secs:
        n = s.in_waiting
        if n:
            buf += s.read(n)
        else:
            time.sleep(0.01)
    s.close()
    with open(out, "wb") as f:
        f.write(bytes(buf))
    print(f"captured {len(buf)} bytes -> {out}", flush=True)
    if buf:
        print(bytes(buf[:600]).decode("utf-8", "replace"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
