# ESP32-P4-Pico の書き込みと復旧手順

実機で確認した事実だけを記録する。2026-10-14 実測。

## 結論（先に要点）

1. **このボードには DTR/RTS による自動リセット回路がない。**
   ダウンロードモードへは毎回 **BOOT+RST の手操作**が必要。
   → 「BOOT を押したまま → RST を押して離す → BOOT を離す」
2. **ROM のダウンロードループは、前回合意したボーレートを保持したまま沈黙する。**
   書き込み後にリセットが効かないと、ボードは 460800 ボーのダウンロードループに取り残される。
3. **esptool は `--baud` を初期同期に効かせない。**
   `components` の実装（`esptool/__init__.py` 781 行目）で
   `--before no_reset_no_sync` のときだけ `initial_baud = args.baud`、
   それ以外では常に `min(115200, args.baud)`。
   つまり **115200 以外のボーで待つ ROM には、通常オプションでは絶対に繋がらない。**

## 取り残された ROM を起こす（ボタン不要）

```powershell
$py = 'D:\esp\tools\python_env\idf5.5_py3.12_env\Scripts\python.exe'
& $py -m esptool --port COM6 --chip esp32p4 --baud 460800 `
    --before no_reset_no_sync --after no_reset --connect-attempts 3 chip_id
```

これで `Chip is ESP32-P4 (revision v1.3)` が返れば、ROM はそのボーで生きている。
ボーが不明なときは 115200 / 230400 / 460800 / 921600 を順に試す。

起こせたら、そのままアプリへ飛べる（ダウンロードモードから抜けて起動する）:

```powershell
& $py D:\esp\scripts\p4boot.py 30 D:\esp\scripts\applog.log
```

`p4boot.py` は
「ボー自動検出で接続 → 115200 に変更 → RUN で起動 → 115200 でログ収集」をやる。

## 書き込み（毎回 BOOT+RST が必要）

```powershell
. D:\esp\scripts\idf_env.ps1
Set-Location C:\Users\korok\gba-capture\recorder
idf.py -p COM6 flash
```

`recorder/sdkconfig.defaults` で `CONFIG_ESPTOOLPY_BEFORE_NORESET=y` を設定している。
既定の `default_reset` は DTR/RTS を組み合わせるが、**このボードにはその配線がないので必ず失敗する**。
`no_reset` にすると「リセットを試みず、115200 で同期パケットを出す」だけになる。
だから **BOOT+RST でダウンロードモードに入った直後**（この状態の ROM は 115200 で待っている）に
実行すれば繋がる。

**書き込みが成功しても、その後リセットはかからない。**
`--after hard_reset` は RTS パルスを出すだけで、このボードでは何も起きないため、
チップはダウンロードループに留まり、前回合意したボーで無言のままになる。
これが「書き込みは成功したのにコンソールが 1 バイトも出ない」現象の説明になっている。

起動させるには次のどちらかをする。

- **RST を 1 回押して離す**（BOOT は押さない）。いちばん確実
- `p4boot.py` の RUN コマンドで ROM に起動させる（上の復旧セクション）

書き込みログに `Hash of data verified.` が 3 枚分出ていれば書き込み自体は成功している。

## 起動確認の階段（何かおかしいとき）

重い機能を全部止めた最小構成で、基板が起動するかだけを見る。

```powershell
. D:\esp\scripts\idf_env.ps1
Set-Location C:\Users\korok\gba-capture\recorder
idf.py -B build-bringup -D SDKCONFIG=build-bringup/sdkconfig `
    -D SDKCONFIG_DEFAULTS=sdkconfig.bringup.defaults -p COM6 flash
```

`-D SDKCONFIG=...` を忘れると本体側の `recorder/sdkconfig` を書き換えてしまうので注意
（ESP-IDF の SDKCONFIG の既定はビルドディレクトリではなくプロジェクト直下）。

BRINGUP モードは PSRAM を切り、USB・SD・エンコーダを初期化せず、
記録 LED を 1 回点滅させて 5 秒ごとに生存ログを出す。
これで出なければ基板/電源/UART の問題、出れば本番設定の問題と切り分かれる。


## ボードの観察ポイント

| 確認 | 方法 |
| --- | --- |
| 電源が入っているか | 基板の **Power indicator**（公式 Hardware Description の 11 番）が点灯 |
| アプリが起動したか | GPIO3 に付けた記録 LED が起動時に 3 回点滅（`gbac_led_blink(3, 150)`） |
| コンソール | Type-C 側の CH343 が **COM6**。UART0 = GPIO37(TX) / GPIO38(RX)、115200 |
| COM5 | 別系統の CH340 アダプタ。このボードとは無関係。使わない |

## 実機で確定している事実

