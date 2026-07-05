# CLAUDE.md — GBA 偽トロキャプチャ開発ガイド

このファイルは Claude Code が（モデルが Opus/Sonnet に変わっても）開発を継続するためのガイド。

## プロジェクト概要

GBA の LCD 信号を nanoCH32V305 で取り込み USB-HS で Windows PC に送る自作キャプチャ。
詳細は README.md とdocs/ を参照。**まず docs/wiring.md と docs/architecture.md を読むこと。**

## 絶対ルール

1. **t-wada 流 TDD で開発する**（docs/tdd.md）。テストリスト → Red → Green → Refactor
2. **配線を変更・仮定したら docs/wiring.md を必ず更新**（記載順は GBA側-nano側）
3. 未検証の数値・仕様は「要検証」と明記し、実測で確認したら文書を更新する
4. ユーザーとのやり取りは日本語
5. 書き込みは wchisp を使い、Claude 上で完結させる（docs/flashing.md の手放しループ）。
   手放しループ確立前に物理操作（BOOT押下・抜き差し）が必要な場合は、
   具体的な手順を明示してユーザーに依頼する

## よく使うコマンド

```bash
# ホスト側テスト (TDD)
cd host && python -m pytest -q

# FWビルド (ツールチェーン確定後に追記)
# 書き込み: wchisp flash <bin>  (詳細は docs/flashing.md)
```

## 現在の状態 (2026-07-05 更新)

- 配線完了（ユーザー実施済み、docs/wiring.md に記録）
- ドキュメント・プロジェクト骨格・ホスト側デコード/パケット解析（テスト11件 green）作成済み
- 環境構築完了（docs/setup.md）: Python 3.12 + pytest / wchisp v0.3.0
  (`tools/wchisp-win-x64/wchisp.exe`) / xPack RISC-V GCC 15.2.0 (`tools/`) /
  native make (ezwinports) / ch32fun クローン済み
- **M0 ファームウェア `firmware/m0-blink` ビルド成功済み**（`cd firmware && ./build.sh m0-blink`）。
  実機書き込みは未（初回はユーザーの BOOT 操作待ち）
- **FW実装方式: ch32fun に決定**（docs/architecture.md 参照。
  雛形 = examples_v30x/turbo_adc_with_usb と usbdevice_hs）
- 信号仕様は文献検証済み（DCKはバーストクロック・負エッジ有効。docs/gba-video-signal.md）
- ブートローダ突入は MODEフラグ+ソフトリセット方式（docs/flashing.md にコード掲載、実機未実証）
- 次の作業: M0 = Lチカ + wchisp 手放し書き込みループの確立（docs/architecture.md）
  ※初回書き込みのみユーザーに BOOT ボタン操作を依頼する必要あり

## スキル

- `.claude/skills/flash/` — wchisp での書き込み手順（手放しループ含む）

## ハードウェア注意点

- nanoCH32V305 の USB は **USB-HS 側**を使う（チップに FS/HS 両方ある）
- PA3 は LED（デバッグ用）。GBA信号は PA0-PA2 + PB多数 + PC4/PC5（docs/wiring.md）
- 現状 LDR1/LDR2 が GPIOC のため、まず GPIOB のみ(13bit)で実装する方針
