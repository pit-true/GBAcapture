# ESP32-P4-Pico レコーダ 通し手順(2026-10-18 改訂)

nanoCH32V305 から映像と音声を受け取り、**480x320 の mp4(H.264 + AAC 32kHz)** を microSD に書く装置の、
準備から検証までの通し手順。960x640 にするときは PC で `recorder/scripts/upscale_x2.bat` を使う
(ドットをそのまま 2 倍にするだけなので画質は落ちない)。
配線の詳細は `docs/recorder-wiring-esp32p4.md`、書き込みと復旧の詳細は `docs/esp32p4-flashing.md`。
ここには手順と期待値だけを残す。

## 0. 必要なもの

| もの | 用途 | 状態 |
|---|---|---|
| ESP32-P4-Pico(SKU 32101) | 本体 | 動作確認済み |
| nanoCH32V305 | 映像・音声の送信元 | FW v0.33。変更しない |
| MX1.25 4PIN → USB(A) ケーブル | P4 の USB-HS ホストへ繋ぐ | 確保済み |
| microSD(FAT32) | 録画先 | SanDisk 32GB で動作確認済み |
| タクトスイッチ 1 個 | 録画の開始と停止 | GPIO2 ↔ GND。実機未確認 |
| LED + 470Ω | 録画中の表示 | GPIO3 → 470Ω → LED → GND。実機未確認 |
| モバイルバッテリー | 電源 | Type-C。基板の Type-C 1 本だけ使う |

基板の Type-C は CH343P の UART ブリッジ専用で、ESP32-P4 の USB データ線は繋がっていない。
nano との受信も、PC へ SD を見せる MSC も、MX1.25 4PIN ヘッダ(USB OTG 2.0 HS)1 本を使う。
つまり **MSC で読むときは nano のケーブルを抜いて PC に差し替える**。差し替えずに mp4 を
確かめたいときは 8 章の `p4pull.py`(コンソール経由)を使う。

## 1. microSD を用意する

1. PC で FAT32 にフォーマットする(32GB を超えるカードは FAT32 で作り直す。ESP-IDF の FATFS は exFAT を読めない)
2. 基板の TF スロットに挿す
3. 書き込みが成功と返るのに内容が残らないカードがあった(古い microSD)。8MB の速度テスト(`w`)で読み戻し一致を確かめる

起動ログの期待値:

```
I (xxxx) rec: SD マウント完了 /sd クラス=... 容量=29512MB
I (xxxx) rec: SD 空き=29454MB
```

## 2. 基板に書き込む

アプリが動いていればボタンは要らない(コンソールの `d` で書き込みモードに入る)。
アプリが死んでいるときだけ **BOOT を押したまま → RST を押して離す → BOOT を離す**。

```powershell
# ビルド + 書き込み + 起動ログ。Git Bash から呼ぶときは MSYSTEM を消す(ESP-IDF の export が MSys を拒む)
pwsh -NoProfile -Command 'Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue; & C:\Users\korok\gba-capture\recorder\scripts\p4flash_auto.ps1 -Port COM6 -BuildDir build -LogSecs 20'
```

`-SkipBuild` を付けるとビルドを飛ばす。書き込みの期待値は `FLASH_DONE`。

注意: **COM6 を開くと基板が再起動する**(CH343 の DTR/RTS の過渡)。スクリプトはそれを前提に、
起動を待ってからコマンドを送る。

## 3. 起動を確認する

```
I (xxx) main: chip rev v1.3 cores=2 features=0x0
I (xxx) main: heap free internal=336KB psram=32765KB
I (xxx) cfg: 設定 scale=2 bitrate=8000kbps split=15min vol=100%
I (xxx) enc: エンコーダを開いた 480x320 8000kbps internal free=193KB 最大=62KB
I (xxx) enc: PSRAM 書き出しは GDMA(段 11520B x4、同時 3、internal free 251KB)
I (xxx) rec: SD マウント完了 /sd クラス=... 容量=...MB
I (xxx) usbnano: 接続 1209:c001
I (xxx) usbnano: 受注開始
```

起動直後の internal RAM は約 250KB 空いていれば正常(直引き表 96KB とエンコーダ 39KB、
GDMA の段 46KB を起動時に確保している)。

## 4. ベンチで速さを測る

コンソールの `b` で合成映像 600 枚を拡大 + エンコードする(SD には書かない)。
nano が止まっているときの 2026-10-18 の実測:

```
I (xxxx) enc: スケール内訳 us/枚 rgb=2852 y=0 uv=2125 copy=0
I (xxxx) enc: エンコーダベンチ 600 枚 4.46 秒 -> 134.5 fps scale<=15408us enc<=3047us err=0
```

`DMA 一致確認: 0/230400 バイト不一致` が出ていれば GDMA 経路の出力は CPU 経路と同じ。

## 5. 録画して確かめる(20 秒試験)

コンソールの `T` で録画を始めると 20 秒後に自動で止まる。
`recorder/scripts/p4seq.py` を使うと、1 プロセスで COM6 を開いたまま時刻指定でコマンドを送れる。

```powershell
$py = 'D:\esp\tools\python_env\idf5.5_py3.12_env\Scripts\python.exe'
& $py C:\Users\korok\gba-capture\recorder\scripts\p4seq.py D:\esp\scripts\rec.log T@0 p@17 i@30
```

2026-10-18 の実測(REC_0013):

```
I (24218) rec: 停止ログ REC_0013.mp4 映像=1211 先頭pts=0.00s 末尾pts=20.27s | 音声=635 先頭pts=0.00s 末尾pts=20.29s
I (24229) enc: スケール内訳 us/枚 rgb=3164 y=0 uv=2258 copy=0 | 枚=1212 scale<=28298us enc<=12089us
```

