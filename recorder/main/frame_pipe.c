#include "frame_pipe.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "gbacap.h"

static const char *TAG = "frame";

#define NSLOT 2

static gbac_asm_t *g_asm[NSLOT];
static uint64_t g_slot_pts[NSLOT];
static int g_cur;            /* USB タスクが埋めている slot */
static int g_ready = -1;     /* 読み待ちの slot(-1 = なし) */
static int g_held = -1;      /* 読み手が持っている slot */
static SemaphoreHandle_t g_sig;
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static gbac_pts_t g_pts;

esp_err_t frame_pipe_init(void)
{
    for (int i = 0; i < NSLOT; i++) {
        /* 1 枚 76.8KB。PSRAM に逃がすと DMA/キャッシュ越しで遅くなるので
         * 内部 RAM に確保する(2 枚で 154KB)。 */
        g_asm[i] = heap_caps_malloc(sizeof(gbac_asm_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (g_asm[i] == NULL) {
            ESP_LOGE(TAG, "フレームバッファが確保できない");
            return ESP_ERR_NO_MEM;
        }
        gbac_asm_init(g_asm[i]);
    }
    g_sig = xSemaphoreCreateBinary();
    gbac_pts_init(&g_pts);
    g_cur = 0;
    g_ready = -1;
    g_held = -1;
    return ESP_OK;
}

void frame_pipe_push(const uint8_t *pkt, int len)
{
    gbac_line_t ln;
    if (gbac_line_parse(pkt, len, &ln) != 0) {
        return;
    }

    int cur;
    portENTER_CRITICAL(&g_mux);
    cur = g_cur;
    portEXIT_CRITICAL(&g_mux);

    int r = gbac_asm_push(g_asm[cur], &ln);
    if (r < 0) {
        return;
    }
    if (r == 0) {
        return;
    }

    /* フレーム完成 */
    uint64_t pts_us;
    gbac_pts_push(&g_pts, ln.frame_no, &pts_us);

    bool swapped = false;
    portENTER_CRITICAL(&g_mux);
    int other = 1 - cur;
    if (g_ready < 0 && g_held < 0) {
        g_slot_pts[cur] = pts_us;
        g_ready = cur;
        g_cur = other;
        swapped = true;
    }
    portEXIT_CRITICAL(&g_mux);

    if (swapped) {
        xSemaphoreGive(g_sig);
    } else {
        /* 読み手が 1 枚も空けていない。このフレームは同じバッファに
         * 上書きされるので、欠落として数えておく。 */
        g_asm[cur]->dropped_frames++;
    }
}

const uint16_t *frame_pipe_take(uint64_t *pts_us, TickType_t wait_ticks)
{
    if (xSemaphoreTake(g_sig, wait_ticks) != pdTRUE) {
        return NULL;
    }
    portENTER_CRITICAL(&g_mux);
    int idx = g_ready;
    if (idx >= 0) {
        g_ready = -1;
        g_held = idx;
    }
    portEXIT_CRITICAL(&g_mux);

    if (idx < 0) {
        return NULL;
    }
    if (pts_us) {
        *pts_us = g_slot_pts[idx];
    }
    return &g_asm[idx]->lines[0][0];
}

void frame_pipe_release(void)
{
    portENTER_CRITICAL(&g_mux);
    g_held = -1;
    portEXIT_CRITICAL(&g_mux);
}

uint32_t frame_pipe_completed(void)
{
    uint32_t n = 0;
    for (int i = 0; i < NSLOT; i++) {
        if (g_asm[i]) {
            n += g_asm[i]->completed_frames;
        }
    }
    return n;
}

uint32_t frame_pipe_dropped(void)
{
    uint32_t n = 0;
    for (int i = 0; i < NSLOT; i++) {
        if (g_asm[i]) {
            n += g_asm[i]->dropped_frames;
        }
    }
    return n;
}

void frame_pipe_stats_reset(void)
{
    for (int i = 0; i < NSLOT; i++) {
        if (g_asm[i]) {
            g_asm[i]->completed_frames = 0;
            g_asm[i]->dropped_frames = 0;
        }
    }
}
