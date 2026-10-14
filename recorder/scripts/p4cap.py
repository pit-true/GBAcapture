# COM6 のコンソールを決まった時間だけ集める。文字化けしないようログはバイトのまま保存。
# 使い: python p4cap.py <秒> <ログファイル> [送るコマンド ...]
# コマンドは 1 個ずつ送って 1.5 秒応答を読む。最後に残り時間を読む。
import sys, time, serial

secs = int(sys.argv[1]) if len(sys.argv) > 1 else 30
logf = sys.argv[2] if len(sys.argv) > 2 else r"D:\llm\tmp\cap.log"
cmds = sys.argv[3:]
PORT = "COM6"
BAUD = 115200

out = open(logf, "wb")
s = None
for _ in range(20):
    try:
        s = serial.Serial(PORT, BAUD, timeout=0.2)
        s.dtr = False
        s.rts = False
        break
    except Exception:
        time.sleep(0.3)
if s is None:
    print("PORT BUSY: %s" % PORT)
    sys.exit(1)


def drain(dur):
    t = time.time()
    n = 0
    while time.time() - t < dur:
        b = s.read(512)
        if b:
            n += len(b)
            out.write(b)
            out.flush()
            sys.stdout.write(b.decode("utf-8", "replace"))
            sys.stdout.flush()
    return n


drain(1.0)
for c in cmds:
    sys.stdout.write("\n>>> send %r\n" % c)
    s.write((c + "\r").encode())
    s.flush()
    drain(1.5)
drain(secs)
s.close()
out.close()
print("\nログ: %s" % logf)
