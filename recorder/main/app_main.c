/* GBA キャプチャ録画機(ESP32-P4-Pico)の本体。
 *   nanoCH32V305(USB-HS) → 受信 → 4x 拡大 → HW H.264 + AAC → microSD の mp4
 * ボタン 1 回で録画開始、もう 1 回で停止。docs/recorder-plan-esp32p4.md 参照。 */
#include <stdio.h>

#include "audio_pipe.h"
#include "board.h"
#include "button.h"
#include "cfg_nvs.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "frame_pipe.h"
#include "gbacap.h"
#include "sdkconfig.h"
#include "sd_recorder.h"
#include "usb_host_nano.h"
#include "video_enc.h"

static const char *TAG = "main";

static gbac_cfg_t g_cfg;
static bool g_recording;

static void on_packet(const uint8_t *pkt, int len, void *arg)
{
    (void)arg;
    if (len > 0 && pkt[0] == GBAC_LINE_MAGIC) {
        frame_pipe_push(pkt, len);
    } else if (len > 0 && pkt[0] == GBAC_AUDIO_MAGIC) {
        audio_pipe_push(pkt, len);
    }
}

static void show_status(void)
{
    ESP_LOGI(TAG, "usb pkts=%llu err=%llu | frame ok=%llu drop=%llu | aud gap=%llu ovf=%llu | enc f=%llu e=%llu scale<=%uus enc<=%uus",
             (unsigned long long)gbac_usb_packets(), (unsigned long long)gbac_usb_errors(),
             (unsigned long long)frame_pipe_completed(), (unsigned long long)frame_pipe_dropped(),
             (unsigned long long)audio_pipe_gaps(), (unsigned long long)audio_pipe_overruns(),
             (unsigned long long)gbac_enc_frames(), (unsigned long long)gbac_enc_errors(),
             (unsigned)gbac_enc_scale_us_max(), (unsigned)gbac_enc_enc_us_max());
}

static void start_recording(void)
{
    if (!gbac_sd_ready() && gbac_sd_mount() != ESP_OK) {
        ESP_LOGE(TAG, "SD がないので録画できない");
        gbac_led_blink(4, 100);
        return;
    }
    frame_pipe_stats_reset();
    audio_pipe_reset();
    if (gbac_rec_start(&g_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "録画を開始できない");
        gbac_led_blink(4, 100);
        return;
    }
    gbac_usb_stream_start();
    gbac_led_rec(true);
    g_recording = true;
    ESP_LOGI(TAG, "録画開始 scale=%dx %dkbps split=%dmin vol=%d%%",
             g_cfg.scale, g_cfg.bitrate_kbps, g_cfg.split_min, g_cfg.volume_pct);
}

static void stop_recording(void)
{
    gbac_usb_stream_stop();
    gbac_rec_stop();
    gbac_led_rec(false);
    g_recording = false;
    show_status();
}

void app_main(void)
{
    gbac_board_init();
    gbac_cfg_load(&g_cfg);
    ESP_LOGI(TAG, "設定 scale=%d bitrate=%dkbps split=%dmin vol=%d%%",
             g_cfg.scale, g_cfg.bitrate_kbps, g_cfg.split_min, g_cfg.volume_pct);

    frame_pipe_init();
    audio_pipe_init(g_cfg.volume_pct);
    gbac_button_init();
    gbac_usb_start(on_packet, NULL);
    if (gbac_sd_mount() != ESP_OK) {
        ESP_LOGW(TAG, "SD 未挿入。挿してからボタンを押してね");
    }

#if CONFIG_GBAC_BENCH_FRAMES > 0
    /* 手順 2: HW エンコーダが 960x640@60 に耐えるかの実測。 */
    gbac_rec_bench(&g_cfg, CONFIG_GBAC_BENCH_FRAMES);
    show_status();
#endif

    int64_t last_log = esp_timer_get_time();
    while (1) {
        if (gbac_button_wait(pdMS_TO_TICKS(1000))) {
            if (g_recording) {
                stop_recording();
            } else {
                start_recording();
            }
        }
        if (g_recording && !gbac_usb_ready()) {
            ESP_LOGE(TAG, "nano が外れた。録画を止める");
            stop_recording();
        }
        if (esp_timer_get_time() - last_log > 10 * 1000 * 1000) {
            last_log = esp_timer_get_time();
            show_status();
        }
    }
}
