---
name: flash
description: GBAcapture ファームウェアの書き込み。通常は CLI の update で手放し更新（IAP経由、ボタン不要）。IAP自体の更新や救済時のみ wchisp + BOOT+RST を使う。FWのビルド後・書き込み依頼時に使用。
---

# GBAcapture ファームウェア書き込み

## 通常の更新（手放し・ボタン不要）— APP FW の更新はこれだけで良い

```bash
cd firmware && ./build.sh app
cd ../host && dotnet run --project GbaCap.Cli -- update ../firmware/app/gbacap_app.bin
```

フロー: APP に 'U' 送信 → BKPマジック+ソフトリセット → IAP更新モード(LED 5Hz) →
消去/書込/検証 → 'G' で再起動 → 新APP起動(LED 1Hz)。所要数秒。

- `dotnet run --project GbaCap.Cli -- ping` でモード確認（APP vX / IAP 更新モード）
- 更新失敗で APP が壊れても、IAP が生きていれば再度 update すれば直る
  （APP先頭が消去済みなら IAP は自動的に更新モードに留まる）

## IAP 自体の更新・救済（このときだけ物理操作）

1. ユーザーに依頼: 「BOOTを押したまま → RSTを押して離す → BOOTを離してください」
2. ```bash
   cd firmware && ./make_full.sh   # IAP+APP 統合イメージ gbacap_full.bin
   ../tools/wchisp-win-x64/wchisp.exe flash gbacap_full.bin
   ```
3. wchisp は書き込み後に自動リセット。IAP→APP と起動する

## 注意

- FW側の配置: IAP=0x08000000(16KB), APP=0x08004000(112KB)。APP は `app_offset.ld` でリンク
- WWDG は絶対に有効化しない（docs/flashing.md）
- wchisp が認識しない場合: Zadig で 4348:55E0 に WinUSB（割当済みのはず）
- 手順が変わったらこの SKILL.md と docs/flashing.md を両方更新すること
