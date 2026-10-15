"""COM6 を 1 プロセスで開いたまま、決まった時刻にコマンドを送り、
届いたログを全行タイムスタンプ付きでファイルに残す。

p4watch.py + p4send.py の組み合わせだと
  - 両方が COM6 を開こうとして競合する
  - p4watch は起動バナーの 6 秒無音で抜けるので、その後の e/t/p/i の結果が残らない
  - コマンドを送った時刻がログに残らない
という問題があった(監督 2026-10-16 指示でログの取り方を見直す)。

使い方:
  python p4seq.py <ログファイル> <cmd>@<秒> [<cmd>@<秒> ...]
  例: python p4seq.py D:\\esp\\scripts\\r7.log e@2 p@22 i@23 e@24 t@28 p@48 i@49 t@50
終りは最後の予定時刻 + 末尾秒(既定 8 秒)。"""
import datetime
import sys
import time

import serial

logf = sys.argv[1]
plan = []
for a in sys.argv[2:]:
    cmd, _, sec = a.rpartition("@")
    plan.append((cmd, float(sec)))
plan.sort(key=lambda x: x[1])
if not plan:
    print("予定がない。e@2 のように cmd@秒 を渡す")
    sys.exit(1)

port = "COM6"
BAUD = 115200
tail_secs = 8.0

t0 = time.time()


def stamp():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


def rel():
    return time.time() - t0


f = open(logf, "ab", buffering=0)


def emit(line):
    f.write(("%8.3f %s %s\n" % (rel(), stamp(), line)).encode("utf-8", "replace"))


s = serial.Serial(port, BAUD, timeout=0.2)
s.dtr = False
s.rts = False
time.sleep(0.3)
s.reset_input_buffer()
emit("=== p4seq 開始 %s %d ===" % (port, BAUD))

end = plan[-1][1] + tail_secs
linebuf = ""
done = set()
while rel() < end:
    for i, (cmd, sec) in enumerate(plan):
        if i in done or rel() < sec:
            continue
        done.add(i)
        s.write((cmd + "\r").encode())
        s.flush()
        emit(">>> SEND '%s'" % cmd)
    b = s.read(256)
    if b:
        linebuf += b.decode("utf-8", "replace")
        while "\n" in linebuf:
            line, linebuf = linebuf.split("\n", 1)
            emit(line.rstrip("\r"))
if linebuf.strip():
    emit(linebuf.rstrip("\r"))
emit("=== p4seq 終了 ===")
s.close()
f.close()
print("OK ログ=%s" % logf, flush=True)
