# ファームウェア書き込み — wchisp による手放し開発ループ

## 目標

**人手ゼロで「ビルド → 書き込み → 実行 → テスト」を回し続ける。**
通常は「BOOT押しながらUSB接続 → 書き込み → USB抜き差し」が必要だが、これをすべてソフトウェアで代替する。

## 手放しループの仕組み（2026-07-05 文献調査済み）

```
[アプリFW実行中]
   │  ① PCから USB ベンダーコマンド CMD_ENTER_BOOTLOADER 送信
   ▼
[FWが MODEフラグ+ソフトリセットでブートローダへ]   ← BOOT ボタン不要
   │  ② wchisp flash firmware.bin
   ▼
[書き込み完了]
   │  ③ wchisp flash はデフォルトで書き込み後チップをリセット → アプリ起動 ✅検証済み
   ▼
[新アプリFW実行中]  → テスト → ①に戻る
```

### ① ソフトブートローダ突入の実装方法

CH32V30x のブートローダはシステムメモリ **0x1FFF8000 (28KB)**。
STM32 流の「アドレスへジャンプ」ではなく、**FLASH_STATR の MODE ビット + ソフトリセット**が正規ルート:

```c
FLASH->BOOT_MODEKEYR = 0x45670123;  // KEY1
FLASH->BOOT_MODEKEYR = 0xCDEF89AB;  // KEY2 (MODEフラグ書き込みアンロック)
FLASH->STATR = 0x4000;              // STATR.MODE(bit14)=1 → 次リセットでBOOT領域実行
RCC->RSTSCKR |= 0x1000000;          // RMVF: リセットフラグ全クリア(必須!)
PFIC->CFGR = 0xBEEF0080;            // KEYCODE 0xBEEF + SYSRESET → ソフトリセット
```

- ブートローダは「SFTRSTF のみ 1、他のリセットフラグ全て 0」を確認して留まる。
  **RMVF クリアを忘れると即アプリへ戻ってしまう**
- リセット前に USB ソフト切断（D+プルアップ解除）をしておくと再列挙が確実
- 出典: https://github.com/basilhussain/ch32v003-bootloader-docs （CH32V003 での解析）
- ⚠️ **V305 実機での動作は未実証**（V003 で解析された手順。V30x のリファレンスマニュアル
  にも MODE ビットの記載あり）。M0 で最初に実証する。失敗してもリスクなし
  （BOOT ボタン手動突入でやり直せる）

## ツール: wchisp（✅ 調査済み）

- CH32V305RBT6 はデバイスDB登録済み（chip_id 0x50, flash 128K）
- コマンド: `probe` / `info` / `reset` / `erase` / `flash` / `verify` / `config`
- **`wchisp flash` はデフォルトで書き込み後リセット**（`--no-reset` で抑止可）
- Windows ドライバ: **Zadig で WinUSB を割当**（README 記載の公式手順）
- 入手: プレビルドバイナリ（GitHub releases v0.3.0 / nightly）。
  cargo install はMSVCリンカが必要（本環境では未導入のためプレビルドを使用）

## 手順（現在の運用）

1. **初回のみ / FW暴走時**: ユーザーに依頼 →「BOOT を押しながら USB を挿す（または BOOT 押しながら RST 押して離す）」
2. `wchisp info` でチップ確認 → `wchisp flash <bin>` → 自動リセットでアプリ起動
3. 手放しループ確立後: ホストツールで `enter-bootloader` → `wchisp flash <bin>` のみ

## 状態記録

| 項目 | 状態 |
|---|---|
| wchisp インストール | ✅ v0.3.0 (`tools/wchisp-win-x64/wchisp.exe`) |
| Zadig / WinUSB ドライバ割当 | ✅ 2026-07-05 実施（4348:55E0 に WinUSB。一度きりの作業） |
| 初回書き込み (M0 Lチカ) | ✅ 2026-07-05 成功。flash→verify→自動リセットまで確認 |
| チップ情報 | CH32V305RBT6 / ブートローダ v2.90 / UID CD-AB-2F-89-3A-BC-36-F1 |
| ソフトブートローダ突入コード | ✅ 実装済み（`firmware/m05-usb`、実機検証待ち） |
| ホスト側突入コマンド | ✅ `dotnet run --project host/GbaCap.Cli -- enter-bootloader` |
| 手放しループ実証 | 未（M0.5 書き込み後に検証。現状は BOOT+RST 手動突入が必要） |

## コマンドプロトコル（M0.5、バルクOUT EP5）

制御コマンドはコントロール転送ではなく**バルクOUTパケット**で送る
（ch32fun の USB スタックにベンダーリクエスト用フックが無いため。スタック改変を避けた）。

| パケット | 動作 |
|---|---|
| `"GBAC" + 'B'` (5B) | ISPブートローダへソフト突入（USB切断→MODEフラグ→ソフトリセット） |
| `"GBAC" + 'P'` (5B) | EP4(バルクIN) で `"GBAC!"+major+minor+0` の8Bを返す（疎通確認） |

アプリFWのUSB ID: **VID 0x1209 / PID 0xC001**、Product "GBAcapture"。
初回のみ Zadig でこの VID:PID に WinUSB を割当てる必要がある（ISPの 4348:55E0 とは別割当）。

## フォールバック

- FW 暴走時: BOOT 手動突入（ユーザー依頼）
- 将来案: USB制御リレー/別MCUで NRST・BOOT を外部制御し完全ハード無人化（docs/roadmap.md）
