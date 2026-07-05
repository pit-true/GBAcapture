---
name: flash
description: nanoCH32V305 へ wchisp でファームウェアを書き込む。手放しループ（ソフトブートローダ突入→flash→リセット）を優先し、不可時のみユーザーに BOOT 操作を依頼する。FWのビルド後・書き込み依頼時に使用。
---

# nanoCH32V305 ファームウェア書き込み

## 前提

- wchisp はリポジトリ内 `tools/wchisp-win-x64/wchisp.exe` を使う
  （無ければ docs/setup.md の手順で再取得。cargo install は本環境では失敗する）
- FW ビルド: `cd firmware && ./build.sh <target-dir>`（例: `./build.sh m0-blink`）
- 対象: nanoCH32V305 (CH32V305RBT6)、USB Type-C 接続済み

## 手順

以下、リポジトリルートで `WCHISP=tools/wchisp-win-x64/wchisp.exe` として読む。

1. `$WCHISP info` でブートローダモードのデバイスが見えるか確認する
2. **見えない場合（アプリFW実行中）**: ホストツールでブートローダ突入コマンドを送る

   ```bash
   cd host && python -m gbacap.cli enter-bootloader
   ```

   （このコマンドが未実装/失敗の場合のみ、ユーザーに依頼:
   「nanoCH32V305 の BOOT を押しながら USB を挿す、または BOOT 押しながら RST を押して離してください」）
3. 書き込み: `$WCHISP flash firmware/<target-dir>/<name>.bin`
4. `wchisp flash` はデフォルトで書き込み後にチップをリセットしアプリが起動する
   （USB抜き差し不要。抑止したい場合のみ `--no-reset`）
5. ホスト側でデバイス列挙を確認（`python -m gbacap.cli info` 等）して動作検証まで行う

## 注意

- 書き込み失敗を2回繰り返したら、ドライバ問題（Zadig で WinUSB 割当）を疑う
- 手順・コマンド体系が変わったらこの SKILL.md と docs/flashing.md を両方更新すること
