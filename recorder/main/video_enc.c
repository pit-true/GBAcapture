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

/* 拡大済みの YUV は 2 枚持つ(2026-10-17)。
 * esp_h264_enc_process は HW の frame_done ミューテックスでブロックする
 * (components/esp_h264/esp_h264_enc_single_hw.c:154)。つまりエンコード中に
 * このタスクは CPU を使わないので、拡大とエンコードを別タスクにできる。
 * 直列だと 拡大 26ms + エンコード 10ms = 36ms/枚だが、
 * 重ねれば max(拡大, エンコード) まで縮む。
 * 2 枚で足りるのは、拡大が次の枚を書き始める頃には前の枚がエンコード済み
 * になれるから。3 枚目はいらない(増やしても pipe の深さで決まる枚数が減らない)。 */
#define YUV_SLOTS 2

typedef struct {
    int      slot;      /* g_yuv[slot] */
    uint64_t pts_us;
} yuv_item_t;

static QueueHandle_t g_free_q;    /* 空いている NAL スロット index */
static QueueHandle_t g_out_q;     /* 完成した gbac_nal_t */
static QueueHandle_t g_yuv_free_q; /* 空いている YUV スロット index */
static QueueHandle_t g_yuv_q;      /* 拡大済み yuv_item_t */
static uint8_t *g_pool[NAL_SLOTS];
static uint8_t *g_yuv[YUV_SLOTS];
static uint8_t *g_encout;
static uint32_t g_yuv_len;
static uint32_t g_encout_len;
static esp_h264_enc_handle_t g_enc;
static esp_h264_enc_param_hw_handle_t g_param;
static TaskHandle_t g_task;        /* enc_task */
static TaskHandle_t g_scale_task;  /* scale_task */
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

/* ---- 前段: RGB565 -> OUYY 拡大 ---- */
static void scale_task(void *arg)
{
    (void)arg;
    uint32_t made = 0;

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
            pts_us = (uint64_t)made * 1000000ULL / 60;
        } else {
            px = frame_pipe_take(&pts_us, pdMS_TO_TICKS(100));
            if (px == NULL) {
                continue;
            }
        }

        int slot = -1;
        if (xQueueReceive(g_yuv_free_q, &slot, pdMS_TO_TICKS(200)) != pdTRUE) {
            g_errs++;
            ESP_LOGW(TAG, "YUV バッファが空かない(enc が遅い)");
            if (!g_bench) {
                frame_pipe_release();
            }
            continue;
        }

        int64_t t0 = esp_timer_get_time();
        gbac_scale_ouyy(px, GBAC_WIDTH, GBAC_HEIGHT, g_scale, g_yuv[slot]);
        int64_t t1 = esp_timer_get_time();
        if (!g_bench) {
            frame_pipe_release();
        }
        if ((uint32_t)(t1 - t0) > g_scale_us_max) {
            g_scale_us_max = (uint32_t)(t1 - t0);
        }

        yuv_item_t it = { .slot = slot, .pts_us = pts_us };
        if (!xQueueSend(g_yuv_q, &it, 0)) {
            g_errs++;
            xQueueSend(g_yuv_free_q, &slot, 0);
            continue;
        }
        made++;
    }

    ESP_LOGI(TAG, "スケール終了 枚=%u stack 残り=%uB",
             (unsigned)made, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    g_scale_task = NULL;
    vTaskDelete(NULL);
}

