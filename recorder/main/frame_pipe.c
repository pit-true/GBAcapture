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

/* 4 枚ぶん持つ(2026-10-18)。
 *   読み手(1)+ 読み待ち(1)+ 組み立て中の今の枠(1)+ 次の枠(1)。
 * 読み待ちは常にいちばん新しい枚だけを残す(古い方は捨てる)。
 * 「次の枠」は、EP4 と EP1 でラインの届く順が前後するのを吸収する。nano v0.34 から
 * パケットが 512B になり、ホストは EP4 を 4KB(8 ライン)ずつまとめて受けるので、
 * EP1 で届いたフレーム N+1 のラインが、EP4 にまだ残っているフレーム N の末尾より
 * 先に見えることがある。1 枠しか持たないと N を途中で閉じて末尾 8 ラインを捨てていた
 * (t10: 1 枚あたり 3.3 ライン欠け)。N と N+1 を同時に組み、N は 160 ライン揃うか
 * N+2 が来た時点で出す。
 * 1 枚 76.8KB x 4 = 307KB は PSRAM に置く。 */
#define NSLOT 4

static gbac_asm_t *g_asm[NSLOT];
static uint64_t g_slot_pts[NSLOT];
static int g_cur = -1;       /* 組み立て中の枠(フレーム N)。-1 = なし */
static int g_next = -1;      /* 次の枠(フレーム N+1)。-1 = なし */
static int g_ready = -1;     /* 読み待ちの slot(-1 = なし) */
static int g_held = -1;      /* 読み手が持っている slot */
static SemaphoreHandle_t g_sig;
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static gbac_pts_t g_pts;
static int g_pts_dbg;   /* 先頭 20 フレームの PTS をログに出す回数 */

esp_err_t frame_pipe_init(void)
{
    for (int i = 0; i < NSLOT; i++) {
        /* 1 枚 76.8KB。internal に置くと枠だけで 300KB 使うので PSRAM。 */
        g_asm[i] = heap_caps_malloc(sizeof(gbac_asm_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (g_asm[i] == NULL) {
            ESP_LOGE(TAG, "フレームバッファが確保できない");
            return ESP_ERR_NO_MEM;
        }
        gbac_asm_init(g_asm[i]);
        /* 復号は拡大側の直引き表に任せる(2026-10-18)。rx は 480B の
         * memcpy だけになり、core1 の rx が 1 ライン 19us → 数 us になる。
         * frame_pipe_take が返す枠は nano のサンプルそのまま。 */
        gbac_asm_set_raw(g_asm[i], 1);
    }
    g_sig = xSemaphoreCreateBinary();
    gbac_pts_init(&g_pts);
    g_cur = -1;
    g_next = -1;
    g_ready = -1;
    g_held = -1;
    return ESP_OK;
}

/* 読み手にも読み待ちにも組み立てにも使っていない slot を 1 つ返す。
 * 4 枠で同時に使うのは最大 4(読み手・読み待ち・今・次)なので、
 * 今か次のどちらかが空いているときに呼べば必ず見つかる。 */
static int free_slot(void)
{
    for (int i = 0; i < NSLOT; i++) {
        if (i != g_held && i != g_ready && i != g_cur && i != g_next) {
            return i;
        }
    }
    return -1;
}

/* slot を完成扱いで読み待ちへ回す。読み待ちにすでに古い枚があれば捨てる
 * (= dropped_frames)。PTS を進められないフレーム(同じ/後戻りのフレーム番号)は
 * 出さずに捨てる。mp4 の尺が実時間と合わなくなるため(2026-10-16 実測)。 */
static void emit_frame(int slot)
{
    uint8_t frame_no = g_asm[slot]->frame_no;
    uint64_t pts_us = 0;
    if (!gbac_pts_push(&g_pts, frame_no, &pts_us)) {
        g_asm[slot]->dropped_frames++;
        if (g_pts_dbg < 20) {
            ESP_LOGW(TAG, "pts 拒否 %d/20 frame_no=%u(last=%u) → 捨てる",
                     g_pts_dbg + 1, (unsigned)frame_no, (unsigned)g_pts.last);
            g_pts_dbg++;
        }
        return;
    }
    if (g_pts_dbg < 20) {
        ESP_LOGI(TAG, "pts %d/20 frame_no=%u pts_us=%llu (%.1fms)", g_pts_dbg + 1,
                 (unsigned)frame_no, (unsigned long long)pts_us, (double)pts_us / 1000.0);
        g_pts_dbg++;
    }
    portENTER_CRITICAL(&g_mux);
    if (g_ready >= 0) {
        g_asm[g_ready]->dropped_frames++;   /* 読み手が遅い。古い方を捨てる */
    }
    g_slot_pts[slot] = pts_us;
    g_ready = slot;
    portEXIT_CRITICAL(&g_mux);
    xSemaphoreGive(g_sig);
}

/* 今の枠を出して、次の枠を今の枠に繰り上げる */
static void close_cur(void)
{
    if (g_cur < 0) {
        return;
    }
    int slot = g_cur;
    g_cur = g_next;
    g_next = -1;
    emit_frame(slot);   /* emit の中で g_ready が slot になる。g_cur からは外してある */
}

void frame_pipe_push(const uint8_t *pkt, int len)
{
    gbac_line_t ln;
    if (!gbac_line_parse(pkt, len, &ln)) {
        return;
    }

    if (g_cur < 0) {
        g_cur = free_slot();
        gbac_asm_start(g_asm[g_cur], ln.frame_no);
    }

    /* フレーム番号の差(0..255)。0 = 今の枠、1 = 次の枠、2..127 = もっと先、
     * 128.. = 古い(後戻り) */
    int d = gbac_frame_delta(g_asm[g_cur]->frame_no, ln.frame_no);
    if (d >= 128) {
        g_asm[g_cur]->dropped_lines++;
        return;
    }
    while (d >= 2) {
        /* N+2 以降が来た。N は欠けていても閉じ、次の枠を繰り上げる */
        g_asm[g_cur]->completed_frames++;
        close_cur();
        if (g_cur < 0) {
            g_cur = free_slot();
            gbac_asm_start(g_asm[g_cur], ln.frame_no);
            d = 0;
            break;
        }
        d = gbac_frame_delta(g_asm[g_cur]->frame_no, ln.frame_no);
    }

    int target;
    if (d == 0) {
        target = g_cur;
    } else {
        if (g_next < 0) {
            g_next = free_slot();
            gbac_asm_start(g_asm[g_next], ln.frame_no);
        }
        target = g_next;
    }

    int r = gbac_asm_push(g_asm[target], &ln);
    if (r == 1) {
        /* 160 ライン揃った */
        if (target == g_cur) {
            close_cur();
        } else {
            /* 次の枠が先に揃った。順番を守るため今の枠を欠けたまま先に出す */
            g_asm[g_cur]->completed_frames++;
            close_cur();          /* g_cur = 揃った次の枠 */
            close_cur();          /* それも出す */
        }
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
