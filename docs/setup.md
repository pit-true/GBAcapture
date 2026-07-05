# 開発環境セットアップ（Windows）

新しい環境・セッションで環境を復元するための手順。2026-07-05 時点の構成。

## 導入済みの構成

| ツール | 入手方法 | 場所 |
|---|---|---|
| Python 3.12 + pytest | `winget install Python.Python.3.12` → `py -3.12 -m pip install pytest` | `py -3.12` で起動 |
| wchisp v0.3.0 | GitHub releases プレビルド (`gh release download v0.3.0 -R ch32-rs/wchisp -p '*win-x64*'`) | `tools/wchisp-win-x64/wchisp.exe` |
| xPack RISC-V GCC | `gh release download -R xpack-dev-tools/riscv-none-elf-gcc-xpack -p '*win32-x64.zip'` | `tools/xpack-riscv-none-elf-gcc-*/` |
| GNU Make 4.4.1 (native) | `winget install ezwinports.make` | `%LOCALAPPDATA%\Microsoft\WinGet\Links\make.exe` |
| ch32fun | `git clone --depth 1 https://github.com/cnlohr/ch32fun firmware/ch32fun` | `firmware/ch32fun/` |

`tools/` は .gitignore 対象（バイナリ類はコミットしない）。クローン直後は上記コマンドで再取得する。

## 注意

- cargo install wchisp は **MSVC リンカ不在で失敗する**（本環境は VS Build Tools 未導入）。プレビルドを使う
- curl での直接ダウンロードはサンドボックスで拒否されることがある → **gh CLI を使う**
- **devkitPro msys2 の make は使わない**（環境変数が native gcc に渡らず
  「Cannot create temporary file in C:\WINDOWS\」で失敗する）。ezwinports の make を使う
- wchisp が nanoCH32V305 (ブートローダモード) を認識しない場合は Zadig で WinUSB を割当てる

## ビルドと書き込み

```bash
cd firmware && ./build.sh m0-blink                    # ビルド
tools/wchisp-win-x64/wchisp.exe flash firmware/m0-blink/m0_blink.bin   # 書き込み(リポジトリルートから)
```

書き込みフロー全体は docs/flashing.md と `.claude/skills/flash/` を参照。

## ホスト側テスト

```bash
cd host && py -3.12 -m pytest -q
```
