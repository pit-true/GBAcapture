#include "video_enc.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "frame_pipe.h"
#include "gbacap.h"

static const char *TAG = "enc";

#define NAL_SLOTS      8
#define NAL_SLOT_BYTES (256 * 1024)

static QueueHandle_t g_free_q;    /* 空いている NAL スロット index */
static QueueHandle_t g_out_q;     /* 完成した gbac_nal_t */
static uint8_t *g_pool[NAL_SLOTS];
static uint8_t *g_yuv;
static uint8_t *g_encout;
static uint32_t g_yuv_len;
static uint32_t g_encout_len;
static esp_h264_enc_handle_t g_enc;
static esp_h264_enc_param_hw_handle_t g_param;
static TaskHandle_t g_task;
static volatile bool g_run;
static bool g_bench;
static volatile int g_bench_left;
static uint32_t g_frames;
static uint32_t g_errs;
static uint32_t g_scale_us_max;
static uint32_t g_enc_us_max;
static int g_scale = 4;
static int g_W = 960;
static int g_H = 640;
static uint16_t *g_bench_src;     /* 合成パターンの元画像(240x160 RGB565) */

/* 合成パターン: 動いている市松 + 横線。エンコーダが実際にビットレートを
 * 使うような動きを入れる。 */
static void bench_fill(uint16_t *dst, int n)
{
    static int phase;
    phase = (phase + 1) & 31;
    for (int i = 0; i < n; i++) {
        int x = i % GBAC_WIDTH;
        int y = i / GBAC_WIDTH;
        int v = (((x + phase) >> 4) ^ ((y + phase) >> 4)) & 1;
        uint16_t c = v ? 0xF81F : 0x07E0;
        if ((y & 7) == (phase >> 2)) {
            c = 0xFFFF;
        }
        dst[i] = c;
    }
}

