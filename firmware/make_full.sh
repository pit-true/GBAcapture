#!/usr/bin/env bash
# IAP + APP の統合イメージを作る（初回インストール/救済用。wchisp で書き込む）
# 使い方: ./make_full.sh  →  gbacap_full.bin
set -euo pipefail
cd "$(dirname "$0")"

./build.sh iap
./build.sh app

OUT=gbacap_full.bin
cp iap/iap.bin "$OUT"
truncate -s 16384 "$OUT"   # IAP領域を16KBまでパディング
cat app/gbacap_app.bin >> "$OUT"
echo "OK: $OUT ($(stat -c%s "$OUT") bytes)"
