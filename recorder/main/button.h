/* タクトスイッチ 1 個。押す → 録画開始、もう一度押す → 停止。
 * GPIO2 ↔ GND。内部プルアップなので押すと Low。
 * 割り込みは使わず 5ms 周期で読んで 30ms 安定したら確定する(プランどおり)。 */
#pragma once

#include "freertos/FreeRTOS.h"

void gbac_button_init(void);

/* イベントを 1 個取り出す。timeout_ticks を待つ。
 * 戻り値: GBAC_BTN_SHORT = 短く押して離した、GBAC_BTN_LONG = 2 秒押し続けた、
 *         0 = タイムアウト
 * 長押しのときは離しても短押しは出ない(2026-10-18)。 */
#define GBAC_BTN_SHORT 1
#define GBAC_BTN_LONG  2
#define GBAC_BTN_LONG_MS 2000
int gbac_button_wait(TickType_t timeout_ticks);

/* いま押されているか(デバウンスなしの生の値)。起動時に「押しながら給電」を
 * 見るために使う。1 = 押されている */
int gbac_button_is_down(void);
