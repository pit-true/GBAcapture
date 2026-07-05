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

### ①' 【結論】ソフトISP突入は不可能 → IAP方式へ転換（2026-07-05 実機検証で確定）

**直接ジャンプ('B'コマンド)を実機で検証した結果、失敗。**観測事実:
- ジャンプ実行後、USB切断は発生する（保持ハンドル死亡で確認）
- しかし ISP (4348:55E0) は 0.2秒間隔のポーリングでも一度も列挙されない
- 数秒以内にアプリFWが正常起動して GBAcapture として復帰する

解釈: BOOT0=0 のまま（=0x00000000がユーザーFlashにマップされたまま）では
ROM内の絶対アドレス参照がアプリ領域に飛ぶため ROM は正常動作できない。
0x00000000への絶対ジャンプ=アプリのリセットベクタなので「アプリが綺麗に再起動する」
観測とも一致。**V30xにはメモリリマップ機構が無い**（RM V2.2 §1.3: マッピングはBOOTピンで
リセット時にのみ決まる）ため、ソフトだけでISP ROMを正しく走らせる手段は無い。
傍証: TinyUSB/AdafruitのV20x向けジャンプ実装は未完成のままコメントアウト、
wchisp作者もデバッガ経由でPC設定して失敗 (ch32-rs/wchisp#31)。

**→ 手放しループは IAP（自前ブートローダ）方式で実現する:**

```
[IAP領域 0x08000000, 16KB]  滅多に変更しない小さなUSBアップデータ
[APP領域 0x08004000〜]      開発対象のFW本体
通常時: IAP起動 → マジック無し → APPへジャンプ
更新時: APPがUSBコマンドでマジックをRAMに書きソフトリセット → IAPが留まり
        USB経由で新APPを受信 → flash自己書き換え → APPへ
救済:   BOOT+RST → 純正ISP → wchisp で IAP+APP を書き直し
```

これは WCH 公式の推奨パターン（EVT の IAP 例、openwch/ch32v307#62）。
以降、wchisp は救済用・IAP自体の更新用となる。

---

### ① ソフトブートローダ突入 — ~~直接ジャンプ方式~~（不成立、記録として残す）

CH32V30x のブートローダはシステムメモリ **0x1FFF8000 (28KB)**、実機の BTVER は 2.90。

**V003 方式（FLASH_STATR.MODE + ソフトリセット）は V20x/V30x には存在しない**
（RM V2.2 で確認、実機の 'S'/'T' 診断でも STATR 読み戻し 0）。

**確定した方式: 0x1FFF8000 への直接関数ジャンプ。**
根拠は V30x BTVER2.9 ブートROMの逆アセンブル
（https://github.com/COKPOWEHEU/ch32v307_bootloader ）:
- ROM は入口チェックを一切しない（BOOTピンはリセット時のメモリマッピング選択のみ）
- ROM 自身が sp/gp/mtvec/RCC を任意状態から再初期化する（HSI→PLL 96MHz）
- V305(chipID 0x50) では USBHS と OTG_FS の両方で ISP をサービスする

必要な前処理（`firmware/m05-usb/m05_usb.c` の `EnterIspBootloader()` に実装）:
1. USB ソフト切断（UC_DEV_PU_EN クリア）+ 50ms 待ち
2. `__disable_irq()` / SysTick 停止
3. **全 PFIC 割り込みの無効化とペンディングクリア**（ROM は mtvec を ROM 内へ向けるため、
   残った割り込みが誤ベクタする — 素朴なジャンプが失敗する最大の理由）
4. USBHS/OTG_FS のクロック停止・リセット
5. `((void(*)(void))0x1FFF8000)();`

⚠️ **WWDG は絶対に有効化しないこと**（ROM は WWDG を食わせないので即リセットされる。
IWDG は ROM が ~26 秒に再設定して食わせるので可）。

副次情報（実測）:
- リセット要因フラグ(RSTSCKR)は RMVF まで累積。SYSRESET 後は PINRSTF|SFTRSTF が立つ
- ch32fun の `USBHS_SendEndpoint` は data ポインタを常に UEP2_TX_DMA へ書くバグがある
  （`[2-1]` ハードコード）。EP4 送信は既定バッファ ENDPOINTS[4] への memcpy で回避

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
