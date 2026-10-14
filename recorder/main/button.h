/* タクトスイッチ 1 個。押す → 録画開始、もう一度押す → 停止。
 * GPIO2 ↔ GND。内部プルアップなので押すと Low。
 * 割り込みは使わず 5ms 周期で読んで 30ms 安定したら確定する(プランどおり)。 */
#pragma once

#include "freertos/FreeRTOS.h"

void gbac_button_init(void);

/* イベントを 1 個取り出す。timeout_ticks を待つ。
 * 戻り値: 1 = 押された、0 = タイムアウト */
int gbac_button_wait(TickType_t timeout_ticks);
