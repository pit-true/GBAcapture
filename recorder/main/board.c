#include "board.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

esp_err_t gbac_board_init(void)
{
    gpio_config_t led = {
        .pin_bit_mask = 1ULL << GBAC_PIN_LED_REC,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led));
    gpio_set_level(GBAC_PIN_LED_REC, 0);

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << GBAC_PIN_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));

#if GBAC_SD_PWR_GPIO >= 0
    gpio_config_t pwr = {
        .pin_bit_mask = 1ULL << GBAC_SD_PWR_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pwr));
    gpio_set_level(GBAC_SD_PWR_GPIO, 0); /* SI2301 はゲート Low でオン */
#endif

    ESP_LOGI(TAG, "btn=GPIO%d led=GPIO%d sd=%d,%d,%d-%d",
             GBAC_PIN_BTN, GBAC_PIN_LED_REC,
             GBAC_PIN_SD_CLK, GBAC_PIN_SD_CMD, GBAC_PIN_SD_D0, GBAC_PIN_SD_D3);
    return ESP_OK;
}

void gbac_led_rec(bool on)
{
    gpio_set_level(GBAC_PIN_LED_REC, on ? 1 : 0);
}

void gbac_led_blink(int times, int half_ms)
{
    for (int i = 0; i < times; i++) {
        gpio_set_level(GBAC_PIN_LED_REC, 1);
        vTaskDelay(pdMS_TO_TICKS(half_ms));
        gpio_set_level(GBAC_PIN_LED_REC, 0);
        vTaskDelay(pdMS_TO_TICKS(half_ms));
    }
}
