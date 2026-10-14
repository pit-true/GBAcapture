# ESP-IDF 環境（ESP32-P4 レコーダ）— この PC の構築記録

2026-10-14 に構築・動作確認済み。**やり直す前にここを読む**（はまった箇所が全部書いてある）。

## 置き場所

| もの | パス |
|---|---|
| ESP-IDF 本体 | `D:\esp\esp-idf`（v5.5.5、git clone） |
| ツールチェーン | `D:\esp\tools`（riscv32-esp-elf 14.2.0 / cmake 3.30.2 / ninja 1.12.1 / ccache / openocd / idf-exe / esp-rom-elfs） |
| Python 環境 | `D:\esp\tools\python_env\idf5.5_py3.12_env`（Python 3.12.10） |
| 環境読み込みスクリプト | `D:\esp\scripts\idf_env.ps1` |
| managed components | `recorder/managed_components/`（ビルド時に自動取得。git 管理しない） |

`D:\esp` に置くのは決まり（C ドライブが 8GB しかない）。`D:\llm\tmp\...` は消えるので道具を置かない。

## 使い方

```powershell
. D:\esp\scripts\idf_env.ps1          # IDF_PATH / IDF_TOOLS_PATH / PATH を設定して export.ps1
Set-Location C:\Users\korok\gba-capture\recorder
idf.py set-target esp32p4             # 初回のみ（sdkconfig を sdkconfig.defaults から生成）
idf.py build
idf.py -p COM5 flash monitor          # 基板 Type-C の CH343P。COM5 か COM6
```

`idf_env.ps1` の中身（要点）:

```powershell
$env:IDF_PATH = 'D:\esp\esp-idf'
$env:IDF_TOOLS_PATH = 'D:\esp\tools'
$env:IDF_SKIP_CHECK_SUBMODULES = '1'   # 下記「サブモジュール」
$env:GIT_CONFIG_KEY_0 = 'safe.directory'; $env:GIT_CONFIG_VALUE_0 = '*'
# PATH から c:\devkitPro\msys2 を除外し、Git\cmd と System32 を前に置く
```

## はまったこと（再現しないための記録）

1. **msys2 系バイナリがこの PC の PATH を汚染している。** `c:\devkitPro\msys2\usr\bin` に
   `cmd.exe` `git.exe` `sh.exe` `cmake.exe` があり、前に立つと
   `*** fatal error - couldn't create signal pipe, Win32 error 5` で死ぬ。
   → `idf_env.ps1` で PATH から除外し、`C:\Program Files\Git\cmd` と `C:\Windows\System32` を前に置く。
   **cmake は `cmd /c` を呼ぶので、cmd が msys 版だとビルドが意味不明にハングする**（2026-10-14 にこれで 15 分フリーズした）。
2. **pip がハングしていた原因は壊れた HTTP キャッシュ**。`--no-cache-dir` を付けると一瞬で通る。
   また IDF は制約ファイルで版を絞っているので `-c D:\esp\tools\espidf.constraints.v5.5.txt` を必ず付ける
   （付けないと esptool 5.x / kconfig 3.x が入って `Requirement ... was not met` になる）。
   ```powershell
   D:\esp\tools\python_env\idf5.5_py3.12_env\Scripts\python.exe -m pip install --no-cache-dir `
     --disable-pip-version-check -c D:\esp\tools\espidf.constraints.v5.5.txt `
     -r D:\esp\esp-idf\tools\requirements\requirements.core.txt
   ```
3. **`install.ps1` / `install-python-env` は使えない**（`Start-Process` がサンドボックスで Access denied、
   またはハング）。Python 環境は上記の pip 直接実行で作った。
4. **サブモジュール**は git ではなく tarball を展開して入れてある（`git submodule status` は未初期化 `-` 表示）。
   cmake が `git submodule update --init` を走らせると「destination path already exists」で失敗する。
   → `IDF_SKIP_CHECK_SUBMODULES=1` でチェックを飛ばす（中身自体は揃っている）。
5. **git の dubious ownership**（D: が ownership を記録しない FS）→ `GIT_CONFIG_KEY_0=safe.directory` / `*=*` で無効化。
6. **ESP-IDF の Python 実行は `python` ではなく venv の python**。`export.ps1` が選んでくれる。
7. `idf.py` は cmake/ninja を子プロセスとして起動する。**サンドボックス（workspace-write 等）では
   子プロセス用パイプが作れずハングする**。サンドボックスなし（フルアクセス）で実行すること。
 8. **`powershell.exe`（Windows PowerShell 5.1）で .ps1 を走らせると、UTF-8 BOM なしだと
    日本語のコメント・文字列が文字化けして構文エラーになる。** pwsh 7 の `-Encoding utf8` は
    BOM を付けないので、.ps1 を書いたら `-Encoding utf8BOM` で保存し直す
    （BOM 付きなら 5.1 でも 7 でも正しく読める）。
 9. **`idf.py -B build-hoge` を指定しても `SDKCONFIG` の既定はビルドディレクトリではなく
    プロジェクト直下の `sdkconfig`**（`tools/cmake/project.cmake` 9 行目が `CMAKE_SOURCE_DIR/sdkconfig`）。
    別プロファイルでビルドするなら `-D SDKCONFIG=build-hoge/sdkconfig` を必ず付ける。
    付けないと本体側の `sdkconfig` を書き換えて設定が混ざる
    （2026-10-14 に BRINGUP 設定が本体へ漏れた）。

## チップリビジョン（実機で確定・重要）

