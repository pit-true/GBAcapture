# スタンドアロン録画デバイス 改訂5 — ESP32-P4-Pico 構成（2026-10-09）

改訂4（Pi Zero 2 W）はボード調達難で停止していた。手元に **Waveshare ESP32-P4-Pico** が
届いたので、Linuxボードを使わず **ESP32-P4 のハードウェア H.264 エンコーダ**で録画する構成に
切り替える。nanoCH32V305 側の FW・配線は一切変更しない（2WAY運用は維持）。

## 要件（2026-10-09 ユーザー指示）

| # | 項目 | 内容 |
|---|---|---|
| 1 | 給電 | Type-C で ESP32-P4-Pico に給電。P4-Pico から nanoCH32V305 にも給電 |
| 2 | 接続 | P4-Pico → nano は USB ケーブル（P4-Pico 側 4ピンヘッダ → Type-C オス） |
| 3 | 記録 | 240×160 を **4倍（倍率は後で変更可能）= 960×640 / 60fps**、音声付き **mp4** を **SDカード**へ |
| 4 | 操作 | タクトスイッチ **TS-06104**（表面実装）1個。押す → LED点灯+録画開始、もう一度 → 停止 |
| 5 | 表示 | LED 2個。電源LED（給電中点灯）、録画LED（録画中点灯） |
| 6 | ケース | タカチ **SW-75B**（外形 50W×30H×75D mm） |
| 7 | PC接続 | Type-C を PC に挿すと給電しつつ **SD の中身が見える**（USBマスストレージ） |

## まず結論（設計判断の要点）

1. **microSD は挿せる。** P4-Pico 基板上に TF スロット（SDIO 3.0 / 4bit）がある。
2. **基板の Type-C は USB データ線が ESP32-P4 に繋がっていない**（CH343P の USB-UART 専用）。
   PC から SD を見せるには **別の Type-C メス（パネル取付）を増設**し、ESP32-P4 の
   Full-Speed USB（GPIO26/27）に繋ぐ。速度は **FS = 実効 約1MB/s** なので
   「ちょっと確認する」用途。1日分 10GB 超の吸い出しは **カードを抜いてリーダーで読む**のが現実解。
3. **nano との接続は 4ピン USB ヘッダ（MX1.25 4P）を使う。** ここが ESP32-P4 の
   High-Speed USB で、5V（VCC_5V）も同じコネクタから出るので nano への給電も兼ねる。
   写真にある手持ちケーブルは **1.25mm 6P ハウジング**なので 4P ヘッダに刺さらない → **4P 品が必要**
   （製品箱に「MX1.25 4PIN to USB cable」が同梱のはずなので先に確認）。
4. エンコードは **esp_h264 のハードウェアエンコーダ**（公称 1080p30）。960×640@60 は
   画素レートで 1080p30 の 59% なので余裕の見込み。**最初に実測する最大リスク**。
5. 音声は nano の 0xA6 パケット（32.8kHz）を 48kHz にリサンプルして **AAC**、映像と
   **esp_muxer で mp4** に多重化。
6. ファイルシステムは **FAT32**（ESP-IDF は exFAT をデフォルト無効・menuconfig も無い）。
   16Mbps × 30分 = 3.6GB で 4GB 上限に近いので **15分または 2GB でファイル分割**する。

## 回路図で確認した ESP32-P4-Pico の事実

Waveshare 配布の回路図（ESP32-P4-Pico-datasheet.pdf）のネット名から確認。

