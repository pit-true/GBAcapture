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
static int g_pts_dbg;   /* 先頭 20 フレームの PTS をログに出す回数 */

esp_err_t frame_pipe_init(void)
{
    for (int i = 0; i < NSLOT; i++) {
        /* 1 枚 76.8KB。internal に置くと 2 枚で 154KB を常時つかむ。
         * エンコーダ(internal に約 69KB の連続ブロック + DMA バッファ)と
         * GDMA のステージ(23,040B x2)を起動時に常時確保するようになったため、
         * internal からは確保できなくなった(2026-10-19 実測: internal に
         * 確保していた間は frame_pipe だけで 152KB 減っていた)。
         * PSRAM でもキャッシュが効くので実測で遅さを確かめる(bench 'b')。 */
        g_asm[i] = heap_caps_malloc(sizeof(gbac_asm_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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

/* slot を完成扱いで読み待ちへ回す。読み手が 1 枚も空けていなければ
 * 枠が取れないので同じ slot にそのまま残す(= dropped_frames)。
 * PTS を進められないフレーム(同じ/後戻りのフレーム番号)も完成扱いにしない。
 * mp4 の尺が実時間と合わなくなるため(2026-10-16 実測)。
 * 戻り値 = 次に埋める slot */
static int finish_frame(int cur, uint8_t frame_no)
{
    uint64_t pts_us = 0;
    if (!gbac_pts_push(&g_pts, frame_no, &pts_us)) {
        g_asm[cur]->dropped_frames++;
        if (g_pts_dbg < 20) {
            ESP_LOGW(TAG, "pts 拒否 %d/20 frame_no=%u(last=%u) → 捨てる",
                     g_pts_dbg + 1, (unsigned)frame_no, (unsigned)g_pts.last);
            g_pts_dbg++;
        }
        return cur;
    }
    if (g_pts_dbg < 20) {
        ESP_LOGI(TAG, "pts %d/20 frame_no=%u pts_us=%llu (%.1fms)", g_pts_dbg + 1,
                 (unsigned)frame_no, (unsigned long long)pts_us, (double)pts_us / 1000.0);
        g_pts_dbg++;
    }

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
        return other;
    }
    g_asm[cur]->dropped_frames++;
    return cur;
}

void frame_pipe_push(const uint8_t *pkt, int len)
{
    gbac_line_t ln;
    if (!gbac_line_parse(pkt, len, &ln)) {
        return;
    }

    int cur = g_cur;
    int r = gbac_asm_push(g_asm[cur], &ln);

    if (r == 2) {
        /* より新しいフレーム番号のラインが来た。
         * 今のフレームは行が欠けていてもここで完成扱いにする
         * (欠けた行は前フレームの内容が残っている)。 */
        cur = finish_frame(cur, g_asm[cur]->frame_no);
        gbac_asm_start(g_asm[cur], ln.frame_no);
        gbac_asm_push(g_asm[cur], &ln);
        return;
    }
    if (r != 1) {
        return;   /* 0=まだ未完、-1=範囲外、-2=古いフレームのライン */
    }
    finish_frame(cur, ln.frame_no);
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

uint32_t frame_pipe_dropped_lines(void)
{
    uint32_t n = 0;
    for (int i = 0; i < NSLOT; i++) {
        if (g_asm[i]) {
            n += g_asm[i]->dropped_lines;
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
            g_asm[i]->dropped_lines = 0;
        }
    }
    /* PTS は録画・テストのたびに 0 から。前回の続きだと mp4 の pts が
     * 0 開始でなくなり、尺の実測と比べにくくなる。 */
    gbac_pts_init(&g_pts);
    g_pts_dbg = 0;
}
