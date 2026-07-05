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

### ① ソフトブートローダ突入 — 調査状況（2026-07-05 実機検証で仕切り直し）

CH32V30x のブートローダはシステムメモリ **0x1FFF8000 (28KB)**、実機の BTVER は 2.90。

**V003 方式（MODEフラグ+ソフトリセット）は V305 実機で不成立と判明:**

- `FLASH->MODEKEYR (0x40022024)` へのキー書き込み後に `FLASH->STATR` bit14 を
  セットしても**読み戻しは 0x0000**（FW v0.6 の 'S'/'T' 診断コマンドで実測）
- 手元の CH32FV2x_V3x RM **V1.5 では FLASH_STATR の bit[31:8] は Reserved で
  MODE ビット自体が存在しない**。0x40022024 は高速書き込み(FLOCK)用の拡張キーのみ
- ソフトリセット（PFIC 0xBEEF0080）と USB 切断は**動作確認済み**
  （リセット→アプリ再起動までは実証できている。ブートローダに留まらないだけ）

実測で分かった副次情報:
- リセット要因フラグは RMVF まで累積する。SYSRESET 後は PINRSTF も同時に立つ模様
  （ping 応答の rst_flags=0x14 = PINRSTF|SFTRSTF）
- 代替手段の候補: (a) 最新RM(v2.2+)にのみ記載の別レジスタ/ビット、
  (b) 0x1FFF8000 への直接ジャンプ、(c) IAP方式（FWが自分でflash書き換え、ISP不要）
- 調査継続中。確定したらここを更新すること

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
