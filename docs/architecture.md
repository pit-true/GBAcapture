# アーキテクチャ設計

## 全体データフロー

```
GBA LCD信号 ──> [nanoCH32V305]                          ──USB-HS──> [Windows PC]
   DCK(PA0) ──trigger──> DMA ──> GPIOB->INDR 読み取り        │      専用キャプチャソフト
   LP (PA1) ──> ライン割り込み（ダブルバッファ切替）           │      (デコード/表示/保存)
   SPS(PA2) ──> フレーム同期（フレーム番号リセット）           │
```

## ファームウェア側 (CH32V305)

### サンプリング方式（DMA）

- **トリガ**: PA0 (DCK) をタイマ入力キャプチャ（TIM2_CH1 = PA0）に入れ、CCイベントで DMA 要求。
  **CH32V30x では EXTI から DMA は起動できない**（EXTIは割り込みのみ）→ タイマ経由が必須
- DCK は**バーストクロック**（可視区間のみ240パルス、docs/gba-video-signal.md）なので、
  DCK エッジ駆動なら余計なサンプルは入らない。データ有効エッジは**負エッジ**
- **転送**: DMA が `GPIOB->INDR` (16bit) → SRAM ラインバッファへ。CPU 介在なし
- **ライン同期**: PA1 (LP, 正エッジ) の EXTI 割り込みでラインバッファ確定・切替（240サンプル/ライン）
- **フレーム同期**: PA2 (SPS, 負エッジ) の EXTI 割り込みでフレーム番号更新・行カウンタリセット
- DCK エッジ→DMA読み取りの遅延（数十〜百ns級 + USB DMA競合ジッタ）とピクセル周期 238ns
  の整合は実機で要検証（M2）
- 当面 GPIOB のみ（R下位2bit欠け）。将来 PB6/PB7 へ配線変更で完全15bit（docs/wiring.md 参照）
- 先行実証例: ch32fun `examples_v30x/turbo_adc_with_usb` が同チップで
  4〜6MSPS 連続DMA + USB-HSバルク送信を実証済み → 帯域的に成立見込み

### USB 転送

- USB-HS (480Mbps) デバイス、**ベンダー固有クラス・バルクIN** 1本（+制御用コマンド）
- CH32V305 の USBHS ペリフェラルを使用。**nanoCH32V305 では USB2 コネクタ側が HS**
  （USB1 は FS。給電・書き込みとの関係は実機確認）
- パケット構成（案）:
  - ラインパケット: `ヘッダ(4B: マジック/フレーム番号/ライン番号) + 240×2B ピクセル`
  - PC 側はヘッダで再同期可能（取りこぼし耐性）
- ベンダーコマンド（コントロール転送 or 別エンドポイント）:
  - `CMD_ENTER_BOOTLOADER` — 手放し書き込み用（docs/flashing.md）
  - `CMD_GET_STATS` — 信号計測値の取得（DCK周波数等）
  - `CMD_START / CMD_STOP` — ストリーミング制御

### SRAM 見積もり

- CH32V305RBT6: **flash 128KB / SRAM 32KB**（✅データシート確認済み）
- フレーム全体 (76.8KB) は載らない → **ライン単位（480B）のダブルバッファ + 即時USB送出** が必須
- 注意: CH32V307 にある DVP（カメラパラレルポート）は **V305 には無い** → GPIO+DMA方式一択

### ファームウェア実装方式（2026-07-05 決定）

**ch32fun (https://github.com/cnlohr/ch32fun) を採用。**

- CH32V305 対応明記、`examples_v30x/usbdevice_hs` に USBHS バルクデバイス例あり
- `examples_v30x/turbo_adc_with_usb` が「連続DMA取得 + USB-HSストリーミング」の直接の雛形
- Makefile ベースで wchisp との CLI 書き込みループに馴染む
- WCH 公式 EVT (https://github.com/openwch/ch32v307) はレジスタ参照実装として併用
- Rust (ch32-hal) は USBHS ドライバの成熟度が低く見送り

## ホスト側 (Windows 専用ソフト)

- 段階1 (現在): **Python + pyusb (WinUSB)** — 受信 → デコード → PNG保存/簡易表示
  - デコードロジック (GPIOビット→RGB888) は pytest で TDD（docs/tdd.md）
- 段階2: リアルタイム表示（pygame/SDL）、フレーム落ち統計
- 段階3: 録画（将来は UVC 化で標準ソフト対応。docs/roadmap.md）
- ドライバ: WCH デバイスに WinUSB を Zadig で割当（要確認）

## 開発マイルストーン

1. **M0**: LED (PA3) Lチカ + wchisp 手放し書き込みループ確立 ← 開発基盤
2. **M1**: 信号計測FW — DCK/LP/SPS の周波数・カウントを USB で報告（信号仕様の実測検証）
3. **M2**: 1ライン分の DMA キャプチャ → USB 転送 → PC でビット列確認
4. **M3**: 全フレームストリーミング → PC で静止画 (PNG) 復元
5. **M4**: リアルタイム表示（簡易ビューア完成 = 当初目標達成）
6. 以降: docs/roadmap.md