static void enc_task(void *arg)
{
    (void)arg;
    int done = 0;

    while (g_run) {
        if (g_bench) {
            if (g_bench_left > 0) {
                g_bench_left--;
            } else {
                g_run = false;
                break;
            }
        }

        uint64_t pts_us = 0;
        const uint16_t *px = NULL;
        if (g_bench) {
            bench_fill(g_bench_src, GBAC_WIDTH * GBAC_HEIGHT);
            px = g_bench_src;
            pts_us = (uint64_t)done * 1000000ULL / 60;
        } else {
            px = frame_pipe_take(&pts_us, pdMS_TO_TICKS(100));
            if (px == NULL) {
                continue;
            }
        }

        int64_t t0 = esp_timer_get_time();
        gbac_scale_ouyy(px, GBAC_WIDTH, GBAC_HEIGHT, g_scale, g_yuv);
        int64_t t1 = esp_timer_get_time();
        if (!g_bench) {
            frame_pipe_release();
        }
        if ((uint32_t)(t1 - t0) > g_scale_us_max) {
            g_scale_us_max = (uint32_t)(t1 - t0);
        }

        int slot = -1;
        if (xQueueReceive(g_free_q, &slot, pdMS_TO_TICKS(200)) != pdTRUE) {
            g_errs++;
            ESP_LOGW(TAG, "NAL スロットが空かない(mux が遅い)");
            continue;
        }

        esp_h264_enc_in_frame_t in = { 0 };
        in.raw_data.buffer = g_yuv;
        in.raw_data.len = g_yuv_len;
        in.pts = (uint32_t)(pts_us / 1000);

        esp_h264_enc_out_frame_t out = { 0 };
        out.raw_data.buffer = g_encout;
        out.raw_data.len = g_encout_len;

        esp_h264_err_t e = esp_h264_enc_process(g_enc, &in, &out);
        int64_t t2 = esp_timer_get_time();
        if ((uint32_t)(t2 - t1) > g_enc_us_max) {
            g_enc_us_max = (uint32_t)(t2 - t1);
        }

        if (e != ESP_H264_ERR_OK) {
            g_errs++;
            xQueueSend(g_free_q, &slot, 0);
            ESP_LOGW(TAG, "encode 失敗(%d)", (int)e);
            continue;
        }
        if ((int)out.length > NAL_SLOT_BYTES) {
            g_errs++;
            xQueueSend(g_free_q, &slot, 0);
            ESP_LOGE(TAG, "NAL が %d バイトありスロットを超える", (int)out.length);
            continue;
        }

        memcpy(g_pool[slot], out.raw_data.buffer, out.length);
        gbac_nal_t n = {
            .buf = g_pool[slot],
            .len = (int)out.length,
            .pts_us = pts_us,
            .key = (out.frame_type == ESP_H264_FRAME_TYPE_IDR ||
                    out.frame_type == ESP_H264_FRAME_TYPE_I),
        };
        if (!xQueueSend(g_out_q, &n, 0)) {
            g_errs++;
            xQueueSend(g_free_q, &slot, 0);
            continue;
        }
        g_frames++;
        done++;
    }

    ESP_LOGI(TAG, "エンコード終了 frames=%u errs=%u scale_max=%uus enc_max=%uus stack 残り=%uB",
             (unsigned)g_frames, (unsigned)g_errs,
             (unsigned)g_scale_us_max, (unsigned)g_enc_us_max,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (g_bench) {
        const int n = done > 0 ? (int)done : 1;
        ESP_LOGI(TAG, "スケール内訳 us/枚 rgb=%.0f y=%.0f uv=%.0f copy=%.0f",
                 (double)gbac_scale_prof[0] / n, (double)gbac_scale_prof[1] / n,
                 (double)gbac_scale_prof[2] / n, (double)gbac_scale_prof[3] / n);
    }
    g_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t gbac_enc_start(const gbac_cfg_t *cfg, int bench_frames)
{
    if (g_run) {
        return ESP_ERR_INVALID_STATE;
    }
    g_scale = cfg->scale;
    g_W = GBAC_WIDTH * g_scale;
    g_H = GBAC_HEIGHT * g_scale;
    g_yuv_len = GBAC_OUYY_SIZE(GBAC_WIDTH, GBAC_HEIGHT, g_scale);
    g_encout_len = g_yuv_len;

    /* YUV は 1 枚で足りる。esp_h264_enc_process は同期なので、
     * エンコード中に次のフレームを書き換えることがない。 */
    if (g_yuv == NULL) {
        /* esp_h264 はエンコード後に出力バッファを
         * esp_cache_msync(..., DIR_M2C) で無効化する。この方向は
         * UNALIGNED フラグを使えず、アドレスとサイズがキャッシュラインの
         * 倍数でないと ESP_ERR_INVALID_ARG を返して無効化が飛ぶ
         * (components/esp_mm/esp_cache_msync.c 120 行目)。
         * P4 の既定ラインは 64B だが Espressif の P4 テスト設定は 128B を
         * 選ぶので、どちらでも安全な 128 に揃える。
         * サイズ側は 960*640*3/2 = 921600 で 128 の倍数。 */
        g_yuv = heap_caps_aligned_alloc(128, g_yuv_len, MALLOC_CAP_SPIRAM);
        g_encout = heap_caps_aligned_alloc(128, g_encout_len, MALLOC_CAP_SPIRAM);
        if (g_yuv == NULL || g_encout == NULL) {
            ESP_LOGE(TAG, "PSRAM に %u バイト x2 が確保できない", (unsigned)g_yuv_len);
            return ESP_ERR_NO_MEM;
        }
    }
    for (int i = 0; i < NAL_SLOTS; i++) {
        if (g_pool[i] == NULL) {
            g_pool[i] = heap_caps_malloc(NAL_SLOT_BYTES, MALLOC_CAP_SPIRAM);
            if (g_pool[i] == NULL) {
                return ESP_ERR_NO_MEM;
            }
        }
    }
    if (g_free_q == NULL) {
        g_free_q = xQueueCreate(NAL_SLOTS, sizeof(int));
        g_out_q = xQueueCreate(NAL_SLOTS, sizeof(gbac_nal_t));
    }
    xQueueReset(g_free_q);
    xQueueReset(g_out_q);
    for (int i = 0; i < NAL_SLOTS; i++) {
        int idx = i;
        xQueueSend(g_free_q, &idx, 0);
    }

    if (bench_frames > 0) {
        if (g_bench_src == NULL) {
            g_bench_src = heap_caps_malloc(sizeof(uint16_t) * GBAC_WIDTH * GBAC_HEIGHT,
                                           MALLOC_CAP_SPIRAM);
            if (g_bench_src == NULL) {
                return ESP_ERR_NO_MEM;
            }
        }
        g_bench = true;
        g_bench_left = bench_frames;
    } else {
        g_bench = false;
        g_bench_left = -1;
    }

    esp_h264_enc_cfg_t c = { 0 };
    c.pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY;
    c.gop = 60;
    c.fps = 60;
    c.res.width = (uint16_t)g_W;
    c.res.height = (uint16_t)g_H;
    c.rc.bitrate = (uint32_t)cfg->bitrate_kbps * 1000u;
    c.rc.qp_min = 10;
    c.rc.qp_max = ESP_H264_QP_MAX;

    esp_h264_err_t e = esp_h264_enc_hw_new(&c, &g_enc);
    if (e != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "enc_hw_new 失敗(%d) %dx%d", (int)e, g_W, g_H);
        return ESP_FAIL;
    }
    e = esp_h264_enc_open(g_enc);
    if (e != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "enc_open 失敗(%d)", (int)e);
        esp_h264_enc_del(g_enc);
        g_enc = NULL;
        return ESP_FAIL;
    }
    /* IDR を任意の位置で打つためのパラメータハンドル。
     * mp4 のスライス先頭を IDR にしたいので muxer に教える必要がある。 */
    e = esp_h264_enc_hw_get_param_hd(g_enc, &g_param);
    if (e != ESP_H264_ERR_OK) {
        ESP_LOGW(TAG, "get_param_hd 失敗(%d)。IDR 強制は使えない", (int)e);
        g_param = NULL;
    }

    g_frames = 0;
    g_errs = 0;
    g_scale_us_max = 0;
    g_enc_us_max = 0;
    for (int i = 0; i < 4; i++) {
        gbac_scale_prof[i] = 0;
    }
    g_run = true;
    ESP_LOGI(TAG, "エンコード開始 %dx%d scale=%d %dkbps bench=%d",
             g_W, g_H, g_scale, (int)cfg->bitrate_kbps, bench_frames);
    /* gbac_scale_ouyy だけで約 4.2KB のスタックを使う。実機(2026-10-14)で
     * 4096 バイトだと "Stack protection fault" で落ちたので 12KB にした。
     * 終了時に high water mark を出すので、それで実際の必要量が分かる。
     *
     * コアは 0 へ移した(2026-10-16 r9/r10 実測)。core1 に置くと enc(prio5)が
     * 1 フレーム 40〜113ms 走り続けて rx(prio4)を starvationさせ、USB rx リング
     * (128 枠=9ms)が溢れて r9 では 61,763 パケット、うち音声約 4,100 パケット
     * (6 秒分)が捨てられていた。rx は 1 パケット 26µs で CPU 26% なので、
     * 同一コアに重い enc を置くこと自体が問題。
     * core0 の上位(usb_lib=22, usbh=21)は enc(prio5)より上なので USB 受信は邪魔されない。 */
    xTaskCreatePinnedToCore(enc_task, "enc", 12288, NULL, 5, &g_task, 0);
    return ESP_OK;
}

