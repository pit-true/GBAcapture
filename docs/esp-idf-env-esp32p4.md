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

## managed components（recorder/main/idf_component.yml）

解決された版（2026-10-14、`recorder/dependencies.lock` に記録）:

| コンポーネント | 版 |
|---|---|
| espressif/esp_h264 | 1.4.1（P4 の HW エンコードはプリビルドライブラリ。プロファイル `esp32p4`） |
| espressif/esp_muxer | 1.2.3 |
| espressif/esp_audio_codec | 2.6.2 |
| espressif/esp_tinyusb | 1.7.6~2（+ espressif/tinyusb 0.21.0~2） |

ヘッダの写し（API 確認用・読み取り専用）は `D:\esp\comps\` に置いてある。

## 部品名の落とし穴（IDF 5.5）

- FATFS のコンポーネント名は **`fatfs`**（`ff` は通らない）
- `sdmmc` と **`esp_driver_sdmmc`** の 2 つが必要（`SDMMC_HOST_DEFAULT()` は `esp_driver_sdmmc` 側）
- USB ホストは **`usb`**
- `sdmmc_host_t` は `sd_protocol_types.h`、スロット設定は `driver/sdmmc_default_configs.h`
