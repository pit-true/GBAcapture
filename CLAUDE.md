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
- ドキュメント・プロジェクト骨格作成済み
- 環境: Python 3.12 (winget導入済)。Rust/wchisp/RISC-Vツールチェーン **未導入**
- FW実装方式（EVT C / ch32fun / Rust ch32-hal）は調査中 → 決定したらここに記載
- 次の作業: M0 = Lチカ + wchisp 手放し書き込みループの確立（docs/architecture.md）

## スキル

- `.claude/skills/flash/` — wchisp での書き込み手順（手放しループ含む）

## ハードウェア注意点

- nanoCH32V305 の USB は **USB-HS 側**を使う（チップに FS/HS 両方ある）
- PA3 は LED（デバッグ用）。GBA信号は PA0-PA2 + PB多数 + PC4/PC5（docs/wiring.md）
- 現状 LDR1/LDR2 が GPIOC のため、まず GPIOB のみ(13bit)で実装する方針