```powershell
python -m esptool --port COM6 --connect-attempts 1 chip_id
# → Chip is ESP32-P4 (revision v1.3)   MAC e8:f6:0a:e5:38:cb
```

- 基板のシリアルは **COM6**（COM5 は CH343P のもう一方の UART。esptool は応答なし）。
- **ESP32-P4 の rev >= 3.0 と < 3.0 は相互排他**。IDF の既定は v3.1 で、そのままビルドすると
  v1.3 の基板ではブートローダが起動を拒否する。Waveshare 公式（`waveshareteam/ESP32-P4-Platform` の
  `docs/ESP32P4_REVISION_CONFIG.md`）どおり **rev1_3 プロファイル**を使う:
  ```
  CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
  CONFIG_ESP32P4_REV_MIN_100=y        # REV_MIN_FULL=100 / REV_MAX_FULL=199
  CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360=y   # rev < 3.0 の上限は 360MHz（400 は追加審査が必要）
  ```
- `esp_h264` はこの設定でプリビルドライブラリのプロファイルが **`esp32p4_less_v3`** に切り替わる
  （cmake のログに出る: `esp_h264: ESP32-P4 prebuilt library profile: esp32p4_less_v3`）。
- `esp_audio_codec` **2.6 以上は `CONFIG_ESP32P4_REV_MIN_FULL >= 300` を要求してビルドを止める**
  （v1.3 では使えない）。1.0.x には esp32p4 のライブラリが無い（`lib/` に esp32/c3/s2/s3 だけ）。
  → **2.0.0〜2.5.0 に esp32p4 ライブラリがある**。本プロジェクトは `~=2.5.0` で張っている。
  版のライブラリ有無は `https://components-file.espressif.com/components/espressif/<name>/<ver>/CHECKSUMS.json`
  の `files[].path` に `lib/esp32p4/...` があるかで確認できる（レジストリの `targets` 欄は空で当てにならない）。
- ESP32-P4 のブートローダは `-Os` でも **0x60c0 バイト**あり、既定のパーティション表オフセット
  `0x8000` に収まらない → `CONFIG_PARTITION_TABLE_OFFSET=0x10000`（パーティションは 0x11000 以降）。

## esp_h264 をソースから確認した制約（2026-10-14、実機前の静的調査）

- Espressif 自身の CI が **P4 v1 系**で使う設定は `test_apps/sdkconfig.ci.p4_v1` で、
  `SELECTS_REV_LESS_V3=y` / `REV_MIN_100=y` / `REV_MIN_FULL=100` / `ESP_REV_MIN_FULL=100`。
  本プロジェクトの設定と一致している。
- テストの `sdkconfig.defaults.esp32p4` は PSRAM HEX 200MHz / CPU 360MHz / L2 128KB /
  `FREERTOS_HZ=1000` / `COMPILER_OPTIMIZATION_PERF`。うちと同じ。
  ただしキャッシュラインは 128B を選んでいる（P4 の IDF 既定は 64B）。
- **PSRAM バッファでエンコードできる**。`soc_caps.h` に `SOC_PSRAM_DMA_CAPABLE=1`、
  `SOC_AHB_GDMA_SUPPORT_PSRAM=1`、`SOC_AXI_GDMA_SUPPORT_PSRAM=1` があり、
  ドライバ側が入力に対して `esp_cache_msync(DIR_C2M | UNALIGNED)` の writeback、
  出力に対して M2C の invalidate を自前で呼んでいる（`esp_h264_enc_single_hw.c` 231 / 169 行目）。
- **落とし穴は出力バッファの整列**。出力の invalidate は `UNALIGNED` フラグなしの M2C で、
  `esp_cache_msync` は M2C 方向で `UNALIGNED` を明示的に拒否する
  （`components/esp_mm/esp_cache_msync.c` 120〜131 行目）。
  アドレスとサイズの両方がキャッシュラインの倍数でないと `ESP_ERR_INVALID_ARG` を返し、
  ドライバはその戻り値を無視するので**キャッシュが無効化されず NAL が壊れたデータになる**。
  → `g_yuv` / `g_encout` は 128 バイト境界に確保し、サイズ 921600（= 960*640*3/2）は 128 の倍数。
  64B ラインでも 128B ラインでも通る。

## managed components（recorder/main/idf_component.yml）

解決された版（2026-10-14、`recorder/dependencies.lock` に記録）:

| コンポーネント | 版 |
|---|---|
| espressif/esp_h264 | 1.4.1（P4 の HW エンコードはプリビルドライブラリ。プロファイル `esp32p4_less_v3`） |
| espressif/esp_muxer | 1.2.3 |
| espressif/esp_audio_codec | 2.5.0（`~=2.5.0` に固定。2.6 以上は rev 3.0 必須、1.0.x は P4 ライブラリ無し） |
| espressif/esp_tinyusb | 1.7.6~2（+ espressif/tinyusb 0.21.0~2） |

依存の版指定は `^` だと意図より広く解決される（`^2.5.0` は 2.6.2 を拾った）。
`~=` は「その系列のみ」。`~>` はこのマニフェストでは構文エラー。

ヘッダの写し（API 確認用・読み取り専用）は `D:\esp\comps\` に置いてある。

## 部品名の落とし穴（IDF 5.5）

- FATFS のコンポーネント名は **`fatfs`**（`ff` は通らない）
- `sdmmc` と **`esp_driver_sdmmc`** の 2 つが必要（`SDMMC_HOST_DEFAULT()` は `esp_driver_sdmmc` 側）
- USB ホストは **`usb`**
- `sdmmc_host_t` は `sd_protocol_types.h`、スロット設定は `driver/sdmmc_default_configs.h`
