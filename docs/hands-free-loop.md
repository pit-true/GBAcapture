# ボタンフリー開発ループ（手放し書き込み）— 方法と手順

2026-07-05 実機実証済み。BOOTボタン・USB抜き差しなしで FW の書き込みサイクルを回す仕組み。

## TL;DR — 普段使うコマンドはこれだけ

```bash
# APP FW をビルドして書き込み（約10秒、物理操作ゼロ）
cd firmware && ./build.sh app
cd ../host && dotnet run --project GbaCap.Cli -- update ../firmware/app/gbacap_app.bin
```

## 仕組み

CH32V30x は純正ISPブートローダへのソフト突入が**不可能**（BOOT0ピンで決まるメモリマッピング
にROMが依存しており、リマップ機構が無い。検証経緯は docs/flashing.md）。
そのため**自前の IAP ブートローダ**を常駐させる方式を採用した。

### フラッシュレイアウト

| 領域 | アドレス | サイズ | 内容 |
|---|---|---|---|
| IAP | 0x08000000 | 16KB | `firmware/iap` — USB経由の自己書き換えローダ（滅多に更新しない） |
| APP | 0x08004000 | 112KB | `firmware/app` — 開発対象FW（`app_offset.ld` でこの番地にリンク） |

### 更新フロー

```
[APP 実行中 (LED 1Hz)]
   │ ① CLI が 'U' コマンド送信
   │    → APP が BKP->DATAR1 に 0x424C を書きソフトリセット
   ▼
[IAP 起動] マジック発見 → クリアして更新モードに留まる (LED 5Hz)
   │ ② 'E' APP領域をページ(4KB)消去
   │ ③ 'W' 256Bチャンクで書き込み（16bit標準プログラミング＋読み戻し確認）
   │ ④ 'V' 加算チェックサムで全体検証
   │ ⑤ 'G' ソフトリセット
   ▼
[IAP 起動] マジック無し & APP有効 → APP へジャンプ → 新FW稼働
```

- マジックの受け渡しは **BKPレジスタ**（バックアップ領域、ソフトリセットでも消えない）
- IAP は起動時 SystemInit **前** に判定するので、APP はリセット直後同然の状態で起動する
- APP が空/壊れている（先頭ワード 0xFFFFFFFF）場合、IAP は自動的に更新モードに留まる
  → **更新に失敗しても再度 update すれば復旧する**

### USBコマンドプロトコル

両モード共通: VID 0x1209 / PID 0xC001、バルクOUT EP5 でコマンド、バルクIN EP4 で応答。
ヘッダは `"GBAC"+コマンド1B`。ping 応答の b5 でモード判別（0xAA=IAP, 0x00=APP）。
詳細は `firmware/iap/iap.c` 冒頭コメントと docs/flashing.md。

## 救済手順（IAP自体の更新もこれ）

APP更新は上記で完結するが、**IAPを更新する時 / IAPまで壊れた時**のみ物理操作が必要:

1. ユーザーに依頼: **「BOOTを押したまま → RSTを押して離す → BOOTを離す」**（純正ISPへ）
2. ```bash
   cd firmware && ./make_full.sh     # IAP+APP統合イメージ gbacap_full.bin を生成
   ../tools/wchisp-win-x64/wchisp.exe flash gbacap_full.bin
   ```
3. wchisp が書き込み後に自動リセット → IAP → APP と起動する

## 初回セットアップ（済み。新環境で再現する場合）

1. Zadig で WinUSB ドライバを割当（各一度きり）: ISP `4348:55E0` と アプリ `1209:C001`
2. 救済手順と同じ方法で統合イメージを書き込む

## 制約・注意

- **WWDG を絶対に有効化しない**（純正ISP ROMがWWDGを食わせないため救済不能になる）
- IAP と APP で BKPマジック値 (0x424C) とUSBプロトコルを一致させること
- APP のリンカスクリプト `app_offset.ld` は ch32fun 更新時に再生成が必要になる場合がある
  （元: `generated_CH32V305RBT6_3.ld` の ORIGIN/LENGTH を変更したもの）
- LED でモード識別: **1Hz=APP動作中 / 5Hz=IAP更新モード**
