/* gbac_audio.c — 音声加工とリサンプル
 * 加工連鎖は host/GbaCap.Viewer/ViewerForm.cs (551〜612行) と同じ順:
 *   DCオフセット追従(係数0.0005) → 2次バターワースLPF → ゲイン(2*volume) → int16
 * 入力は ADC 生 [右, 左](SO1=右, SO2=左)。出力は [左, 右]。 */
#include "gbacap.h"

/* fc=13kHz @ 32.768kHz の 2次バターワース。係数は ViewerForm の Biquad と同一。 */
#define BQ_B0 0.6309f
#define BQ_B1 1.2618f
#define BQ_B2 0.6309f
#define BQ_A1 1.1206f
#define BQ_A2 0.4030f

static float biquad(float x, float *x1, float *x2, float *y1, float *y2) {
  float y = BQ_B0 * x + BQ_B1 * (*x1) + BQ_B2 * (*x2) - BQ_A1 * (*y1) - BQ_A2 * (*y2);
  *x2 = *x1;
  *x1 = x;
  *y2 = *y1;
  *y1 = y;
  return y;
}

static int16_t clamp16(int v) {
  if (v < -32768) return -32768;
  if (v > 32767) return 32767;
  return (int16_t)v;
}

void gbac_audio_fx_init(gbac_audio_fx_t *fx, float volume) {
  fx->dc_l = 0.0f;
  fx->dc_r = 0.0f;
  fx->x1_l = fx->x2_l = fx->y1_l = fx->y2_l = 0.0f;
  fx->x1_r = fx->x2_r = fx->y1_r = fx->y2_r = 0.0f;
  fx->volume = volume;
}

void gbac_audio_fx_process(gbac_audio_fx_t *fx, const uint16_t *in, int pairs,
                           int16_t *out) {
  for (int i = 0; i < pairs; i++) {
    float r = (float)in[i * 2];
    float l = (float)in[i * 2 + 1];

    /* DC 追従はフィルタ前(C# と同じ位置) */
    fx->dc_l += (l - fx->dc_l) * 0.0005f;
    fx->dc_r += (r - fx->dc_r) * 0.0005f;

    float fl = biquad(l - fx->dc_l, &fx->x1_l, &fx->x2_l, &fx->y1_l, &fx->y2_l);
    float fr = biquad(r - fx->dc_r, &fx->x1_r, &fx->x2_r, &fx->y1_r, &fx->y2_r);

    out[i * 2] = clamp16((int)(fl * 2.0f * fx->volume));
    out[i * 2 + 1] = clamp16((int)(fr * 2.0f * fx->volume));
  }
}

/* ---- リサンプル(線形補間) ---- */
void gbac_resamp_init(gbac_resamp_t *rs, int in_rate, int out_rate) {
  rs->step_q16 = (uint32_t)(((uint64_t)in_rate << 16) / (uint32_t)out_rate);
  if (rs->step_q16 == 0) rs->step_q16 = 1;
  rs->pos_q16 = 0;
  rs->last_l = 0;
  rs->last_r = 0;
  rs->have_last = 0;
}

static void pick_pair(const int16_t *in, int pairs, int idx, int16_t *l,
                      int16_t *r) {
  /* 補間の右端が今回のチャンクに無い場合は保持(先読みしない) */
  if (idx > pairs - 1) idx = pairs - 1;
  *l = in[idx * 2];
  *r = in[idx * 2 + 1];
}

int gbac_resamp_run(gbac_resamp_t *rs, const int16_t *in, int pairs,
                    int16_t *out, int max_out) {
  if (pairs <= 0) return 0;
  int n = 0;
  const uint32_t span = (uint32_t)pairs << 16;

  while (rs->pos_q16 < span && n < max_out) {
    int idx = (int)(rs->pos_q16 >> 16);
    uint32_t frac = rs->pos_q16 & 0xFFFFu;
    int16_t l0, r0, l1, r1;
    pick_pair(in, pairs, idx, &l0, &r0);
    pick_pair(in, pairs, idx + 1, &l1, &r1);

    int32_t L = l0 + (int32_t)(((int64_t)(l1 - l0) * (int64_t)frac) >> 16);
    int32_t R = r0 + (int32_t)(((int64_t)(r1 - r0) * (int64_t)frac) >> 16);
    out[n * 2] = clamp16(L);
    out[n * 2 + 1] = clamp16(R);
    n++;

    rs->pos_q16 += rs->step_q16;
  }

  rs->last_l = in[(pairs - 1) * 2];
  rs->last_r = in[(pairs - 1) * 2 + 1];
  rs->have_last = 1;
  if (rs->pos_q16 >= span) rs->pos_q16 -= span;
  return n;
}
