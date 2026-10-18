#include "video_enc.h"

#include <string.h>

#include "esp_async_memcpy.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
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
static TaskHandle_t g_scale_task;  /* scale_task(arg=0、上半分) */
static TaskHandle_t g_scale_task_b; /* scale_task(arg=1、下半分) */
static volatile bool g_run;
static bool g_bench;
static volatile int g_bench_left;
static uint32_t g_frames;
static uint32_t g_errs;
/* H264 は internal RAM に約 69KB の連続ブロック(reference frame)を要求する。
 * 動かしたあとでは internal の最大ブロックが 71KB まで減り、録画のたびに
 * 開けなくなった(2026-10-19 r99/r100 で "No memory for reference frame" -3)。
 * そこで起動直後の heap が荒れていないところで 1 回だけ開いて、
 * 録画ごとは close/del しない(リセットして使い回す)ようにした。
 * 実際に確保できた internal の一覧は gbac_enc_early_init で 1 回印字する。 */
static int g_open_scale;
static int g_open_bitrate;
/* enc タスクが今どこで止まっているか(録画中に enc が回っていないときの切り分け)。
 * 0=待機以外 1=g_yuv_q 待ち 2=NAL スロット待ち 3=HW エンコード内 4=out_q 待ち */
static volatile int g_enc_state;
static int g_fail_logged;
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

/* ---- PSRAM への書き出しを GDMA に逃がす(2026-10-18) ----
 * r91 実測: esp_async_memcpy(AXI-GDMA)で internal→PSRAM を 921,600B 送る時間は
 *   5,760B 刻み ×160 = 19.91ms(46MB/s)  ← 1 個あたりのオーバーヘッドが大きい
 *  11,520B 刻み ×80  = 10.40ms
 *  23,040B 刻み ×40  =  5.71ms(161MB/s) ← ここから頭打ち
 *  46,080B 刻み ×20  =  5.58ms
 * で、CPU memcpy(10.48ms)より 23,040B 刻みが速い。46,080B は internal RAM が
 * 46KB の塊を 2 個もたないので使えない。よって元行 4 本ぶん(23,040B)を
 * internal RAM のステージに組み、それを DMA で PSRAM へ送る。
 * ステージは n 個を循環。前回そのステージを使った DMA(= n-1 個前の発行)が
 * 終わるのを待ってから組み直す。発行の直前に「残り n-2 個以下」まで待てば、
 * 次の組み立て先(n-1 個前に使ったステージ)の DMA は必ず終わっている。
 * よって同時に飛ぶ DMA は n-1 個。2026-10-19 監督指示で 2 → 5 個(同時 4 個、
 * internal 23040B x5 = 113KB)。起動直後(enc+GDMA 後)の internal 309KB に入るので
 * 録画開始時の空きは減らない。
 * ドライバは source のキャッシュ writeback(C2M)と、dst のキャッシュライン
 * 境界外部分の退避を自分でやる(async_memcpy_gdma.c 374〜411 行)。
 * 2026-10-19 監督指示で 2 組に分割。上半分(元行 0〜79)を core0、下半分
 * (元行 80〜159)を core1 のタスクが作るので、段と DMA と待ちは組ごとに持つ。
 * 1 組 4 個(90KB)x2 = 180KB。internal の空きと相談して足りない組は CPU で書く。 */
#define STG_BYTES_X4 (GBAC_OUYY_STRIDE(GBAC_WIDTH, 4) * 4 * 4) /* 23040 */
#define STG_BYTES_X2 (GBAC_OUYY_STRIDE(GBAC_WIDTH, 2) * 2 * 4) /* 5760 */
/* 段の大きさは設定の scale に合わせる(2026-10-19 の仕様変更で既定は 2)。
 * scale が奇数(専用経路なし)なら段を作らず汎用経路で CPU が書く。 */
static size_t g_stg_bytes;
#define STG_N 4
#define SCALE_SETS 2
#define SCALE_HALF (GBAC_HEIGHT / 2)   /* 80。4 の倍数なので 4 元行組で割れる */
/* 2026-10-19: 録画が 480x320 になり、1 枚ぶんの仕事は 1/4。
 * 2 コア分割はここで止める(コードは残す)。既定は core0 の scale 1 本。 */
#define SPLIT_ENABLE 0