| 項目 | 事実 | 設計への影響 |
|---|---|---|
| Type-C（H2） | D+/D- → `USB_DBG_D_P/N` → **CH343P（USB-UART）のみ**。CC は 5.1kΩ プルダウン済 | 書き込み・ログ・給電専用。**USBデータは通らない** |
| 4ピン USB ヘッダ（P1, MX1.25 4P） | `VCC_5V / USBD_N / USBD_P / GND`。USBD_P/N は ESP32-P4 の **専用 HS PHY ピン（49/50）** | nano を繋ぐ HS ホストポート。5V はスイッチ無しの直出し |
| FS USB PHY | **GPIO26 = D- / GPIO27 = D+**（デフォルトで OTG_FS 側に接続。eFuse 不要）。両方ヘッダに出ている | PC 向け MSC デバイス用 |
| 電源経路 | Type-C `USB0_5V` → Q2(AO3401 Pch) → `VCC_5V` → MP1658（3A）→ `ESP_3V3` | 外部 5V は **VSYS ピン（=VCC_5V）**に入れる |
| Pico 互換ピン | **VBUS = USB0_5V、VSYS = VCC_5V、3V3 = ESP_3V3**。パッド番号は資料間で食い違い（KiCad シンボルでは VBUS=21 / VSYS=22 / 3V3=25）。**シルクの名前で確認する**（要実機確認） | |
| LED1 | VCC_5V の電源表示（基板上、ケースに入れると見えない） | ケース面には別 LED を付ける |
| TF スロット | SDIO 4bit: CLK=43 CMD=44 D0=39 D1=40 D2=41 D3=42、電源スイッチ Q1(SI2301)、CD=GPIO45(要確認) | 短体スロット。**基板裏か表か実物で確認**（ケース切り欠き位置） |
| コーデック | ES8311 + NS4150B（I2C=GPIO7/8、I2S=GPIO9-13、PA_EN=GPIO53） | 今回は未使用（音は nano から来る） |
| ストラッピング | GPIO34〜38 | ボタン/LED に使わない |
| 空き GPIO（ヘッダ） | 2,3,4,5,6,14-19,20,21,22,23,32,33,46,47,48,54 | ボタン・LED はここから |

## 全体構成

```
GBA ──(既存タップ19本)── nanoCH32V305 ═══ USB-C(USB2-HS側) ═══ [Type-Cオス変換基板]
                        (FW v0.33 無変更)                           │ 1.25mm 4P ケーブル
                                                                    ▼  ケース壁のグロメット穴
 ┌──────────── タカチ SW-75B ───────────────────────────────────────────┐
 │  [MX1.25 4P] ESP32-P4-Pico ─── TF(microSD, FAT32) ── mp4             │
 │   5V/D+/D-/G   │ GPIO2 ← タクトSW(TS-06104)  GPIO3 → 録画LED           │
 │                │ 3V3 ──1kΩ── 電源LED                                 │
 │                │ GPIO26/27 ←→ [パネルType-Cメス] D-/D+   ← PC(MSC, FS) │
 │                │ VSYS ←SS14← [パネルType-Cメス] VBUS   ← モバイルバッテリー/PC │
 │  [基板のType-C] ← 書き込み/ログ専用（壁に穴だけ開ける）                 │
 └─────────────────────────────────────────────────────────────────────┘
```

## 配線表（P4-Pico ↔ 追加部品）

> 実作業版（はんだ・結線・使い方）は **docs/recorder-wiring-esp32p4.md**。ピンは番号でなくシルク名で書く。

| 部品 | P4-Pico 側 | 備考 |
|---|---|---|
| nano へのケーブル（MX1.25 4P） | P1: V=VCC_5V / D- / D+ / G | 反対側は Type-C オス変換基板へ **VBUS, D-, D+, GND** の4線。CC は不要（nano は受電側だがデバイス機なので CC 処理不要。nano 側 J1 は 5V 直結） |
| パネル Type-C メス（秋月 AE-USB2.0-TYPE-C-PNL） | VBUS →**SS14（ショットキー）**→ **VSYS（シルク名）**、GND → GND、D+ → **GPIO27**、D- → **GPIO26** | CC1/CC2 に **5.1kΩ プルダウン必須**（C-C ケーブルで 5V を出させるため）。SS14 は基板 Type-C と同時挿しで 5V 同士がぶつかるのを防ぐ（約0.3V 落ちるが 3V3 レギュレータも nano の LDO も問題なし） |
| タクトスイッチ TS-06104 | **GPIO2** ↔ GND | 内部プルアップ。30ms デバウンス。表面実装品なので **小片のユニバーサル基板か SMD→2.54 変換基板に載せてリード線を出す** |
| 録画 LED（3mm 赤） | **GPIO3** → 470Ω → LED → GND | |
| 電源 LED（3mm 緑） | **3V3（シルク名）** → 1kΩ → LED → GND | 給電されていれば点灯 |
| D+/D- 配線 | 15cm 以内、2本を撚る | FS なので神経質でなくてよい |

