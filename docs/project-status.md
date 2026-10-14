# プロジェクト現状サマリ（2026-07-15時点）

「偽ころキャプチャ」= GBA実機の映像・音声を自作ハードでUSBキャプチャするプロジェクトの
全体像と現在地。**新しいセッション（PC再起動後・モデル変更後）はまずこれを読む。**

## 一言でいうと

GBA(AGB-CPU-02)の基板ビア19本から信号をタップ → nanoCH32V305(RISC-V)が取り込み →
USB-HSでPCへ → 自作ビューア「GBAcapture (Nisekoro-capture)」で表示・再生。
**映像240×160/60fps・クリアなステレオ音声・録画はOBS任せ、という当初目標はすべて達成済み。**

## 現在の状態

| 項目 | 状態 |
|---|---|
| 映像キャプチャ | ✅ 完成（FW v0.33、59.73fps、フレーム落ちなし、乱れゼロ） |
| 音声キャプチャ | ✅ 完成（プリボリューム、キャリア同期積分、32802Hz→48kHz） |
| ビューア | ✅ 完成（1-4x整数拡大、フルスクリーン、F12スクショ、音量/Mute、NO SIGNAL、デバイス追従） |
| FW更新基盤 | ✅ 完成（IAP自己更新、ボタン不要、約10秒） |
| **確定ロジック** | 🔒 **2026-07-08確定。変更前に docs/capture-logic-invariants.md 必読（絶対ルール）** |
| README | ✅ 日本語化済み。日記帳スタイル（リポジトリは非公開のまま運用と決定済み） |
| 録画デバイス | 📋 **改訂5（2026-10-09）: ESP32-P4-Pico 構成に変更**（docs/recorder-plan-esp32p4.md）。Pi Zero 2 W 案（改訂4）は調達難で撤回 |
| UVC化・基板化 | 未着手（ロードマップ） |

## 環境・場所

- リポジトリ: `C:\Users\korok\gba-capture`（git、リモート https://github.com/pit-true/GBAcapture 、**非公開運用**）
- ビューアexe: `host\GbaCap.Viewer\bin\Debug\net8.0-windows\GbaCap.Viewer.exe`
- F12スクショ保存先: `ピクチャ\GBAcapture\`
- ハード: GBA（配線済み・**配線変更提案は禁止**）+ nanoCH32V305（USB-HS接続）
- ユーザー環境: Windows 11 / .NET 8 SDK / RISC-V GCC・wchisp は `tools\` に導入済み

## よく使うコマンド（リポジトリ直下で）

```bash
cd host && dotnet test                                   # ホスト側テスト（26件）
powershell -NoProfile -File recorder\test\run_tests.ps1   # 録画FWの単体テスト（2330項目）
cd firmware && ./build.sh app                            # FWビルド
cd host && dotnet run --project GbaCap.Cli -- update ../firmware/app/gbacap_app.bin  # 手放しFW更新
cd host && dotnet run --project GbaCap.Cli -- freq       # 診断: DCK/LP/SPL周波数
cd host && dotnet run --project GbaCap.Cli -- framestat  # 診断: フレーム同期(全て0が正)
```

## 文書の読む順番（新セッション向け）

1. `CLAUDE.md` — 絶対ルール（TDD、配線変更禁止、確定ロジック不変条件、日本語）
2. `docs/capture-logic-invariants.md` — 🔒 キャプチャ関連コードを触るなら必読
3. `docs/specification.md` — 仕様全体+FW/ホスト処理ロジック詳解
4. 作業対象に応じて: `docs/wiring.md`（配線の正） / `docs/audio-plan.md` /
   `docs/hands-free-loop.md` / `docs/recorder-plan.md`（録画デバイス計画）

## 進行中・次のアクション

- **録画デバイス**（詳細: docs/recorder-plan-esp32p4.md 改訂5、2026-10-09）
  - 構成: **ESP32-P4-Pico**（手元にあり）+ esp_h264 HWエンコーダ + mp4(4x/960×640/16Mbps/AAC)
    を基板上の microSD(FAT32) へ。nano とは 4ピン USB ヘッダ(HS)→Type-C オスで接続・給電。
    PC 向け SD 公開はパネル Type-C → GPIO26/27 の FS USB MSC（遅いので基本はカード抜き）。
    ケースはタカチ SW-75B、タクトSW TS-06104 + LED×2
  - **次のアクション**: ESP-IDF 環境構築 → リスク検証3点（HWエンコーダ 960×640@60 実測 /
    usb_host で nano 受信 10k転送/秒 / HSホスト+FSデバイス同時動作）
  - **2026-10-14 実機でアプリが動いているのを初めて確認**。COM6(115200) に 10 秒ごと状態ログ、
    uptime 約 5900 秒。中身は `sd=0 rec=0 usb pkts=0 err=0 frame ok=0 enc f=0`。
    つまり SD 未挿入・nano 未接続・エンコーダ未実行。基板もコンソールも生きている
  - 載っているのはベンチ導入前の古いビルド。新しいビルド（エンコーダベンチ 600 枚内蔵）を
    書くには **BOOT+RST でダウンロードモードに入れるだけ**（DTR/RTS 未配線なので自動リセット不可）。
    以降は `recorder/scripts/p4flash_and_log.ps1` が書き込み→起動→ログまで自動
  - 手持ちの 1.25mm **6P** ケーブルは 4P ヘッダに合わない → 4P 品（製品同梱ケーブルを確認）
  - **通し手順・期待ログ・mp4検査の期待値・未完了一覧は docs/recorder-esp32p4-runbook.md**
  - 単体テストは **2330 項目 green**（`recorder/test/run_tests.ps1`。実解像度の拡大位置と
    pts の ms 換算まで固定済み）。C# 側 26 件も green
  - 改訂4（Pi Zero 2 W）は docs/recorder-plan.md に記録として残す
- （その後）UVC/UAC化、専用基板化（battery.pngのLiPo基板拡張を含む）

## 開発履歴の要点（トラブルと解決は specification.md の年表参照）

2026-07-05 着工・初キャプチャ成功 → 07-06 60fpsビューア → 07-07 音声クリア化 →
07-08 映像最終修正(v0.33)+ロジック確定宣言・README日記化 → 07-10 録画デバイス計画確定。
着工から3日で当初目標達成、というのがこのプロジェクトの自慢ポイント。
