#include "button.h"

#include "board.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define POLL_MS      5
#define DEBOUNCE_MS 30

static QueueHandle_t g_q;

/* ボタンは ISR にせず 5ms 周期で読む。30ms 同じ状態が続いたら確定させる。
 * GPIO2 はロープルアップで、スイッチが GND に落ちる=押下。
 * 短押し = 押して 2 秒未満で離した(離したときに出す)。
 * 長押し = 2 秒押し続けた(2 秒たった時点で 1 回出す。離しても短押しは出さない)。 */
static void btn_task(void *arg)
{
    (void)arg;
    int stable = 1;
    int last_raw = gpio_get_level(GBAC_PIN_BTN);
    TickType_t changed = xTaskGetTickCount();
    TickType_t pressed_at = 0;
    int long_sent = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        int raw = gpio_get_level(GBAC_PIN_BTN);
        TickType_t now = xTaskGetTickCount();
        if (raw != last_raw) {
            last_raw = raw;
            changed = now;
        } else if (raw != stable && (now - changed) >= pdMS_TO_TICKS(DEBOUNCE_MS)) {
            stable = raw;
            if (raw == 0) {
                pressed_at = now;
                long_sent = 0;
            } else if (!long_sent) {
                int ev = GBAC_BTN_SHORT;
                xQueueSend(g_q, &ev, 0);
            }
        }
        if (stable == 0 && !long_sent && (now - pressed_at) >= pdMS_TO_TICKS(GBAC_BTN_LONG_MS)) {
            long_sent = 1;
            int ev = GBAC_BTN_LONG;
            xQueueSend(g_q, &ev, 0);
        }
    }
}

int gbac_button_is_down(void)
{
    return gpio_get_level(GBAC_PIN_BTN) == 0 ? 1 : 0;
}

void gbac_button_init(void)
{
    if (g_q) {
        return;
    }
    g_q = xQueueCreate(4, sizeof(int));
    xTaskCreate(btn_task, "btn", 3072, NULL, 3, NULL);
}

int gbac_button_wait(TickType_t timeout_ticks)
{
    int ev;
    if (!g_q) {
        return 0;
    }
    return xQueueReceive(g_q, &ev, timeout_ticks) == pdTRUE ? ev : 0;
}