typedef struct {
    uint8_t *stg[STG_N];
    int n;
    async_memcpy_handle_t mcp;
    SemaphoreHandle_t sem;
    volatile int issue;
    volatile int done;
    int row0, row1;
    gbac_scale_ctx_t ctx;
    int64_t prof[4];
    uint32_t us_max;
} scale_set_t;
static scale_set_t g_ss[SCALE_SETS];
static int g_split;               /* 2 組そろって動かせるか */
/* 完了コールバックが載るコア。ISR でログは出せないので変数だけ置いて
 * 後でタスク側から印字する(GDMA の ISR が core1 なら mux/enc を踏む)。 */
static volatile int g_dma_isr_core = -99;

/* job の受け渡し(2026-10-19 監督指示の分割)。
 * scale_a が枠を配って g_job_sem を与え、scale_b は自分の分を終わらせて
 * g_join_sem を返す。scale_a は join を取るまで次の枠を作らないので、
 * 同時に走るのは 1 枠ぶんだけ。 */
static SemaphoreHandle_t g_job_sem;
static SemaphoreHandle_t g_join_sem;
static const uint16_t *g_job_px;
static uint8_t *g_job_dst;

static bool dma_done_cb(async_memcpy_handle_t h, async_memcpy_event_t *e, void *arg)
{
    (void)h;
    (void)e;
    scale_set_t *s = (scale_set_t *)arg;
    g_dma_isr_core = (int)xPortGetCoreID();
    s->done++;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s->sem, &hp);
    return hp == pdTRUE;
}

/* gbac_scale が 4 元行ぶんを組み終えるたびに呼ばれる(flush は組ごとに差し替え) */
static void dma_flush_set(scale_set_t *s, uint8_t *dst, const uint8_t *stage, size_t n)
{
    while (s->issue - s->done >= s->n - 2) {
        if (xSemaphoreTake(s->sem, pdMS_TO_TICKS(200)) != pdTRUE) {
            g_errs++;
            ESP_LOGW(TAG, "DMA 完了待ちが 200ms 超過");
            break;
        }
    }
    if (s->mcp == NULL || esp_async_memcpy(s->mcp, dst, (void *)stage, n, dma_done_cb, s) != ESP_OK) {
        memcpy(dst, stage, n); /* 発行に失敗したらその場で CPU が書く */
        g_errs++;
    } else {
        s->issue++;
    }
}

/* 1 枚ぶん全部が PSRAM に乗るまで待つ(enc が読み始める前に必要) */
static void dma_finish_set(scale_set_t *s)
{
    static bool isr_core_logged;
    if (!isr_core_logged && g_dma_isr_core >= 0) {
        isr_core_logged = true;
        ESP_LOGI(TAG, "DMA 完了 ISR は core%d で動いている", g_dma_isr_core);
    }
    while (s->done < s->issue) {
        if (xSemaphoreTake(s->sem, pdMS_TO_TICKS(200)) != pdTRUE) {
            g_errs++;
            ESP_LOGE(TAG, "DMA 完了待ちが 200ms 超過。枚を捨てる");
            break;
        }
    }
}

static void dma_flush_a(uint8_t *d, const uint8_t *stg, size_t n) { dma_flush_set(&g_ss[0], d, stg, n); }
static void dma_flush_b(uint8_t *d, const uint8_t *stg, size_t n) { dma_flush_set(&g_ss[1], d, stg, n); }
static void dma_finish_a(void) { dma_finish_set(&g_ss[0]); }
static void dma_finish_b(void) { dma_finish_set(&g_ss[1]); }

/* 内訳カウンタは組ごとに別。既定の作業台(分割しない経路・ベンチ)と足し合わせる */
static int64_t prof_sum(int i)
{
    int64_t v = gbac_scale_prof[i];
    for (int k = 0; k < SCALE_SETS; k++) {
        v += g_ss[k].prof[i];
    }
    return v;
}

static void prof_zero_all(void)
{
    for (int i = 0; i < 4; i++) {
        gbac_scale_prof[i] = 0;
        for (int k = 0; k < SCALE_SETS; k++) {
            g_ss[k].prof[i] = 0;
        }
    }
    for (int k = 0; k < SCALE_SETS; k++) {
        g_ss[k].us_max = 0;
    }
}