## ソフトウェア構成（ESP-IDF 5.5 以降、C）

### 使うコンポーネント

| 役割 | コンポーネント | 備考 |
|---|---|---|
| nano からの受信 | `usb_host`（IDF 標準）、HS コントローラ（peripheral 0） | ベンダークラスなのでクラスドライバ不要。`usb_host_transfer_submit` を直接使う |
| PC への MSC | `esp_tinyusb`（`tinyusb_config_t.port = TINYUSB_PORT_FULL_SPEED_0`）+ `tinyusb_msc_new_storage_sdmmc()` | **HS ホストと FS デバイスの同時動作は公式に未実証** → 最初に検証（下記リスク3） |
| 4倍拡大 + YUV 変換 | 自前（LUT + 最近傍） | PPA の SRM はバイリニア補間でドットが滲むので使わない |
| H.264 | `esp_h264` HW エンコーダ、入力 `ESP_H264_RAW_FMT_O_UYY_E_VYY`、Baseline、RC 16Mbps、GOP 60 | 幅高さは 16 の倍数が安全（960×640 は OK） |
| 音声 | `esp_audio_codec`（リサンプラ + AAC-LC エンコーダ） | 32802Hz → 48000Hz ステレオ 128kbps |
| mp4 | `esp_muxer`（MP4） | もしくは `esp_capture` の mp4 シンク。カスタムソースを書くより muxer を直接叩く方が見通しが良い |
| SD | `sdmmc`（4bit）+ FATFS | `CONFIG_FATFS_ALLOC_PREFER_ALIGNED_WORK_BUFFERS=y`、`IMMEDIATE_FSYNC=n`、allocation_unit 64KB |
| 設定 | NVS: `scale`(1-4, 既定4), `bitrate_kbps`(既定16000), `split_min`(既定15) | 倍率はここで変更 |

### データフロー（タスク分担）

```
[core0] usb_rx タスク
   EP4/EP1(映像) EP3(音声) それぞれ 8本の 512B 転送を常時キュー
   → 484B パケットをマジックで振り分け
   → 映像: フレーム番号/ライン番号で 240×160×16bit フレームバッファ（内蔵RAM, 2面）へ配置
   → 音声: 0xA6 のシーケンス番号で欠落検知、16bit PCM リングへ
[core1] scale タスク（フレーム確定ごと）
   15bit RGB → LUT(32768×3B=96KB, 内蔵RAM) → Y,U,V
   4×4 ブロックに最近傍展開しつつ O_UYY_E_VYY 形式で PSRAM の YUV フレーム（921.6KB, 3面リング）へ
[HW]   esp_h264 エンコード → NAL を PSRAM リングへ
[core1] audio タスク: リサンプル → AAC
[core0] mux/writer タスク: esp_muxer に映像/音声を PTS 付きで投入 → 512KB 単位で SD へ書き出し
```

- **PTS**: 映像 = フレーム番号 × (1/59.7275s)、音声 = 累積サンプル数 / 48000。どちらも GBA の
  クロック由来なので長時間でもズレない。欠落フレームは前フレームを再投入して番号を詰めない
- **4倍拡大の理由**（改訂4から継承）: 4:2:0 でも 1ドット = 4×4 画素なら色滲みが見えない。
  O_UYY_E_VYY はドット内で同色なので色差も厳密に一致する
