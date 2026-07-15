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
| 録画デバイス | 📋 計画確定（docs/recorder-plan.md 改訂4）、**ボード調達待ちで一時停止中** |
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

- **録画デバイス**（詳細: docs/recorder-plan.md 改訂4）
  - 構成確定: Pi Zero 2 W + HWエンコーダ(h264_v4l2m2m) + mp4(4x/960×640/16Mbps/AAC)のみ保存、
    Wi-Fi不使用(DS3231 RTC)、モバイルバッテリー給電（ケース面は秋月Type-Cパネルキット）、
    タカチSW-125ケース
  - **次のアクション = ボード調達**: Pi Zero 2 Wをメルカリ/ヤフオク（相場¥3,000〜4,500）か
    マーケプレ（¥5,000台まで）で確保する。2026-07-10時点で新品流通は枯れ
    （秋月4ヶ月待ち・SS/共立売切、Radxa Zero 3Wも公式ストア全滅で撤回）
  - 他の購入品: microSD 64GB、L型OTGアダプタ、秋月Type-Cパネルキット、DS3231、
    LED付きボタン、SW-125（リスト詳細は recorder-plan.md）
- （その後）UVC/UAC化、専用基板化（battery.pngのLiPo基板拡張を含む）

## 開発履歴の要点（トラブルと解決は specification.md の年表参照）

2026-07-05 着工・初キャプチャ成功 → 07-06 60fpsビューア → 07-07 音声クリア化 →
07-08 映像最終修正(v0.33)+ロジック確定宣言・README日記化 → 07-10 録画デバイス計画確定。
着工から3日で当初目標達成、というのがこのプロジェクトの自慢ポイント。
