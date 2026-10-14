/* Waveshare ESP32-P4-Pico のピン割り当て。
 * docs/recorder-plan-esp32p4.md の配線表どおり。
 * 40pin ヘッダの番号系は回路図シンボルと文書で食い違うので、
 * 必ずシルクの GPIO 名(=ここでいう番号)で書くこと。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* --- ユーザー操作系(はんだ付けする場所) --- */
#define GBAC_PIN_BTN        2   /* タクト TS-06104 ↔ GND。内部プルアップ */
#define GBAC_PIN_LED_REC    3   /* 録画 LED → 470Ω → GND */

/* --- microSD(SDMMC 4bit) ---
 * 回路図(Waveshare 公式 PDF)で TF1 の 6 本が NLGPIO39〜44 だと確認できた。
 * P4 では slot0 が GPIO マトリクス非対応(hal/esp32p4/include/hal/sdmmc_ll.h の
 * SDMMC_LL_SLOT_SUPPORT_GPIO_MATRIX(0) == 0)で、これらのピンは slot0 の IOMUX 直結。
 * つまり TF スロットは slot0。IDF の SDMMC_HOST_DEFAULT() は slot=1 なので注意。 */
#define GBAC_PIN_SD_CLK     43
#define GBAC_PIN_SD_CMD     44
#define GBAC_PIN_SD_D0      39
#define GBAC_PIN_SD_D1      40
#define GBAC_PIN_SD_D2      41
#define GBAC_PIN_SD_D3      42
/* TF1 の隣に Q1 + R17/R22 があり、R17 側へ NLGPIO45(slot0 の D4)が来ている。
 * Q1 が TF の電源スイッチなら、この GPIO を駆動しないとカードに電気が行かない
 * (4bit モードでは D4 は使われないので GPIO 出力にできる)。要検証。 */
#define GBAC_PIN_SD_PWR     45

/* --- PC に挿した側(FullSpeed。SD をマスストレージとして見せる) --- */
#define GBAC_PIN_USB_FS_DM  26  /* D- */
#define GBAC_PIN_USB_FS_DP  27  /* D+ */

/* nanoCH32V305 をつなぐ HighSpeed 側は専用 UTMI ピン(GPIO49/50)。
 * MX1.25 4pin ヘッダ P1 の VCC_5V / USBD_N / USBD_P / GND に直結。
 * GPIO 番号では指定しない(内部 PHY なので設定不要)。 */

/* TF カードの電源スイッチ Q1(SI2301、ゲート Low でオン)のゲート GPIO は
 * 回路図で未確定。Kconfig(GBAC_SD_PWR_GPIO)で変えられるようにしておき、
 * -1 の間は「常時給電」として扱う(要検証)。 */
#include "sdkconfig.h"
#ifdef CONFIG_GBAC_SD_PWR_GPIO
#define GBAC_SD_PWR_GPIO    CONFIG_GBAC_SD_PWR_GPIO
#else
#define GBAC_SD_PWR_GPIO    (-1)
#endif

esp_err_t gbac_board_init(void);

/* 録画 LED。点灯 = 録画中 */
void gbac_led_rec(bool on);

/* 停止中に n 回点滅させる(阻塞する。ボタン応答の合図などに使う) */
void gbac_led_blink(int times, int half_ms);
