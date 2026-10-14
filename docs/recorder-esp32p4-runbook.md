# ESP32-P4-Pico レコーダ 通し手順(2026-10-14)

nanoCH32V305 から映像と音声を受け取り、960x640 の mp4 を microSD に書く装置の、
準備から検証までの通し手順。配線の詳細は `docs/recorder-wiring-esp32p4.md`、
書き込みと復旧の詳細は `docs/esp32p4-flashing.md` に書く。ここには手順と期待値だけを残す。

## 0. 必要なもの

| もの | 用途 | 状態 |
|---|---|---|
| ESP32-P4-Pico(SKU 32101) | 本体 | 確保済み。アプリが起動することを確認済み |
| nanoCH32V305 | 映像・音声の送信元 | FW v0.33 完成。変更禁止 |
| MX1.25 4PIN → USB(A) ケーブル | P4 の USB-HS ホストへ繋ぐ | **未確保。手持ちは 6P で合わない** |
| microSD(FAT32) | 録画先 | 未確認(起動ログで `sd=0`) |
| タクトスイッチ 1 個 | 録画の開始と停止 | GPIO2 ↔ GND。外部部品不要 |
| LED + 470Ω | 録画中の表示 | GPIO3 → 470Ω → LED → GND |
| モバイルバッテリー | 電源 | Type-C。基板の Type-C 1 本だけ使う |

基板の Type-C は CH343P の UART ブリッジ専用で、ESP32-P4 の USB データ線は繋がっていない。
nano との受信は MX1.25 4PIN ヘッダ(USB OTG 2.0 HS)だけが使える。12Mbps の FS 側
(GPIO26/27)では映像 4.67MB/s を運べないため、4PIN のケーブルは必須品です。

## 1. microSD を用意する

1. PC で FAT32 にフォーマットする(32GB 以下が簡単)
2. 基板の TF スロットに挿す

起動ログに出る期待値:

```
I (xxxx) rec: SD マウント完了 /sd クラス=10 容量=30436MB
I (xxxx) rec: SD 空き=30400MB
```

`SD マウント失敗(...)` なら `docs/recorder-wiring-esp32p4.md` の 5 章と 9 章を見る。

## 2. 基板に書き込む

毎回 BOOT+RST が必要(DTR/RTS での自動リセットはこのボードに無い)。

1. **BOOT を押したまま → RST を押して離す → BOOT を離す**
2. 書き込み

```powershell
. D:\esp\scripts\idf_env.ps1
Set-Location C:\Users\korok\gba-capture\recorder
idf.py -B build -p COM6 flash
```

押す人が席を外すときは、待機スクリプトに押させる。ダウンロードモードはそのまま維持される。

```powershell
Start-Process pwsh -WindowStyle Hidden -ArgumentList `
  '-NoProfile','-File','C:\Users\korok\gba-capture\recorder\scripts\p4flash_and_log.ps1',`
  '-Port','COM6','-BuildDir','C:\Users\korok\gba-capture\recorder\build',`
  '-WaitSecs','14400','-LogSecs','150',`
  '-LogFile','D:\esp\scripts\applog.log','-ProgressFile','D:\esp\scripts\flashprog.log'
```

進捗は `-ProgressFile` に 1 行ずつ積まれる。書き込みの期待値は `Hash of data verified.` が 3 回。

## 3. 起動を確認する

COM6 を 115200 で開く(または `recorder/scripts/p4watch.py 900 D:\esp\scripts\applog.log`)。
出てくる順番:

```
I (xxx) main: chip rev v1.3 cores=2 features=0x...
I (xxx) main: heap free internal=...KB psram=...KB
I (xxx) board: btn=GPIO2 led=GPIO3 sd=43,44,39-42
I (xxx) main: コンソールで r=再起動 i=状態 b=ベンチ t=録画 ON/OFF
I (xxx) cfg: 設定 scale=4 bitrate=16000kbps split=15min vol=100%
I (xxx) rec: SD マウント完了 /sd クラス=... 容量=...MB
I (xxx) rec: SD 空き=...MB
I (xxx) main: sd=1 rec=0 free=...MB | usb pkts=0 err=0 | frame ok=0 drop=0 | ...
```

