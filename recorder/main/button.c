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
 * GPIO2 はロープルアップで、スイッチが GND に落ちる=押下。 */
static void btn_task(void *arg)
{
    (void)arg;
    int stable = 1;
    int last_raw = gpio_get_level(GBAC_PIN_BTN);
    TickType_t changed = xTaskGetTickCount();

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
                int ev = 1;
                xQueueSend(g_q, &ev, 0);
            }
        }
    }
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
    return xQueueReceive(g_q, &ev, timeout_ticks) == pdTRUE ? 1 : 0;
}
