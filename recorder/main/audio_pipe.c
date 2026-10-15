#include "audio_pipe.h"

#include <stdlib.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "gbacap.h"

static const char *TAG = "aud";

/* リングは 2 の冪。1 ペア = 右/左の 2 つの uint16。
 * 4096 ペア = 125ms 分(16KB)。内部 RAM に置く。 */
#define RING_PAIRS 4096
#define RING_MASK  (RING_PAIRS * 2 - 1)

static uint16_t *g_ring;
static volatile uint32_t g_w;   /* 書き込み位置(uint16 個) */
static volatile uint32_t g_r;   /* 読み出し位置(uint16 個) */

static gbac_audio_fx_t g_fx;
static gbac_resamp_t g_rs;
static uint32_t g_gaps;
static uint32_t g_overruns;
static int g_have_seq;
static uint8_t g_seq;

static uint16_t s_raw[64 * 2];
static int16_t s_fx[64 * 2];

esp_err_t audio_pipe_init(int volume_pct)
{
    if (g_ring == NULL) {
        g_ring = heap_caps_malloc(sizeof(uint16_t) * RING_PAIRS * 2,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (g_ring == NULL) {
            ESP_LOGE(TAG, "音声リングが確保できない");
            return ESP_ERR_NO_MEM;
        }
    }
    audio_pipe_reset();
    gbac_audio_fx_init(&g_fx, (float)volume_pct / 100.0f);
    gbac_resamp_init(&g_rs, 32802, 48000);
    return ESP_OK;
}

void audio_pipe_reset(void)
{
    g_w = 0;
    g_r = 0;
    g_gaps = 0;
    g_overruns = 0;
    g_have_seq = 0;
    g_seq = 0;
}

void audio_pipe_push(const uint8_t *pkt, int len)
{
    gbac_audio_t a;
    if (!gbac_audio_parse(pkt, len, &a)) {
        return;
    }

    /* seq は 8bit 循環。飛んでいれば音声データそのものが失われている */
    if (g_have_seq) {
        uint8_t want = (uint8_t)(g_seq + 1);
        if (a.seq != want) {
            g_gaps++;
        }
    }
    g_have_seq = 1;
    g_seq = a.seq;

    int words = a.pairs * 2;
    uint32_t w = g_w;
    uint32_t used = w - g_r;
    if (used + (uint32_t)words > RING_PAIRS * 2) {
        g_overruns++;
        return;     /* 読み出しが遅い。新しい方を捨てる */
    }
    for (int i = 0; i < words; i++) {
        g_ring[(w + i) & RING_MASK] = a.samples[i];
    }
    g_w = w + (uint32_t)words;    /* 32bit 循環で問題なし */
}

int audio_pipe_read(int16_t *out, int max_pairs)
{
    int produced = 0;

    while (produced < max_pairs) {
        int room = max_pairs - produced;
        uint32_t avail = g_w - g_r;          /* uint16 個 */
        int avail_pairs = (int)(avail / 2);
        if (avail_pairs <= 0) {
            break;
        }
        /* リサンプルは入力チャンク 1 個に対して 48000/32802 倍(+1)の出力を作る。
         * 出力が足りないと入力を途中で捨ててしまうので、部屋から入力個数を決める。 */
        int n = (int)(((int64_t)room * 32802) / 48000);
        if (n <= 0) {
            break;
        }
        if (n > avail_pairs) {
            n = avail_pairs;
        }
        if (n > (int)(sizeof(s_raw) / sizeof(s_raw[0]) / 2)) {
            n = (int)(sizeof(s_raw) / sizeof(s_raw[0]) / 2);
        }

        uint32_t r = g_r;
        for (int i = 0; i < n * 2; i++) {
            s_raw[i] = g_ring[(r + i) & RING_MASK];
        }
        g_r = r + (uint32_t)(n * 2);

        gbac_audio_fx_process(&g_fx, s_raw, n, s_fx);
        int got = gbac_resamp_run(&g_rs, s_fx, n, out + produced * 2, room);
        produced += got;
        if (got == 0) {
            break;
        }
    }
    return produced;
}

uint32_t audio_pipe_gaps(void)
{
    return g_gaps;
}

uint32_t audio_pipe_overruns(void)
{
    return g_overruns;
}