同時に GPIO3 の LED が 3 回点滅する。PSRAM の確保に失敗するとこのログまで辿り着かない。
`SD 未挿入。挿してからボタンを押してね` が出るなら SD が読めていない。

## 4. HW エンコーダを測る(最大のリスク)

960x640@60 が 16.6ms/枚に載るかを最初に測る。`CONFIG_GBAC_BENCH_FRAMES=600` なので、
**起動するとボタンを押さなくても 600 枚のベンチが自動で走る**。SD が入っていれば
そのベンチは mp4 も書く(`/sd/REC_0001.mp4`)。SD がなければこう出る:

```
W (xxxx) main: SD がないのでエンコーダ単体のベンチにする
```

SD を挿したときの期待ログ:

```
I (xxxx) enc: エンコード開始 960x640 scale=4 16000kbps bench=600
I (xxxx) rec: /sd/REC_0001.mp4 開始 960x640 split=15min
I (xxxx) enc: エンコーダベンチ 600 枚 10.20 秒 -> 58.8 fps scale<=1200us enc<=14000us err=0
I (xxxx) rec: ベンチ 600 枚 11.00 秒 -> 54.5 fps, video=600 err=0
```

- `enc<=` が **16600us 以下**なら 60fps に載る
- 超えるなら scale 3(720x480)か 30fps に落とす。決め打ちせず実測で決める
- 公称は 1080p@30。960x640@60 は画素数で公称の 59% なので、載って不自然はない
- 書き直し後にもう一度測りたいときは、コンソールに `b`(録画中は拒否される)

```powershell
$py = 'D:\esp\tools\python_env\idf5.5_py3.12_env\Scripts\python.exe'
& $py C:\Users\korok\gba-capture\recorder\scripts\p4cmd.py b 40 D:\esp\scripts\bench1.log
```


## 5. mp4 を検査する

再生機がなくても箱を読むだけで検査できる。SD をカードリーダーで PC に移して読む。

```powershell
& $py C:\Users\korok\gba-capture\recorder\scripts\mp4check.py D:\esp\scripts\REC_0001.mp4
```

サンプルの中身まで見るなら `--samples 12` を足す。

ベンチ 600 枚での期待値(単体テスト `test_pts_ms` が決める値):

| 項目 | 期待値 |
|---|---|
| moov | ある(なければ途中で切れている) |
| trak | 2 本。`vide` と `soun` |
| vide のコーデック | `avc1` + avcC に SPS と PPS |
| vide のサイズ | 960x640 |
| vide のサンプル数 | 600 |
| vide の長さ | **10.028 秒**(600 枚 ÷ 59.7275fps) |
| vide の同期フレーム | 10 個(60 枚ごとに IDR) |
| soun | `mp4a` 48000Hz。ベンチではサンプル 0(nano が繋がっていないため) |

nano を繋いだ実録画では、音声サンプル数 = 秒数 × 48000 ÷ 1024 になり、
2 トラックの長さが実時間と一致する。一致していれば esp_muxer に渡している
pts の単位は ms で正しい。倍率でずれていれば単位が違う。この検証は nano を
繋いだ実録画でしかできない(ベンチの mp4 では音声が入らない)。


## 6. 録画の使い方

1. nano の 4PIN ケーブルを繋ぐ(基板側 MX1.25 4P: `VCC_5V / USBD_N / USBD_P / GND`)
2. ボタンを 1 回押す → GPIO3 の LED が点灯し、`/sd/REC_0001.mp4` が始まる
3. 15 分ごとに自動で `REC_0001_p2.mp4` へ移る(FAT32 の 4GB 上限より小さい)
4. もう 1 回押すと閉じる。閉じないと moov が書かれず再生できない
5. nano が抜れると自動で停止する
6. SD の空きが 400MB 未満のときは開始しない。250MB を切ったら停止して 4 回点滅する