- チップ: **ESP32-P4 revision v1.3**（エンジニアリングサンプル系）。MAC `e8:f6:0a:e5:38:cb`、クリスタル 40MHz
- therefore `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` + `CONFIG_ESP32P4_REV_MIN_100=y` が正しい
  （Waveshare 公式 FAQ の `esp32p4_rev_pre_v3.defaults` と同じ構成）
- フラッシュ: 搭載は 32MB。書き込みは `--flash_mode dio --flash_freq 80m --flash_size 16MB`
- パーティションオフセット 0x10000、ブートローダ 0x2000
- SPI2 の octal IOMUX は GPIO28〜37 を使う。**GPIO37 は UART0 TX と重なる**ので、
  SPI2 octal を自前で使う機能を増やすときはコンソールに注意
- ただし **PSRAM は SPI2 を使わない**。P4 の PSRAM は `PSRAM_MSPI1`
  （`DR_REG_PSRAM_MSPI1_BASE`）という専用コントローラで、Kconfig のモードは
  `SPIRAM_MODE_HEX`（16 本）。MSPI 側の IOMUX ピンは全て `GPIO_NUM_INVALID`
  （`soc/esp32p4/include/soc/spi_pins.h`）で専用パッド。
  つまり **PSRAM を有効にしても GPIO37 は食われない**。コンソールは安全
- flash / PSRAM の MSPI は専用パッドで GPIO を消費しない（`spi_pins.h` の MSPI 欄が全て `GPIO_NUM_INVALID`）

## 最初にやること: 電源の抜き差し

ボタンより手軽で、状態を確実にリセットできる。Type-C を抜いて差し直す（モバイルバッテリー側でもよい）。
P4 は電源オンで ROM ブートローダから走り、書き込み済みのアプリを読みに行く。
ダウンロードループに残っていても、アプリが走っていても、どちらでも結果が変わる。

- アプリが走っていれば **10 秒ごとに状態ログが出る**。
  `app_main.c` のループが `show_status()` を 10 秒間隔で出しているため、
  10 秒以上無音ならアプリが走っていないことになる
- 起動直後に GPIO3 の記録 LED が 3 回点滅する。シリアルが壊れていてもこれは見える

抜き差しに耐える監視スクリプトが `recorder/scripts/p4watch.py`。
ポートを開き直し続けるので、ケーブルを抜いても終了しない。

```powershell
# 15 分あい COM6 を拾い続ける。ログは第2引数
& 'D:\esp\tools\python_env\idf5.5_py3.12_env\Scripts\python.exe' `
    C:\Users\korok\gba-capture\recorder\scripts\p4watch.py 900 D:\esp\scripts\watch1.log
```

書き込みをしたいときは先にこれを止めること。COM6 は同時に使えない。

## 書き込み後はボタン不要: コンソール 1 文字で操作できる

書き込み済みのアプリは UART0 の受信(GPIO38)で 1 文字を受け取る。
esptool が 460800 で ROM と握手できたので、PC → 基板の受信配線は実証済み。

| 文字 | 働き |
|---|---|
| `r` | 再起動。RST ボタンの代わりに効く |
| `i` | 状態ログとヒープを表示 |
| `b` | エンコーダベンチ(設定枚数)を回す |
| `t` | 録画の ON / OFF を切り替え |

送るのは `recorder/scripts/p4cmd.py`。

```powershell
$py = 'D:\esp\tools\python_env\idf5.5_py3.12_env\Scripts\python.exe'
# 状態を見る
& $py C:\Users\korok\gba-capture\recorder\scripts\p4cmd.py i 6
# ベンチを回して 40 秒集める
& $py C:\Users\korok\gba-capture\recorder\scripts\p4cmd.py b 40 D:\esp\scripts\bench1.log
# 録画を開始して 20 秒、止めて状態表示
& $py C:\Users\korok\gba-capture\recorder\scripts\p4cmd.py t 20
& $py C:\Users\korok\gba-capture\recorder\scripts\p4cmd.py ti 8
```

コマンドを拾うのはメインループなので、反映まで最大 1 秒かかる。
`b` は 600 枚で 10 秒ほどループを止める。
これがあるため書き込み後に RST を押す必要はない。`p4boot.py` が RUN を送って起こす。

## 分かっていないこと（実機待ち）

- ~~書き込み成功後にコンソールへ 1 バイトも出ない理由~~ → **解決済み(2026-10-14)**。
  アプリは走っていて、10 秒ごとの状態ログが出ていた。取れなかったのは
  ログ取得の窓が 10 秒周期を外していたことと、COM6 を別プロセスが握っていたこと。
  教訓は「10 秒以上無音ならアプリが走っていない」。それ以外は実機待ち
- microSD が刺さっていないのか、マウントが失敗しているのか。`sd=0` の理由を起動ログで見る
- nano を繋いだときの usb_host の実測スループット
