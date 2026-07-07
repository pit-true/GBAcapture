# CLAUDE.md — GBA 偽トロキャプチャ開発ガイド

このファイルは Claude Code が（モデルが Opus/Sonnet に変わっても）開発を継続するためのガイド。

## プロジェクト概要

GBA の LCD 信号を nanoCH32V305 で取り込み USB-HS で Windows PC に送る自作キャプチャ。
詳細は README.md とdocs/ を参照。**まず docs/wiring.md と docs/architecture.md を読むこと。**

## 絶対ルール

0. **映像・音声のキャプチャロジックは確定済み（2026-07-08）。
   docs/capture-logic-invariants.md の不変条件を読まずに関連コードを変更しないこと。**
   変更した場合は同文書末尾の検証コマンドを全て通すこと

1. **t-wada 流 TDD で開発する**（docs/tdd.md）。テストリスト → Red → Green → Refactor
2. **配線は最終確定済み・変更提案禁止**（パターン剥がれ懸念。docs/wiring.md が正）
3. **ホスト側（受信・表示）は C# / .NET 8、FW は C (ch32fun)**（ユーザー指定）
4. 未検証の数値・仕様は「要検証」と明記し、実測で確認したら文書を更新する
5. ユーザーとのやり取りは日本語
6. 書き込みは wchisp を使い、Claude 上で完結させる（docs/flashing.md の手放しループ）。
   物理操作が必要な場合の依頼文言:
   「BOOTを押したまま → RSTを押して離す → BOOTを離す」（ISP突入。抜き差し不要）
7. GitHub リモート: https://github.com/pit-true/GBAcapture（main に push）。
   **非公開リポジトリのまま運用する方針（2026-07-08決定）** — テスト基板の開発記録扱い。
   README/docs はゲーム画面スクショ込みで日記帳的に書いてよい。READMEは日本語

## よく使うコマンド

```bash
# ホスト側テスト (TDD)
cd host && dotnet test

# FWビルド
cd firmware && ./build.sh m0-blink

# 書き込み (詳細は docs/flashing.md と .claude/skills/flash)
tools/wchisp-win-x64/wchisp.exe flash firmware/m0-blink/m0_blink.bin
```

## 現在の状態 (2026-07-05 更新)

- 配線完了（ユーザー実施済み、docs/wiring.md に記録）
- ドキュメント・プロジェクト骨格・ホスト側デコード/パケット解析（テスト11件 green）作成済み
- 環境構築完了（docs/setup.md）: .NET 8 SDK / wchisp v0.3.0
  (`tools/wchisp-win-x64/wchisp.exe`) / xPack RISC-V GCC 15.2.0 (`tools/`) /
  native make (ezwinports) / ch32fun クローン済み
- ホスト側を C# (.NET 8 + xUnit) へ移行完了、テスト14件 green（`cd host && dotnet test`）
- **M0 実機書き込み成功（2026-07-05）**: Zadig で WinUSB 割当（一度きりの作業）→
  `wchisp flash` → verify → 自動リセットまで確認。書き込みパイプライン確立
- チップにあった旧FW「GBA Capture Device」(1209:C001) はユーザーの過去のテストの名残
  と判明、上書き済み
- **FW実装方式: ch32fun に決定**（docs/architecture.md 参照。
  雛形 = examples_v30x/turbo_adc_with_usb と usbdevice_hs）
- 信号仕様は文献検証済み（DCKはバーストクロック・負エッジ有効。docs/gba-video-signal.md）
- ブートローダ突入は MODEフラグ+ソフトリセット方式（docs/flashing.md にコード掲載、実機未実証）
- **M0.5 実装完了・実機検証待ち**: `firmware/m05-usb`（USB-HSバルクデバイス
  1209:C001 + ブートローダ突入/pingコマンド）と `host/GbaCap.Cli`
  （LibUsbDotNet。ping / enter-bootloader）。ビルド済み
- M0.5 検証結果 (2026-07-05): USB通信・ping・保持ハンドルテストは動作。
  **ソフトISP突入は直接ジャンプ含め不可能と実機で確定**（docs/flashing.md 参照。
  V30xはメモリリマップ不可でISP ROMがBOOT0=0マッピングでは動けない）
- **M0.6 = IAP方式の手放しループ完成・実機実証済み（2026-07-05）**:
  `firmware/iap`(16KB @0x08000000) + `firmware/app`(@0x08004000, app_offset.ldでリンク)。
  初回のみ BOOT+RST + `wchisp flash firmware/gbacap_full.bin`（./make_full.sh で生成）。
  以降のAPP更新は `dotnet run --project GbaCap.Cli -- update firmware/app/gbacap_app.bin`
  だけで完結（ボタン不要）。詳細 .claude/skills/flash と docs/flashing.md
- **M2完了（2026-07-05）初キャプチャ成功**: docs/captures/2026-07-05_first-light.png。
  途中の大障害はGND（P2-VSSはパネル電源系≠ロジックGND → P1-GNDへ付け替えで解決）
- **🎉 M3/M4完了（2026-07-06）: 60fpsリアルタイムビューア動作** (FW v0.21 + GbaCap.Viewer)。
  キー知見:
  - **1ライン周期 = 241 DCKパルス**（可視240 + H-blank中ジャンク1。stride=241必須。
    240と仮定すると対角線ノイズ/白縦線が出る）
  - ストリーミングは**サーキュラDMA + VBLANK休止(250µs)検出アンカー**方式。
    ライン毎のIRQ再アームは水平ジッタの原因になるため禁止
  - USB-HSバルクは1EPあたり実効~3.3MB/s（NAK後の再ポーリングがマイクロフレーム単位）
    → **EP4/EP1/EP3の3本分散**で6.9MB/s(60fps)確保
  - パケットv3(484B): 赤下位2bitはGPIOC第2DMA(TIM2_CC2→DMA1_CH7)で取得しFWがbit2/6に合成
- 一発キャプチャ('L')は旧方式(SPL毎アーム+SampleOffset=2補正)のまま。将来v3方式へ統一余地
- 実測確定: PA2の実体はSPL / LDx1=LSB / 計測窓1.155倍問題は未解決(相対値は正確)
- ハード計画: docs/hardware-adapter-plan.md（基板化3段階）/ docs/audio-plan.md
  （音声はLOUT/ROUT→PA4/PA5、ADC+DMA+USB多重化）
- 次の作業: 映像品質の実用確認 → M6音声 / 基板化 / UVC化（docs/roadmap.md）
- **録画デバイス要件確定（2026-07-08 改訂2）**: オフ会でポケモン対戦を1日10試合録画、
  小型重視。**mp4のみ（4x/960×640, 16Mbps目安, 音声AAC）、生データ保存なし**。
  電源は**モバイルバッテリー(Type-C)給電**（LiPo基板統合は将来の基板化で）。
  **Wi-Fi不使用**（時刻はDS3231 RTC）。ボードは Raspberry Pi Zero 2 W
  （HWエンコーダがv4l2m2mで普通に使える唯一の小型ボード）。詳細 docs/recorder-plan.md

## スキル

- `.claude/skills/flash/` — wchisp での書き込み手順（手放しループ含む）

## ハードウェア注意点

- nanoCH32V305 の USB は **USB-HS 側**を使う（チップに FS/HS 両方ある）
- PA3 は LED（デバッグ用）。GBA信号は PA0-PA2 + PB多数 + PC4/PC5（docs/wiring.md）
- 現状 LDR1/LDR2 が GPIOC のため、まず GPIOB のみ(13bit)で実装する方針