static void dma_stage_setup(int scale)
{
    static bool done_once;
    if (done_once) {
        return;
    }
    done_once = true;
    g_stg_bytes = (scale == 4) ? STG_BYTES_X4 : (scale == 2 ? STG_BYTES_X2 : 0);
    if (g_stg_bytes == 0) {
        /* 1 や 3 など専用経路のない scale。汎用経路が PSRAM へ直接書く */
        ESP_LOGI(TAG, "scale=%d は専用経路なし。段は作らない", scale);
        return;
    }
    for (int k = 0; k < (SPLIT_ENABLE ? SCALE_SETS : 1); k++) {
        scale_set_t *s = &g_ss[k];
        s->row0 = (k == 0) ? 0 : SCALE_HALF;
        s->row1 = (k == 0) ? SCALE_HALF : GBAC_HEIGHT;
        for (int i = 0; i < STG_N; i++) {
            s->stg[i] = heap_caps_aligned_alloc(64, g_stg_bytes,
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            if (s->stg[i] == NULL) {
                break;
            }
            s->n = i + 1;
        }
        if (s->n < 2) {
            for (int i = 0; i < STG_N; i++) {
                if (s->stg[i] != NULL) {
                    heap_caps_free(s->stg[i]);
                    s->stg[i] = NULL;
                }
            }
            s->n = 0;
            continue;
        }
        s->sem = xSemaphoreCreateCounting(16, 0);
        async_memcpy_config_t ac = ASYNC_MEMCPY_DEFAULT_CONFIG();
        ac.backlog = 8;
        ac.dma_burst_size = 128;
        esp_err_t e = esp_async_memcpy_install_gdma_axi(&ac, &s->mcp);
        if (e != ESP_OK) {
            e = esp_async_memcpy_install_gdma_ahb(&ac, &s->mcp);
        }
        if (e != ESP_OK) {
            s->mcp = NULL;   /* DMA が飛べなければ CPU がその場で書く */
        }
        s->issue = s->done = 0;
        s->ctx.prof = s->prof;   /* ctx_set より先に置く(ctx_set は NULL のときだけ埋める) */
        gbac_scale_ctx_set(&s->ctx, s->stg, s->n, g_stg_bytes,
                           (k == 0) ? dma_flush_a : dma_flush_b,
                           (k == 0) ? dma_finish_a : dma_finish_b);
    }
    g_split = SPLIT_ENABLE && (g_ss[0].n >= 2 && g_ss[1].n >= 2);
    if (!g_split) {
        /* 分割しないので組 0 だけで全枠を作る(既定の作業台に同じ段を渡す) */
        if (g_ss[1].n >= 2) {
            for (int i = 0; i < STG_N; i++) {
                if (g_ss[1].stg[i] != NULL) {
                    heap_caps_free(g_ss[1].stg[i]);
                    g_ss[1].stg[i] = NULL;
                }
            }
            g_ss[1].n = 0;
        }
        gbac_scale_set_stage(g_ss[0].stg, g_ss[0].n, g_stg_bytes, dma_flush_a, dma_finish_a);
    }
    g_job_sem = xSemaphoreCreateBinary();
    g_join_sem = xSemaphoreCreateBinary();
    ESP_LOGI(TAG, "PSRAM 書き出しは GDMA(分割 %s、段 %dB x%d x%d 組、同時 %d、internal free %uKB)",
             g_split ? "上下 2 コア" : "なし", (int)g_stg_bytes, g_ss[0].n,
             SPLIT_ENABLE ? SCALE_SETS : 1,
             g_ss[0].n - 1,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

/* DMA で PSRAM へ書いた枚と、CPU が直接書いた枚がバイト単位で同じかを確かめる。
 * ベンチの 1 枚目だけ。違うときは拡大の組み替えか DMA のキャッシュ面倒が
 * 壊れている証拠なので、映像を見る前に分かるようにする。 */
static void dma_selfcheck(uint8_t *slot, const uint16_t *px)
{
    static uint8_t *ref;
    if (ref == NULL) {
        ref = heap_caps_aligned_alloc(128, g_yuv_len, MALLOC_CAP_SPIRAM);
        if (ref == NULL) {
            ESP_LOGW(TAG, "DMA 一致確認用の %u バイトが確保できない", (unsigned)g_yuv_len);
            return;
        }
    }
    gbac_scale_set_stage(NULL, 0, 0, NULL, NULL);   /* CPU が直接書く経路 */
    gbac_scale_ouyy(px, GBAC_WIDTH, GBAC_HEIGHT, g_scale, ref);
    gbac_scale_set_stage(g_ss[0].stg, g_ss[0].n, g_stg_bytes, dma_flush_a, dma_finish_a);
    /* slot は CPU が一度も読んでいないのでキャッシュに無いはずだが、
     * 念のため M2C で無効化してから読む(アドレス・サイズは 64B の倍数) */
    esp_cache_msync(slot, g_yuv_len, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    size_t bad = 0, first = 0;
    for (size_t i = 0; i < g_yuv_len; i++) {
        if (slot[i] != ref[i]) {
            if (bad == 0) first = i;
            bad++;
        }
    }
    ESP_LOGI(TAG, "DMA 一致確認: %u/%u バイト不一致(最初の不一致=%u slot=%p ref=%p)",
             (unsigned)bad, (unsigned)g_yuv_len, (unsigned)first,
             (void *)slot, (void *)ref);
    prof_zero_all();   /* 確認ぶんを内訳から除外する */
}

/* ---- 前段: RGB565 -> OUYY 拡大 ----
 * scale_a(core0)が枠を受け取り、上半分(元行 0〜79)を作る。
 * scale_b(core1)は g_job_sem を受け取って下半分(元行 80〜159)を作る。
 * 両方がそろってから scale_a が枠を enc へ渡す(2026-10-19 監督指示)。 */
static void scale_task(void *arg)
{
    const int k = (int)(intptr_t)arg;
    scale_set_t *s = &g_ss[k];
    uint32_t made = 0;

    if (k == 1) {
        /* 下半分。枠の受け取りから enc へ渡すまで(=解放)は scale_a がやる */
        while (g_run) {
            if (xSemaphoreTake(g_job_sem, pdMS_TO_TICKS(300)) != pdTRUE) {
                continue;
            }
            if (!g_run) {
                break;
            }
            int64_t t0 = esp_timer_get_time();
            gbac_scale_ouyy_rows(&s->ctx, g_job_px, GBAC_WIDTH, GBAC_HEIGHT,
                                 g_scale, g_job_dst, s->row0, s->row1);
            uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
            if (dt > s->us_max) {
                s->us_max = dt;
            }
            xSemaphoreGive(g_join_sem);
            made++;
        }
        ESP_LOGI(TAG, "スケール B 終了 枚=%u stack 残り=%uB",
                 (unsigned)made, (unsigned)uxTaskGetStackHighWaterMark(NULL));
        g_scale_task_b = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }

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
        if (g_split) {
            g_job_px = px;
            g_job_dst = g_yuv[slot];
            xSemaphoreGive(g_job_sem);
            gbac_scale_ouyy_rows(&s->ctx, px, GBAC_WIDTH, GBAC_HEIGHT, g_scale,
                                 g_yuv[slot], s->row0, s->row1);
        } else {
            gbac_scale_ouyy(px, GBAC_WIDTH, GBAC_HEIGHT, g_scale, g_yuv[slot]);
        }
        int64_t tA = esp_timer_get_time();
        if (g_split && xSemaphoreTake(g_join_sem, pdMS_TO_TICKS(400)) != pdTRUE) {
            g_errs++;
            ESP_LOGW(TAG, "scale_b が 400ms 以内に終わらない");
        }
        int64_t t1 = esp_timer_get_time();
        if (!g_bench) {
            frame_pipe_release();
        }
        if (g_bench && g_ss[0].mcp != NULL && (g_scale == 4 || g_scale == 2) && made == 0) {
            dma_selfcheck(g_yuv[slot], px);
        }
        if ((uint32_t)(tA - t0) > s->us_max) {
            s->us_max = (uint32_t)(tA - t0);
        }
        if ((uint32_t)(t1 - t0) > g_scale_us_max) {
            g_scale_us_max = (uint32_t)(t1 - t0);
        }

        yuv_item_t it = { .slot = slot, .pts_us = pts_us };
        if (!xQueueSend(g_yuv_q, &it, 0)) {
            g_errs++;
            static int qf_logged;
            if (qf_logged < 5) {
                qf_logged++;
                ESP_LOGW(TAG, "g_yuv_q が満杯 %d/5(enc が取りに来ていない) enc_state=%d yuv_q=%u",
                         qf_logged, (int)g_enc_state,
                         (unsigned)uxQueueMessagesWaiting(g_yuv_q));
            }
            xQueueSend(g_yuv_free_q, &slot, 0);
            continue;
        }
        made++;
    }

    ESP_LOGI(TAG, "スケール終了 枚=%u stack 残り=%uB",
             (unsigned)made, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    g_scale_task = NULL;
    /* スタックは PSRAM なので WithCaps 用の削除を使う */
    vTaskDeleteWithCaps(NULL);
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
        g_enc_state = 1;
        if (xQueueReceive(g_yuv_q, &it, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        const int slot = it.slot;
        const uint64_t pts_us = it.pts_us;

        int nslot = -1;
        g_enc_state = 2;
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
        g_enc_state = 3;
        esp_h264_err_t e = esp_h264_enc_process(g_enc, &in, &out);
        int64_t t2 = esp_timer_get_time();
        g_enc_state = 0;
        if ((uint32_t)(t2 - t1) > g_enc_us_max) {
            g_enc_us_max = (uint32_t)(t2 - t1);
        }
        if (t2 - t1 > 300000) {
            ESP_LOGW(TAG, "encode が %lldms かかった(ret=%d)",
                     (long long)((t2 - t1) / 1000), (int)e);
        }
        /* 入力の YUV はここで読み終わり。HW が cache の書き戻しまで済ませている
         * (esp_h264_enc_single_hw.c:231 で esp_h264_cache_check_and_writeback)。 */
        xQueueSend(g_yuv_free_q, &slot, 0);

        if (e != ESP_H264_ERR_OK) {
            g_errs++;
            xQueueSend(g_free_q, &nslot, 0);
            if (g_fail_logged < 5) {
                g_fail_logged++;
                ESP_LOGW(TAG, "encode 失敗 %d/5 ret=%d slot=%d in=%u out=%u internal free=%uKB 最大=%uKB",
                         g_fail_logged, (int)e, slot,
                         (unsigned)in.raw_data.len, (unsigned)out.length,
                         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                         (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
            }
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
        g_enc_state = 4;
        if (!xQueueSend(g_out_q, &n, 0)) {
            g_errs++;
            xQueueSend(g_free_q, &nslot, 0);
            continue;
        }
        g_enc_state = 0;
        g_frames++;
        done++;
    }

    ESP_LOGI(TAG, "エンコード終了 frames=%u errs=%u scale_max=%uus enc_max=%uus enc_state=%d stack 残り=%uB",
             (unsigned)g_frames, (unsigned)g_errs,
             (unsigned)g_scale_us_max, (unsigned)g_enc_us_max, (int)g_enc_state,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (g_bench) {
        const int n = done > 0 ? (int)done : 1;
        ESP_LOGI(TAG, "スケール内訳 us/枚 rgb=%.0f y=%.0f uv=%.0f copy=%.0f | a<=%uus b<=%uus",
                 (double)prof_sum(0) / n, (double)prof_sum(1) / n,
                 (double)prof_sum(2) / n, (double)prof_sum(3) / n,
                 (unsigned)g_ss[0].us_max, (unsigned)g_ss[1].us_max);
    }
    g_task = NULL;
    /* enc のスタックは PSRAM なので WithCaps 用の削除を使う */
    vTaskDeleteWithCaps(NULL);
}

/* PSRAM 側の部屋とキューを作る。録画のたびに作り直さない(断片化避け)。 */
static esp_err_t pool_alloc(void)
{
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
        if (g_free_q == NULL || g_out_q == NULL || g_yuv_free_q == NULL || g_yuv_q == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

/* H.264 HW エンコーダを開く。internal RAM に約 69KB の連続ブロックが要る。 */
static esp_err_t enc_open(const gbac_cfg_t *cfg)
{
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
        ESP_LOGE(TAG, "enc_hw_new 失敗(%d) %dx%d internal free=%uKB 最大=%uKB",
                 (int)e, g_W, g_H,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
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
    g_open_scale = cfg->scale;
    g_open_bitrate = cfg->bitrate_kbps;
    ESP_LOGI(TAG, "エンコーダを開いた %dx%d %dkbps internal free=%uKB 最大=%uKB",
             g_W, g_H, (int)cfg->bitrate_kbps,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    return ESP_OK;
}

static void enc_close(void)
{
    if (g_enc != NULL) {
        esp_h264_enc_close(g_enc);
        esp_h264_enc_del(g_enc);
        g_enc = NULL;
        g_param = NULL;
    }
}

/* 起動直後の一番最初に 1 回だけ呼ぶ。internal RAM の大口
 * (H264 の約 69KB と GDMA のステージ 23,040B x2)を heap が荒れる前に
 * たしかめておく。録画のたびに開くと断片化して取れない(2026-10-19 r99/r100)。 */
esp_err_t gbac_enc_early_init(const gbac_cfg_t *cfg)
{
    g_scale = cfg->scale;
    g_W = GBAC_WIDTH * g_scale;
    g_H = GBAC_HEIGHT * g_scale;
    g_yuv_len = GBAC_OUYY_SIZE(GBAC_WIDTH, GBAC_HEIGHT, g_scale);
    g_encout_len = g_yuv_len;
    /* RGB565 → (Y,U,V) の 256KB 表を PSRAM に作る(2026-10-19 監督指示)。
     * 拡大の 1 画素ぶんの計算を表引き 1 回にする。録画前に済ませておく。 */
    if (gbac_scale_init_lut() != 0) {
        ESP_LOGE(TAG, "色の変換表が作れない。拡大は計算経路で動く");
    }
    esp_err_t r = pool_alloc();
    if (r != ESP_OK) {
        return r;
    }
    /* 一番大きな連続ブロックを要求する方を先に確保させる */
    r = enc_open(cfg);
    if (r != ESP_OK) {
        return r;
    }
    dma_stage_setup(cfg->scale);
    ESP_LOGI(TAG, "internal free=%uKB 最大=%uKB DMA 可=%uKB PSRAM free=%uKB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    /* internal RAM を何がどれだけ食っているかの表(1 回だけ) */
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    return ESP_OK;
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
    /* 起動時に開いてあるので、普通はここを通らない。
     * 開けなかったか、scale かビットレートが変わったときだけ作り直す。 */
    if (g_enc == NULL || g_open_scale != g_scale || g_open_bitrate != cfg->bitrate_kbps) {
        if (g_enc != NULL) {
            enc_close();
        }
        if (pool_alloc() != ESP_OK) {
            return ESP_ERR_NO_MEM;
        }
        if (enc_open(cfg) != ESP_OK) {
            return ESP_FAIL;
        }
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

    g_frames = 0;
    g_errs = 0;
    g_scale_us_max = 0;
    g_enc_us_max = 0;
    g_fail_logged = 0;
    prof_zero_all();
    /* 前段の PSRAM 書き出しを DMA に切り替える(起動時に済んでいる。
     * 失敗していても従来経路(CPU が直接書く)で動く) */
    dma_stage_setup(cfg->scale);
    for (int k = 0; k < SCALE_SETS; k++) {
        g_ss[k].issue = g_ss[k].done = 0;
    }
    /* 前の録画で取り残した job の旗を下ろしておく(2026-10-19 の分割) */
    if (g_job_sem != NULL) {
        while (xSemaphoreTake(g_job_sem, 0) == pdTRUE) { }
    }
    if (g_join_sem != NULL) {
        while (xSemaphoreTake(g_join_sem, 0) == pdTRUE) { }
    }
    /* エンコーダは開きっぱなしなので、この録画の 1 枚目を IDR にする。
     * IDR の NAL には SPS+PPS が付くので、muxer はそこから mp4 を始められる。 */
    gbac_enc_force_idr();
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
     * 2026-10-19 r103 は core0 に scale(13.2ms/枚)+enc+aac(約 47%)を載せたため
     * core0 が 100% になり、IDLE0 が餓死して task_wdt が 10 秒ごとに出た。
     * main(prio1)も回らず 20 秒の自動停止も統計も出なかった(MEPC=build_row_x4)。
     * 2026-10-19 監督指示の配分: core0 = scale の上半分(+USB)と scale_b 待ち、
     * core1 = enc(HW 待ちが大半)・aac・mux・rx。
     *
     * スタックは PSRAM(計算だけのタスクなので SD/NVS は呼ばない)。
     * エンコーダと GDMA のステージを起動時に常時確保したため、internal の
     * 空きが少なくなっている(2026-10-19 r101)。TCB は internal のまま。 */
    xTaskCreatePinnedToCoreWithCaps(scale_task, "scale_a", 12288, (void *)(intptr_t)0,
                                    5, &g_scale_task, 0, MALLOC_CAP_SPIRAM);
    if (g_split) {
        xTaskCreatePinnedToCoreWithCaps(scale_task, "scale_b", 12288, (void *)(intptr_t)1,
                                        5, &g_scale_task_b, 1, MALLOC_CAP_SPIRAM);
    } else {
        g_scale_task_b = NULL;
    }
    /* 後段(エンコード)。esp_h264_enc_process は HW 待ちでブロックするので
     * CPU は常に取り合わない。core1 では rx(4)の下、mux(2)の上の prio3。
     * 2026-10-19 r98 は core1 prio2 で mux(prio3)に餓死され 3 枚しか進まなかった
     * ので、mux より上に置く。
     *
     * スタックは PSRAM(xTaskCreatePinnedToCoreWithCaps)。エンコーダと GDMA の
     * ステージを起動時に常時確保したので、internal に 8KB のスタックを置くと
     * 録画のときに internal が枯渇する(2026-10-19 r101: 録画開始時点で
     * internal free 45KB・最大 21KB しかなく、mux が詰まって停止時に
     * CPU0 の interrupt WDT で panic した)。TCB は internal のまま。
     * 自己終了は vTaskDeleteWithCaps(NULL) が必須。 */
    xTaskCreatePinnedToCoreWithCaps(enc_task, "enc", 8192, NULL, 3, &g_task, 1,
                                    MALLOC_CAP_SPIRAM);
    /* 録画中に enc が回っていないときの切り分け用。コアと優先度の実値と、
     * この時点の internal RAM を 1 回だけ出す(2026-10-19 r98 で
     * aac が 6 倍遅くなり ringovf=10662 になった原因を見る)。 */
    ESP_LOGI(TAG, "タスク表 core/prio: usb_lib 0/22 usbh 0/21 rx 1/4 mux 1/2 aac 1/1 "
                  "scale_a %d/%d scale_b %d/%d enc %d/%d (このログを出したタスクは core%d)",
             0, g_scale_task ? (int)uxTaskPriorityGet(g_scale_task) : -1,
             1, g_scale_task_b ? (int)uxTaskPriorityGet(g_scale_task_b) : -1,
             1, (int)uxTaskPriorityGet(g_task), (int)xPortGetCoreID());
    ESP_LOGI(TAG, "internal free=%uKB 最大ブロック=%uKB DMA 可=%uKB PSRAM free=%uKB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return ESP_OK;
}

void gbac_enc_stop(void)
{
    g_run = false;
    /* 前段は frame_pipe_take のタイムアウト(100ms)で一度ループを見て抜ける。
     * 後段は g_yuv_q を空にしてから抜ける。 */
    for (int i = 0; i < 300 && (g_task != NULL || g_scale_task != NULL ||
                                g_scale_task_b != NULL); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    /* エンコーダは開きっぱなしにする。close/del すると internal の塊が
     * 断片化して、後で reference frame が取れなくなる。
     * 次回の gbac_enc_start で force_idr するので、mp4 の先頭は IDR のまま。 */
    for (int k = 0; k < SCALE_SETS; k++) {
        g_ss[k].issue = g_ss[k].done = 0;
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
    ESP_LOGI(TAG, "スケール内訳 us/枚 rgb=%.0f y=%.0f uv=%.0f copy=%.0f | 枚=%u a<=%uus b<=%uus wall<=%uus enc<=%uus",
             (double)prof_sum(0) / n, (double)prof_sum(1) / n,
             (double)prof_sum(2) / n, (double)prof_sum(3) / n,
             (unsigned)g_frames, (unsigned)g_ss[0].us_max, (unsigned)g_ss[1].us_max,
             (unsigned)g_scale_us_max, (unsigned)g_enc_us_max);
}

void gbac_enc_bench(const gbac_cfg_t *cfg, int bench_frames)
{
    prof_zero_all();
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
