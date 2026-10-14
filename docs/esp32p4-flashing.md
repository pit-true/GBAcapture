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

`idf.py flash` は `--before default_reset --after hard_reset` で走るが、
**default_reset はこのボードでは効かない**。ボードがダウンロードモードに入っていなければ
接続に失敗する。失敗したら BOOT+RST を依頼する。

書き込みログに `Hash of data verified.` が 3 枚分出ていれば書き込み自体は成功している。

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
- SPI2 の octal IOMUX は GPIO28〜37 を使う。**GPIO37 は UART0 TX と共有**なので、
  SPI2 octal を使う機能（PSRAM の別経路など）を増やすときはコンソールに注意
- flash / PSRAM の MSPI は専用パッドで GPIO を消費しない（`spi_pins.h` の MSPI 欄が全て `GPIO_NUM_INVALID`）

## 分かっていないこと（実機待ち）

- 書き込み成功後にコンソールへ 1 バイトも出ない理由。
  候補は (a) ROM がブートローダを起動できていない、(b) チップが給電不足で止まっている、
  (c) アプリが極早期に止まっている。電源 LED と BOOT+RST の操作で切り分ける。