## 7. 状態ログとコンソール

基板の Type-C(COM6、115200)は送信だけでなく受信も使える。1 文字で操作できる。

```powershell
& $py C:\Users\korok\gba-capture\recorder\scripts\p4cmd.py i 6      # 状態表示
& $py C:\Users\korok\gba-capture\recorder\scripts\p4cmd.py t 20     # 録画 ON/OFF
& $py C:\Users\korok\gba-capture\recorder\scripts\p4cmd.py r 5      # 再起動
```

| 文字 | 動作 |
|---|---|
| `r` | 再起動(RST ボタンを押さなくてよい) |
| `i` | 状態とヒープを表示 |
| `b` | エンコーダベンチ(録画中は拒否) |
| `t` | 録画の ON・OFF 切替 |

## 8. つまずいたとき

| 症状 | 原因 | 対処 |
|---|---|---|
| ログが 10 秒以上途絶える | アプリが動いていない | 電源の抜き差し。直らなければ BOOT+RST |
| `Could not open COM6` | 別のプロセスが掴んでいる | `p4watch.py` などの待ちプロセスを PID で止める |
| `The chip stopped responding` | 書き込みモードでない | BOOT+RST。詳細は `docs/esp32p4-flashing.md` |
| `sd=0` | 未挿入かマウント失敗 | 挿して再起動。ログの `SD マウント失敗(...)` を読む |
| `usb pkts=0` | nano が繋がっていない | 4PIN ケーブル。6P では繋がらない |
| mp4 が再生できない | 停止を押さずに電源が落ちた | 未クローズは復旧不可。ボタンで止める運用 |
| 画面上でコマンドが効かない | 旧ビルドが載っている | 書き直す |

## 9. 未完了項目(2026-10-14 時点)

実機で確かめるまで完了にできないもの。板はアプリを動かしているが、
書き込みモードに入っていないため、新しいビルドを一度も載せられていない。

1. **HW エンコーダ 960x640@60 の実測**(計画中の step 2、最大のリスク)
   600 枚ベンチ内蔵のビルドが書き込み待ち。ボタン 1 操作で測れる
2. **SD マウントの実測**。`sd=0` の理由が未確定(未挿入か失敗か)
3. **usb_host で nano から受信**(step 3)。MX1.25 4PIN ケーブルの調達待ち
4. **mp4 の A/V 同期と pts 単位の実証**。5 章の期待値と突き合わせる
5. **スライス境界が IDR になるか**。muxer がいつ切るかがソースで見えない
6. **HS ホストと FS デバイスの同時動作 / MSC 未実装**(step 4)。目的の必須要件ではない
7. **30 分連続録画と発熱**(step 7)
8. **ケース加工と組み込み**(step 8)
9. **TF スロットの電源ゲート GPIO が必要か**。`CONFIG_GBAC_SD_PWR_GPIO=-1` のまま
10. **40pin ヘッダのパッド番号の実機確認**(`docs/recorder-wiring-esp32p4.md` 6 章)
11. **電源断で未クローズの mp4 が失われる**。esp_muxer に断片 mp4 がないため仕様上の限界

完了しているもの:

- 受信パケットの解析、デコード、4 倍最近傍拡大、OUYY 変換(単体テスト 2330 項目 green)
- 音声の DC 除去・ゲイン・リサンプル(32802 → 48000Hz)
- H.264 と AAC の設定、mp4 の mux、15 分スライス、空き容量のガード
- ボタンと LED、NVS 設定、状態ログ、コンソール操作
- 書き込みと復旧の手順、mp4 の検査スクリプト、配線と使い方の文書