void gbac_enc_stop(void)
{
    g_run = false;
    for (int i = 0; i < 200 && g_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (g_enc) {
        esp_h264_enc_close(g_enc);
        esp_h264_enc_del(g_enc);
        g_enc = NULL;
        g_param = NULL;
    }
}

/* 次のエンコードフレームを IDR にする。mp4 のスライス切り替え時に呼ぶ。
 * GOP の設定は変わらない。 */
void gbac_enc_force_idr(void)
{
    if (g_enc == NULL || g_param == NULL) {
        return;
    }
    esp_h264_err_t e = esp_h264_enc_force_idr(&g_param->base);
    if (e != ESP_H264_ERR_OK) {
        ESP_LOGW(TAG, "force_idr 失敗(%d)", (int)e);
    }
}

int gbac_enc_running(void)
{
    return g_run ? 1 : 0;
}

/* まだ muxer に入れていない NAL の数。ベンチの終了判定に使う。 */
int gbac_enc_queued(void)
{
    return g_out_q ? (int)uxQueueMessagesWaiting(g_out_q) : 0;
}

/* 録画停止時にスケールの内訳を出す。gbac_scale_prof は gbac_enc_start で
 * ゼロにしているので、今回の録画の 1 枚あたりになる。
 * USB 受信と同時だとどこが膨らむのかを見る(2026-10-17 実測:
 * 実録画 85ms/枚、ベンチ 26ms/枚)。 */
void gbac_enc_prof_report(void)
{
    double n = g_frames > 0 ? (double)g_frames : 1.0;
    ESP_LOGI(TAG, "スケール内訳 us/枚 rgb=%.0f y=%.0f uv=%.0f copy=%.0f | 枚=%u scale<=%uus enc<=%uus",
             (double)gbac_scale_prof[0] / n, (double)gbac_scale_prof[1] / n,
             (double)gbac_scale_prof[2] / n, (double)gbac_scale_prof[3] / n,
             (unsigned)g_frames, (unsigned)g_scale_us_max, (unsigned)g_enc_us_max);
}

void gbac_enc_bench(const gbac_cfg_t *cfg, int bench_frames)
{
    for (int i = 0; i < 4; i++) gbac_scale_prof[i] = 0;
    int64_t t0 = esp_timer_get_time();
    if (gbac_enc_start(cfg, bench_frames) != ESP_OK) {
        return;
    }
    gbac_nal_t n;
    while (g_run || uxQueueMessagesWaiting(g_out_q) > 0) {
        if (gbac_enc_pop(&n, pdMS_TO_TICKS(50))) {
            gbac_enc_free(&n);
        }
    }
    gbac_enc_stop();
    int64_t us = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "エンコーダベンチ %d 枚 %.2f 秒 -> %.1f fps scale<=%uus enc<=%uus err=%u",
             bench_frames, (double)us / 1e6, (double)bench_frames * 1e6 / (double)us,
             (unsigned)g_scale_us_max, (unsigned)g_enc_us_max, (unsigned)g_errs);
}

int gbac_enc_pop(gbac_nal_t *out, TickType_t wait_ticks)
{
    return xQueueReceive(g_out_q, out, wait_ticks) == pdTRUE ? 1 : 0;
}

void gbac_enc_free(gbac_nal_t *n)
{
    for (int i = 0; i < NAL_SLOTS; i++) {
        if (g_pool[i] == n->buf) {
            xQueueSend(g_free_q, &i, 0);
            return;
        }
    }
}

uint32_t gbac_enc_frames(void)
{
    return g_frames;
}

uint32_t gbac_enc_errors(void)
{
    return g_errs;
}

uint32_t gbac_enc_scale_us_max(void)
{
    return g_scale_us_max;
}

uint32_t gbac_enc_enc_us_max(void)
{
    return g_enc_us_max;
}