合格の目安:

| 項目 | 期待値 |
|---|---|
| 映像の枚数 | 20 秒で 1150 枚以上(59.7fps × 20 = 1195) |
| 音声の枚数 | 20 秒で約 625 枚(32000Hz ÷ 1024 = 31.25 枚/秒) |
| 映像と音声の末尾 pts | 両方 20 秒前後で 0.1 秒以内の差 |
| `ringovf` `rxadrop` | 0(開始直後の数個は可) |
| `task_wdt` | 出ない |

`p` のタスク統計で core1 の IDLE1 が 30% 前後あれば余裕がある(2026-10-18: scale 28%、aac 22%、rx 8%)。

## 6. mp4 を検査する

```powershell
& $py C:\Users\korok\gba-capture\recorder\scripts\mp4check.py REC_0013.mp4
ffprobe -v error -show_entries stream=codec_name,width,height,sample_rate,nb_frames,duration -of default=nw=1 REC_0013.mp4
```

| 項目 | 期待値 |
|---|---|
| moov | ある(なければ途中で切れている。停止を押さずに電源を切ると失われる) |
| trak | 2 本。`vide` と `soun` |
| vide | `avc1` 480x320、タイムスケール 59690、1 枚の間隔 999(= 59.7275fps) |
| soun | `mp4a` 32000Hz 2ch、1 枚 1024 サンプル |
| 長さ | 映像と音声が一致 |

960x640 にする(ffmpeg が要る。`winget install Gyan.FFmpeg` など):

```
recorder\scripts\upscale_x2.bat REC_0013.mp4   # 隣に REC_0013_960x640.mp4 ができる
```

## 7. 録画の使い方(本番)

1. nano の 4PIN ケーブルを繋ぐ(基板側 MX1.25 4P: `VCC_5V / USBD_N / USBD_P / GND`)
2. ボタンを 1 回押す → GPIO3 の LED が点灯し、`/sd/REC_NNNN.mp4` が始まる(番号は SD 内の最大 + 1)
3. 15 分ごとに自動で `_p2.mp4` へ移る(FAT32 の 4GB 上限より小さい)
4. もう 1 回押すと閉じる。閉じないと moov が書かれず再生できない
5. nano が抜かれると自動で停止する
6. SD の空きが 400MB 未満のときは開始しない。250MB を切ったら停止して 4 回点滅する

## 8. 録画した mp4 を PC へ出す

### 8a. コンソール経由(ケーブルを差し替えない)

いちばん番号の大きい mp4 を 1.5Mbps の UART で受ける。20 秒の録画(約 6MB)で 45 秒。

```powershell
& $py C:\Users\korok\gba-capture\recorder\scripts\p4pull.py D:\esp\scripts\pull
```

CRC が一致すれば完了。基板側は `D` コマンド。録画中は断られる。

### 8b. MSC(PC から SD として見る)

nano のケーブルを抜き、4PIN ヘッダを PC に繋いでからコンソールの `m`。
Explorer に「TinyUSB Flash Storage」が出る。戻すときはもう 1 回 `m`。
2026-10-18 時点では、最新ビルドでの往復(戻してから録画)は未確認。

## 9. コンソール

基板の Type-C(COM6、115200)で 1 文字コマンドが使える。

| 文字 | 動作 |
|---|---|
| `r` | 再起動 |
| `i` | 状態とヒープを表示 |
| `p` | タスクごとの CPU 使用率 |
| `b` | 拡大 + エンコードのベンチ(録画中は拒否) |
| `t` | 録画の ON・OFF |
| `T` | 録画を始めて 20 秒で自動停止(試験用) |
| `e` | 受信 + 拡大 + エンコードだけ回す(SD に書かない) |
| `D` | 最新の mp4 を UART で送る(`p4pull.py` が受ける) |
| `m` | MSC(PC に SD を見せる)⇄ ホストの切り替え |
| `d` | 書き込みモードで再起動 |
| `w` `s` `y` `z` `x` `v` `F` `k` | SD と PPA の診断(開発用) |

## 10. つまずいたとき

| 症状 | 原因 | 対処 |
|---|---|---|
| `Could not open COM6` | 別のプロセスが掴んでいる | `p4watch.py` などの待ちプロセスを止める |
| ESP-IDF の export が `MSys/Mingw is not supported` | Git Bash の `MSYSTEM` が残っている | pwsh 内で `Remove-Item Env:MSYSTEM` してから export |
| `sd=0` | 未挿入かマウント失敗 | 挿して再起動 |
| `usb pkts=0` | nano が繋がっていない、または `S` を送っていない | 録画か `e` を始めると受信が始まる |
| `task_wdt` で IDLE1 | core1 が飽和 | `p` で内訳を見る。aac と scale で 60% を超えたら要対策 |
| 画面の左端に縦の筋があり、左の十数ドットが右端の絵 | nano 側の列位相が 1 回ずれた(2026-10-18 に 1 度観測、次の録画では正常) | 録画を止めて始め直す。ビューアでも起きるか要確認 |
| mp4 が再生できない | 停止を押さずに電源が落ちた | 復旧不可。ボタンで止める運用 |

## 11. 未完了(2026-10-18 時点)

1. ボタン(GPIO2)と LED(GPIO3)の実機確認
2. MSC の往復(ケーブル差し替え → `m` → 読み出し → `m` → 続けて録画)
3. 30 分連続録画と発熱、15 分スライスの境界が IDR になっているか
4. 試験用コードの整理(`ppa_probe.c`、SD の生セクタ診断、1 文字診断コマンド)
5. 左端の列位相ずれの再現条件(nano 側)