- **ファイル名**: P4-Pico に RTC 電池が無いので `REC_0001.mp4` 連番 + 分割は `_p2`。
  日時が欲しくなったら DS3231 を I2C(GPIO7/8) に足す（改訂4の部品がそのまま使える）
- **MSC と録画の排他**: 録画中は MSC 側に "メディア無し" を返す。PC がマウント中は
  録画ボタンを無視して録画 LED を 2 回点滅

### 処理量の見積もり（要実測）

| 項目 | 見積 | 根拠 |
|---|---|---|
| USB 受信 | 映像 9,557 + 音声 683 ≈ **10.2k 転送/秒** | 484B < 512B(MPS) なので毎パケットがショートパケット = 転送完了割り込み。360MHz なら処理可能のはずだが **一番の未知数** |
| 拡大+変換 | 921.6KB 書き込み/フレーム ≈ 55MB/s | PSRAM 帯域の 1/4 程度。1 コアで 5ms/フレーム以下の見込み |
| H.264 HW | 1080p30 比で画素レート 59% → **約 10ms/フレーム** | 60fps の 16.7ms に収まる見込み |
| SD 書き込み | 16Mbps = 2MB/s | SDIO 4bit で余裕 |
| 内蔵 RAM | フレーム 154KB + LUT 96KB + USB 12KB + FATFS + スタック ≈ 400KB / 768KB | |
| PSRAM | YUV 2.8MB + NAL 1MB + 書き込みバッファ 1MB ≈ 5MB / 32MB | |

## 未検証リスク（優先順）と検証方法

1. **HW エンコーダが 960×640@60 を回せるか** — テストパターンを `esp_h264` に流し 1 フレームの
   所要時間を計測。NG なら 3倍(720×480) か 30fps に落とす（倍率は設定で変えられる設計にしておく）
2. **usb_host が 10k 転送/秒を落とさず受けられるか** — nano を繋いで `freq`/`framestat` 相当を
   P4 側で再現、1分間のフレーム欠落 0 を確認。NG の場合の逃げ道: 転送キュー深さ増、受信を
   core0 専用化。それでも駄目なら nano FW で 3 ライン束ね送信（1452B）を**別 FW として**追加
   （既定 FW は変えない）
3. **HS ホスト + FS デバイス（TinyUSB）の同時動作** — Espressif のドキュメントでは
   両コントローラは独立だが、同時運用は「ハード検証されていない」と PR に明記。
   MSC だけ・ホストだけ・同時、の順で確認。NG なら MSC 機能を諦めてカード抜き運用に割り切る
4. **FAT32 で 4GB 直前のファイル分割が正しく閉じられるか**（moov の書き込み）
5. **電源断耐性** — 録画中に抜いた mp4 が壊れる。対策は分割を短く（5分）+ 停止ボタンを押す運用

## ケース（SW-75B）レイアウト

- 内寸は **約 45.5W × 70.5D × 26H mm（推定）**。カタログの寸法表は SW-55 までしか抽出できず、
  SW-55 の「外形−4.5mm」の比率で推定した。**タカチ公式の CAD/PDF 図面で確定すること**
- ESP32-P4-Pico の外形は公式に数値が無い。写真のピンヘッダ間隔(48.3mm)からの推定で
  **約 58 × 31 mm**。**実物をノギスで測る**
- 配置（上から見た図、蓋側にボタンと LED）:

```
 短辺A                                                            短辺B
 ┌──[基板Type-C用の穴]──────────────────────────────────────────────┐
 │   ESP32-P4-Pico（長手方向に置く。両面テープ or 3mm スペーサ）       │
 │   ┌──────────────────────────────────────────┐  [MX1.25]→ グロメット穴φ4 → nano へ │
 │   │ Type-C  ESP32-P4   TF(裏?)   MX1.25 4P  │                     │
 │   └──────────────────────────────────────────┘  [パネルType-Cメス] │
 │   小基板: タクトSW + LED×2 + 抵抗 → 蓋の穴へ                       │
 └────────────────────────────────────────────────────────────────┘
```

