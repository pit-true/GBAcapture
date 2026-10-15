# ESP32-P4-Pico のコンソールを拾い続ける。抜き差しでも終わりません。
# 起動バナー（"boot:"）を見つけたら、その後のログをまとめてポートを離れます。
# 書き込みを始める前にポートを空けるのが目的。
# 使い: python p4watch.py [秒] [ログファイル]
import re, sys, time, datetime
import serial

secs = int(sys.argv[1]) if len(sys.argv) > 1 else 900
logf = sys.argv[2] if len(sys.argv) > 2 else r"D:\esp\scripts\watch.log"
port = "COM6"
BAUD = 115200

def stamp():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]

f = open(logf, "ab", buffering=0)
def emit(line):
    f.write((line + "\n").encode("utf-8", "replace"))
    print(line, flush=True)

emit("%s p4watch %s %d 待ち開始" % (stamp(), port, BAUD))
end = time.time() + secs
total = 0
while time.time() < end:
    try:
        s = serial.Serial(port, BAUD, timeout=0.2)
        s.dtr = False
        s.rts = False
    except Exception:
        time.sleep(0.5)
        continue
    emit("%s ポートを開いた" % stamp())
    buf = b""
    booted = False
    last = time.time()
    while time.time() < end:
        b = s.read(256)
        if b:
            total += len(b)
            buf += b
            last = time.time()
            emit("%s %dバイト %r" % (stamp(), len(b), b[:300]))
            if not booted and re.search(rb"boot: ESP-IDF", buf):
                booted = True
                emit("%s 起動バナーを検出。この後のログを集めます" % stamp())
        if booted and (time.time() - last > 6 or len(buf) > 200000):
            break
    try:
        s.close()
    except Exception:
        pass
    if booted:
        emit("%s 起動ログ取得。合計 %d バイト。ポートを離れます" % (stamp(), total))
        break
    emit("%s 無音のまま。合計 %d バイト" % (stamp(), total))
emit("%s 終了" % stamp())
print("TOTAL=%d" % total, flush=True)
