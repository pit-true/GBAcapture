---
name: flash
description: nanoCH32V305 へ wchisp でファームウェアを書き込む。手放しループ（ソフトブートローダ突入→flash→リセット）を優先し、不可時のみユーザーに BOOT 操作を依頼する。FWのビルド後・書き込み依頼時に使用。
---

# nanoCH32V305 ファームウェア書き込み

## 前提

- `wchisp` がインストール済みであること（`wchisp --version` で確認。無ければ `cargo install wchisp`）
- 対象: nanoCH32V305 (CH32V305)、USB Type-C 接続済み

## 手順

1. `wchisp info` でブートローダモードのデバイスが見えるか確認する
2. **見えない場合（アプリFW実行中）**: ホストツールでブートローダ突入コマンドを送る

   ```bash
   cd host && python -m gbacap.cli enter-bootloader
   ```

   （このコマンドが未実装/失敗の場合のみ、ユーザーに依頼:
   「nanoCH32V305 の BOOT ボタンを押しながら USB を挿し直してください」）
3. 書き込み: `wchisp flash firmware/build/<name>.bin`
4. 書き込み後、チップがリセットされアプリが起動する（USB抜き差し不要のはず）。
   `wchisp` が自動リセットしない場合は `wchisp reset` 相当のコマンドを確認すること
5. ホスト側でデバイス列挙を確認（`python -m gbacap.cli info` 等）して動作検証まで行う

## 注意

- 書き込み失敗を2回繰り返したら、ドライバ問題（Zadig で WinUSB 割当）を疑う
- 手順・コマンド体系が変わったらこの SKILL.md と docs/flashing.md を両方更新すること