- 短辺A: 基板の Type-C がのぞく穴（書き込み・ログ用。普段はテープで塞いでもよい）
- 短辺B: パネル Type-C メス（秋月キットは L 金具でネジ止め）+ 4線ケーブルのグロメット穴
- 蓋: ボタン穴 φ3.5（タクトSW のステムが出る位置）、LED 穴 φ3 ×2
- 側面: TF スロットの位置に合わせて切り欠き（基板裏なら底面側）。開閉がワンタッチなので
  「蓋を開けてカードを抜く」運用でも可
- 通気: 側面に φ2 を数個。P4 は録画中 1〜1.5W 程度でほんのり熱い

## 購入・準備リスト

| 品 | 備考 |
|---|---|
| **MX1.25 4P ハウジング付きケーブル** | 手持ちは 6P なので不適合。製品同梱の「MX1.25 4PIN to USB cable」の先端が何かを先に確認 |
| Type-C オス変換基板（手持ち） | VBUS/D+/D-/GND の 4 点に半田 |
| 秋月 USB Type-C パネル取付キット AE-USB2.0-TYPE-C-PNL | + 5.1kΩ ×2 + SS14 ×1 |
| TS-06104（手持ち）+ SMD→2.54 変換基板 or ユニバーサル基板の切れ端 | |
| LED 3mm ×2（赤/緑）+ 470Ω + 1kΩ | |
| microSD 64GB（UHS-I A1 以上） | **FAT32 でフォーマット**（Windows 標準では 32GB 超を FAT32 にできないので guiformat 等を使う。または P4 側で初回フォーマット） |
| タカチ SW-75B（手持ち） | + ゴムグロメット φ4、3mm スペーサ or 厚手両面テープ |
| モバイルバッテリー | 低負荷自動 OFF しないものを事前確認（消費は 5V 0.3〜0.4A 程度の見込み） |

## 開発ステップ

1. **環境**: ESP-IDF 5.5+（P4 対応）、`idf.py set-target esp32p4`。Type-C(UART) で Lチカ・SD マウント
2. **リスク1**: `esp_h264` HW でテストパターン 960×640 を 600 フレーム連続エンコード、所要時間計測
3. **リスク2**: `usb_host` で nano（VID 1209 / PID C001）を開き EP4/EP1/EP3 を受信、
   フレーム組立まで。GbaCap のパケット解析ロジックを C に移植（純粋関数部分はホストで TDD）
4. **リスク3**: FS 側 TinyUSB MSC（SD）単体 → ホストと同時
5. 統合: 拡大変換 → エンコード → AAC → mp4 → SD。PC で ffprobe / 再生して A/V 同期確認
6. ボタン / LED / 分割 / MSC 排他 / NVS 設定
7. モバイルバッテリーで 30 分連続録画、発熱確認
8. ケース加工・組み込み

## 参考

- Waveshare ESP32-P4-Pico wiki: https://www.waveshare.com/wiki/ESP32-P4-Pico （回路図 PDF は wiki の Resources）
- ESP-USB Host (P4): https://docs.espressif.com/projects/esp-usb/en/latest/esp32p4/usb_host.html
- ESP-USB Device / TinyUSB (P4): https://docs.espressif.com/projects/esp-usb/en/latest/esp32p4/usb_device.html
- ESP-FAQ USB（PHY の割り当て）: https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/usb.html
- esp_h264: https://components.espressif.com/components/espressif/esp_h264
- esp_capture / esp-gmf: https://github.com/espressif/esp-gmf
- ESP-IDF FATFS（exFAT 無効・最適化オプション）: https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-reference/storage/fatfs.html
- PPA（バイリニア補間のため不採用）: https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-reference/peripherals/ppa.html
