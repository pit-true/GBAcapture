"""COM6 などコンソール UART に 1 文字送って基板にコマンドを執行させる。
使い方: python p4send.py [COM6] [d] [読む秒数=1.0]
  d = ダウンロードモード(書き込みモード)で再起動
  r = 再起動 / i = 状態表示 / s = SD 総当たり
送ったあと数秒分のログを標準出力に出す(ダウンロードモードに入ると無音になる)。"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else 'COM6'
cmd = sys.argv[2] if len(sys.argv) > 2 else 'd'
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0

s = serial.Serial(port, 115200, timeout=0.2)
s.reset_input_buffer()
s.write((cmd + '\r').encode())
s.flush()
buf = b''
end = time.time() + secs
while time.time() < end:
    buf += s.read(256)
s.close()
sys.stdout.write(buf.decode('utf-8', 'replace'))
sys.stdout.flush()
