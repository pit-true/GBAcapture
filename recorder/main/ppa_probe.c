/* ppa_probe.c — 拡大前段の高速化調査(段階 A)。録画経路は触らない。コマンド 'k' 専用。
 *
 * r89 で確定したこと
 *   - PPA SRM の YUV420 出力の並びは O_UYY_E_VYY(符号器と同じ)で合格
 *   - PPA scale4 = 6.92ms/枚、scale1(色変換のみ) = 1.34ms/枚
 *   - ただし PPA の 4 倍拡大は補間になる。白 1 画素の縦線が
 *     出力で 9b da da 9b となり左右へ 1f 5d までにじむ(元画素 7,8,9 列で
 *     4x4 ブロック内が不均一)。ピクセルパーフェクトの仕様を満たせない
 *   - 色は full/BT.601 が最も近い(Y -3..0、U 0..+1、V -5..0)
 * → PPA で 4 倍は作れない。PSRAM への書き込みだけ DMA(GDMA)に逃がす。
 *   ここでは esp_async_memcpy(AXI-GDMA)で internal→PSRAM の複製が
 *   何 ms/枚か、CPU に何 ms 負荷がかかるかを見る。 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_async_memcpy.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "gbacap.h"
#include "ppa_probe.h"

static const char *TAG = "ppaprobe";

#define PP_W   GBAC_WIDTH    /* 240 */
#define PP_H   GBAC_HEIGHT   /* 160 */
#define PP_SC  4
#define PP_OW  (PP_W * PP_SC)  /* 960 */
#define PP_OH  (PP_H * PP_SC)  /* 640 */
#define PP_LEN GBAC_OUYY_SIZE(PP_W, PP_H, PP_SC)  /* 921600 */
#define PP_STRIDE GBAC_OUYY_STRIDE(PP_W, PP_SC)   /* 1440 */
#define PP_BLOCK (4 * PP_STRIDE)                  /* 5760: 元行 1 本ぶん */

static ppa_client_handle_t s_cl;
static uint16_t *s_src;      /* RGB565 元画像 */
static uint8_t *s_dst;       /* 出力先(PSRAM) */
static uint8_t *s_blk;       /* 複製ブロック(internal RAM) */
static async_memcpy_handle_t s_mcp;
static SemaphoreHandle_t s_done;
static volatile int s_ndone;