/* ---- 後段: H.264 HW エンコード ---- */
static void enc_task(void *arg)
{
    (void)arg;
    int done = 0;

    /* 前段が止まったあとも、並んでいる分はエンコードしきる。
     * そうしないと録画停止の直前 2 枚が mp4 から消える。 */
    while (g_run || uxQueueMessagesWaiting(g_yuv_q) > 0) {
        yuv_item_t it;
        if (xQueueReceive(g_yuv_q, &it, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        const int slot = it.slot;
        const uint64_t pts_us = it.pts_us;

        int nslot = -1;
        if (xQueueReceive(g_free_q, &nslot, pdMS_TO_TICKS(200)) != pdTRUE) {
            g_errs++;
            xQueueSend(g_yuv_free_q, &slot, 0);
            ESP_LOGW(TAG, "NAL スロットが空かない(mux が遅い)");
            continue;
        }

        esp_h264_enc_in_frame_t in = { 0 };
        in.raw_data.buffer = g_yuv[slot];
        in.raw_data.len = g_yuv_len;
        in.pts = (uint32_t)(pts_us / 1000);

        esp_h264_enc_out_frame_t out = { 0 };
        out.raw_data.buffer = g_encout;
        out.raw_data.len = g_encout_len;

        int64_t t1 = esp_timer_get_time();
        esp_h264_err_t e = esp_h264_enc_process(g_enc, &in, &out);
        int64_t t2 = esp_timer_get_time();
        if ((uint32_t)(t2 - t1) > g_enc_us_max) {
            g_enc_us_max = (uint32_t)(t2 - t1);
        }
        /* 入力の YUV はここで読み終わり。HW が cache の書き戻しまで済ませている
         * (esp_h264_enc_single_hw.c:231 で esp_h264_cache_check_and_writeback)。 */
        xQueueSend(g_yuv_free_q, &slot, 0);

        if (e != ESP_H264_ERR_OK) {
            g_errs++;
            xQueueSend(g_free_q, &nslot, 0);
            ESP_LOGW(TAG, "encode 失敗(%d)", (int)e);
            continue;
        }
        if ((int)out.length > NAL_SLOT_BYTES) {
            g_errs++;
            xQueueSend(g_free_q, &nslot, 0);
            ESP_LOGE(TAG, "NAL が %d バイトありスロットを超える", (int)out.length);
            continue;
        }

        memcpy(g_pool[nslot], out.raw_data.buffer, out.length);
        gbac_nal_t n = {
            .buf = g_pool[nslot],
            .len = (int)out.length,
            .pts_us = pts_us,
            .key = (out.frame_type == ESP_H264_FRAME_TYPE_IDR ||
                    out.frame_type == ESP_H264_FRAME_TYPE_I),
        };
        if (!xQueueSend(g_out_q, &n, 0)) {
            g_errs++;
            xQueueSend(g_free_q, &nslot, 0);
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

    if (g_yuv[0] == NULL) {
        /* esp_h264 はエンコード後に出力バッファを
         * esp_cache_msync(..., DIR_M2C) で無効化する。この方向は
         * UNALIGNED フラグを使えず、アドレスとサイズがキャッシュラインの
         * 倍数でないと ESP_ERR_INVALID_ARG を返して無効化が飛ぶ
         * (components/esp_mm/esp_cache_msync.c 120 行目)。
         * P4 の既定ラインは 64B だが Espressif の P4 テスト設定は 128B を
         * 選ぶので、どちらでも安全な 128 に揃える。
         * サイズ側は 960*640*3/2 = 921600 で 128 の倍数。 */
        for (int i = 0; i < YUV_SLOTS; i++) {
            g_yuv[i] = heap_caps_aligned_alloc(128, g_yuv_len, MALLOC_CAP_SPIRAM);
            if (g_yuv[i] == NULL) {
                ESP_LOGE(TAG, "PSRAM に %u バイト x%d が確保できない",
                         (unsigned)g_yuv_len, YUV_SLOTS);
                return ESP_ERR_NO_MEM;
            }
        }
        g_encout = heap_caps_aligned_alloc(128, g_encout_len, MALLOC_CAP_SPIRAM);
        if (g_encout == NULL) {
            ESP_LOGE(TAG, "PSRAM に %u バイトが確保できない", (unsigned)g_encout_len);
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
    if (g_yuv_free_q == NULL) {
        g_yuv_free_q = xQueueCreate(YUV_SLOTS, sizeof(int));
        g_yuv_q = xQueueCreate(YUV_SLOTS, sizeof(yuv_item_t));
    }
    xQueueReset(g_free_q);
    xQueueReset(g_out_q);
    xQueueReset(g_yuv_free_q);
    xQueueReset(g_yuv_q);
    for (int i = 0; i < NAL_SLOTS; i++) {
        int idx = i;
        xQueueSend(g_free_q, &idx, 0);
    }
    for (int i = 0; i < YUV_SLOTS; i++) {
        int idx = i;
        xQueueSend(g_yuv_free_q, &idx, 0);
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
    /* 前段(拡大)。gbac_scale_ouyy だけで約 4.2KB のスタックを使う。
     * 実機(2026-10-14)で 4096 バイトだと "Stack protection fault" で落ちたので
     * 12KB にした。終了時に high water mark を出すので実測はそれで分かる。
     *
     * コアは 0 のまま(2026-10-16 r9/r10 実測)。core1 に置くと prio5 が
     * rx(prio4)を starvation させ、USB rx リング(128 枠=9ms)が溢れて
     * r9 では 61,763 パケット(音声 6 秒分)が捨てられた。
     * core0 の上位(usb_lib=22, usbh=21)は prio5 より上なので USB 受信は邪魔されない。
     * 拡大が USB 受信に割り込まれて 85ms/枚になったのは、割り込みで止まる
     * 見かけ上の値なので、後段と重ねることで実効スループットを上げる。 */
    xTaskCreatePinnedToCore(scale_task, "scale", 12288, NULL, 5, &g_scale_task, 0);
    /* 後段(エンコード)。esp_h264_enc_process は HW 待ちでブロックするので
     * CPU はあまり使わない。core1 の rx(prio4)/mux(prio3)より下において、
     * 受信と SD 書き込みを邪魔しないようにする。 */
    xTaskCreatePinnedToCore(enc_task, "enc", 8192, NULL, 2, &g_task, 1);
    return ESP_OK;
}

void gbac_enc_stop(void)
{
    g_run = false;
    /* 前段は frame_pipe_take のタイムアウト(100ms)で一度ループを見て抜ける。
     * 後段は g_yuv_q を空にしてから抜ける。 */
    for (int i = 0; i < 200 && (g_task != NULL || g_scale_task != NULL); i++) {
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
 * 実録画 85ms/枚、ベンチ 26ms/枚)。
 * scale=4 の専用経路では rgb 欄が変換+luma+彩度、y 欄が internal RAM 内の複製、
 * uv 欄が PSRAM への書き込みで、copy 欄は 0 のまま。 */
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
