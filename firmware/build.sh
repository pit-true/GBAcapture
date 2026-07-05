#!/usr/bin/env bash
# FWビルドスクリプト (Git Bash 用)
# 使い方: ./build.sh <ターゲットディレクトリ>   例: ./build.sh m0-blink
set -euo pipefail
cd "$(dirname "$0")"

TOOLCHAIN_BIN="$(ls -d ../tools/xpack-riscv-none-elf-gcc-*/bin 2>/dev/null | head -1 || true)"
if [ -z "$TOOLCHAIN_BIN" ]; then
    echo "ERROR: tools/ に xPack RISC-V GCC が見つからない。docs/setup.md 参照" >&2
    exit 1
fi
# devkitPro msys2 の make は環境変数が native gcc に渡らず失敗する。
# ezwinports の native make (winget install ezwinports.make) を優先する
export PATH="$LOCALAPPDATA/Microsoft/WinGet/Links:$(cd "$TOOLCHAIN_BIN" && pwd):$PATH"

TARGET_DIR="${1:?usage: ./build.sh <target-dir> (e.g. m0-blink)}"
make -C "$TARGET_DIR" build
echo "OK: $(ls "$TARGET_DIR"/*.bin)"