static void fill_grad(void)
{
    for (int y = 0; y < PP_H; y++) {
        for (int x = 0; x < PP_W; x++) {
            int r = (x * 255) / (PP_W - 1);
            int g = (y * 255) / (PP_H - 1);
            int b = ((x + y) * 255) / (PP_W + PP_H - 2);
            if (((x >> 4) + (y >> 4)) & 1) {
                r = 255 - r;
                g = 255 - g;
                b = 255 - b;
            }
            s_src[y * PP_W + x] =
                (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

static bool done_cb(async_memcpy_handle_t h, async_memcpy_event_t *e, void *arg)
{
    (void)h;
    (void)e;
    s_ndone++;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, NULL);
    return false;
}

/* nblk 個の blk_size バイトコピーを mcp で順に発行し、全部終わるまで待つ。
 * 所要(実時間)と、発行にかかった CPU 時間を出す */
static void dma_case(const char *tag, int nblk, int blk, uint8_t *dst)
{
    if ((size_t)nblk * blk > PP_LEN) return;
    if (blk > PP_BLOCK) return;
    s_ndone = 0;
    int64_t t0 = esp_timer_get_time();
    int64_t cpu0 = t0;
    int issued = 0, err = 0;
    for (int i = 0; i < nblk; i++) {
        while (issued - s_ndone >= 12) {
            if (xSemaphoreTake(s_done, pdMS_TO_TICKS(200)) != pdTRUE) break;
        }
        esp_err_t e = esp_async_memcpy(s_mcp, dst + (size_t)i * blk, s_blk, blk,
                                       done_cb, s_done);
        if (e != ESP_OK) {
            err = (int)e;
            break;
        }
        issued++;
    }
    int64_t t1 = esp_timer_get_time();   /* 発行だけ終わった時点 */
    while (s_ndone < issued) {
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(500)) != pdTRUE) break;
    }
    int64_t t2 = esp_timer_get_time();
    ESP_LOGI(TAG, "GDMA %s: %d 個 × %dB 実時間 %.2f ms 発行 CPU %.2f ms 完了 %d err=%d",
             tag, issued, blk, (double)(t2 - t0) / 1000.0,
             (double)(t1 - cpu0) / 1000.0, s_ndone, err);
}

void gbac_ppa_probe(void)
{
    if (s_src == NULL) {
        s_src = heap_caps_aligned_alloc(128, PP_W * PP_H * 2, MALLOC_CAP_SPIRAM);
        s_dst = heap_caps_aligned_alloc(128, PP_LEN, MALLOC_CAP_SPIRAM);
        s_blk = heap_caps_aligned_alloc(64, PP_BLOCK,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!s_src || !s_dst || !s_blk) {
            ESP_LOGE(TAG, "alloc fail src=%p dst=%p blk=%p",
                     (void *)s_src, (void *)s_dst, (void *)s_blk);
            return;
        }
        ESP_LOGI(TAG, "internal free=%uKB largest=%uKB",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
        async_memcpy_config_t ac = ASYNC_MEMCPY_DEFAULT_CONFIG();
        ac.backlog = 16;
        ac.dma_burst_size = 128;
        esp_err_t e = esp_async_memcpy_install_gdma_axi(&ac, &s_mcp);
        ESP_LOGI(TAG, "install gdma_axi: %s", esp_err_to_name(e));
        if (e != ESP_OK) {
            e = esp_async_memcpy_install_gdma_ahb(&ac, &s_mcp);
            ESP_LOGI(TAG, "install gdma_ahb: %s", esp_err_to_name(e));
        }
        if (s_mcp == NULL) return;
        s_done = xSemaphoreCreateCounting(64, 0);
    }
    fill_grad();
    memset(s_blk, 0xA5, PP_BLOCK);

    /* 1 枚ぶんの複製(160 個 × 5760B)を GDMA で */
    dma_case("160x5760", PP_H, PP_BLOCK, s_dst);
    /* CPU memcpy と比較 */
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < PP_H; i++) memcpy(s_dst + (size_t)i * PP_BLOCK, s_blk, PP_BLOCK);
    ESP_LOGI(TAG, "CPU memcpy 160x5760: %.2f ms", (double)(esp_timer_get_time() - t0) / 1000.0);
    /* 続けて 3 枚(実時間を見る) */
    for (int f = 0; f < 3; f++) dma_case("160x5760", PP_H, PP_BLOCK, s_dst);

    /* 大きな塊にできるか(internal RAM がもつか) */
    for (int mult = 2; mult <= 8; mult *= 2) {
        size_t n = (size_t)PP_BLOCK * mult;
        uint8_t *big = heap_caps_aligned_alloc(64, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (big == NULL) {
            ESP_LOGI(TAG, "internal %uB 確保できず", (unsigned)n);
            break;
        }
        memset(big, 0x5A, n);
        int nblk = PP_LEN / (int)n;
        s_ndone = 0;
        int64_t u0 = esp_timer_get_time();
        int issued = 0, err = 0;
        for (int i = 0; i < nblk; i++) {
            while (issued - s_ndone >= 6) {
                if (xSemaphoreTake(s_done, pdMS_TO_TICKS(200)) != pdTRUE) break;
            }
            esp_err_t e = esp_async_memcpy(s_mcp, s_dst + (size_t)i * n, big, n, done_cb, s_done);
            if (e != ESP_OK) { err = (int)e; break; }
            issued++;
        }
        int64_t u1 = esp_timer_get_time();
        while (s_ndone < issued) {
            if (xSemaphoreTake(s_done, pdMS_TO_TICKS(500)) != pdTRUE) break;
        }
        ESP_LOGI(TAG, "GDMA %dx%uB: 実時間 %.2f ms 発行 CPU %.2f ms err=%d",
                 issued, (unsigned)n, (double)(esp_timer_get_time() - u0) / 1000.0,
                 (double)(u1 - u0) / 1000.0, err);
        heap_caps_free(big);
    }
    ESP_LOGI(TAG, "internal free=%uKB largest=%uKB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}
