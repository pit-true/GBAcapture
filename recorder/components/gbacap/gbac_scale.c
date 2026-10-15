/* gbac_scale.c — 整数倍 nearest neighbor 拡大 + BT.601 フルレンジ O_UYY_E_VYY
 * 画素複製のみ(補間しない)。P4 の PPA は bilinear しか無いので使わない。
 *
 * 出力レイアウト(esp_h264 の HW エンコーダが要求する形式):
 *   1 行 = W*3/2 バイト。偶数行は [U Y Y U Y Y ...]、奇数行は [V Y Y V Y Y ...]。
 *   行 r の画素ペア cx の彩度は、拡大後平面の 2x2 ブロック
 *   {(2cx,2cy),(2cx+1,2cy),(2cx,2cy+1),(2cx+1,2cy+1)} の RGB 平均から作る。
 *
 * 速さのための約束(2026-10-15 実測で 37ms → 目標 5ms 以下):
 *   - 出力画素 → 元画素の対応(xmap)は呼び出しごとに 1 回作る。
 *     内側ループで割り算をすると 1 枚 30 万回の割り算になり 37ms の主因だった。
 *   - 1 行はワークバッファに 32bit で組み、同じ行を memcpy で複製する。
 *     1 バイトずつの書き込み(1 枚 92 万回)も 37ms の一因。
 *   - スケールが偶数のとき、彩度の 2x2 ブロックは元画素 1 個に潰れる
 *     (2cx と 2cx+1 が違う元画素へ移るのは 2cx+1 がスケールの倍数のときだけ。
 *      偶数スケールでは 2cx+1 は奇数なので倍数になりえない)。
 *     なので 4 画素平均の代わりに 1 画素の RGB をそのまま使う。結果は同一。
 *
 * ワーク領域は static(単一タスクからの呼び出し前提)。再入はしない。
 * スタックは 1KB 未満。 */
#include "gbacap.h"
#include <string.h>

/* 内訳の実測。ESP_IDF でのだけ計測する(ホストテストは 0 のまま)。
 * 1 フレームで 4 回読むだけなのでオーバーヘッドは無視できる。 */
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#define PROF_NOW() esp_timer_get_time()
#else
#define PROF_NOW() (int64_t)0
#endif

int64_t gbac_scale_prof[4];

#define GBAC_CW_MAX (GBAC_WIDTH * GBAC_MAX_SCALE / 2) /* 480 */
#define GBAC_YW_MAX (GBAC_WIDTH * GBAC_MAX_SCALE)     /* 960 */
#define GBAC_YH_MAX (GBAC_HEIGHT * GBAC_MAX_SCALE)    /* 640 */

static int clamp8(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return v;
}

/* Y = (77R + 150G + 29B) >> 8 (フルレンジ) */
static int luma(int r, int g, int b) { return (77 * r + 150 * g + 29 * b) >> 8; }

/* Cb/Cr。算術右シフト(GCC/Clang 前提)で切り捨てる。 */
static int chroma_u(int r, int g, int b) {
  return clamp8(128 + ((-43 * r - 85 * g + 128 * b + 128) >> 8));
}
static int chroma_v(int r, int g, int b) {
  return clamp8(128 + ((128 * r - 107 * g - 21 * b + 128) >> 8));
}

/* ---- 元画像の行を RGB888 に展開して保持する 3 スロットのキャッシュ ----
 * 要求される行番号は単調に増える(彩度は隣り合う 2 行を見比べる)ので、
 * いちばん古い行を追い出せば常に当たる。 */
typedef struct {
  int idx[3];
  uint8_t r[3][GBAC_WIDTH];
  uint8_t g[3][GBAC_WIDTH];
  uint8_t b[3][GBAC_WIDTH];
} row_cache_t;

/* row を展開済みスロットの番号で返す。無ければいちばん古い行と入れ替える。 */
static int row_slot(row_cache_t *c, const uint16_t *src, int w, int row) {
  int oldest = 0;
  for (int i = 0; i < 3; i++) {
    if (c->idx[i] == row) return i;
    if (c->idx[i] < c->idx[oldest]) oldest = i;
  }
  const uint16_t *s = src + (size_t)row * w;
  int64_t t0 = PROF_NOW();
  for (int x = 0; x < w; x++) {
    uint8_t rgb[3];
    gbac_rgb565_to888(s[x], rgb);
    c->r[oldest][x] = rgb[0];
    c->g[oldest][x] = rgb[1];
    c->b[oldest][x] = rgb[2];
  }
  gbac_scale_prof[0] += PROF_NOW() - t0;
  c->idx[oldest] = row;
  return oldest;
}

static row_cache_t s_cache;
static uint8_t s_yrow[GBAC_YW_MAX];
static uint8_t s_urow[GBAC_YW_MAX * 3 / 2];
static uint8_t s_vrow[GBAC_YW_MAX * 3 / 2];
static uint16_t s_xmap[GBAC_YW_MAX];

/* 元行 sy の Y を W 画素ぶん最近傍で複製する */
static void build_yrow(const row_cache_t *c, int slot, int w, int scale,
                       uint8_t *yrow) {
  for (int x = 0; x < w; x++) {
    uint8_t y = (uint8_t)luma(c->r[slot][x], c->g[slot][x], c->b[slot][x]);
    uint8_t *p = yrow + x * scale;
    for (int dx = 0; dx < scale; dx++) p[dx] = y;
  }
}

/* 彩度行(U 行と V 行)を Y と合わせて組み立てる。
 * 偶数行は [U Y Y U Y Y ...]、奇数行は [V Y Y V Y Y ...] の形になる。
 * even が真ならスケールが偶数で、彩度ブロックは元画素 1 個に潰れる。 */
static void build_uv_rows(uint8_t *urow, uint8_t *vrow, const uint8_t *yrow,
                          const uint16_t *xmap, int cw, int even,
                          const uint8_t *ra, const uint8_t *ga,
                          const uint8_t *ba, const uint8_t *rb,
                          const uint8_t *gb, const uint8_t *bb) {
  int cx = 0;
  if (even) {
    for (; cx + 4 <= cw; cx += 4) {
      const int x0 = xmap[2 * cx], x1 = xmap[2 * cx + 2], x2 = xmap[2 * cx + 4],
                x3 = xmap[2 * cx + 6];
      const uint32_t y0 = yrow[2 * cx], y1 = yrow[2 * cx + 2],
                     y2 = yrow[2 * cx + 4], y3 = yrow[2 * cx + 6];
      uint32_t u0, u1, u2, u3, v0, v1, v2, v3;
      u0 = (uint32_t)chroma_u(ra[x0], ga[x0], ba[x0]);
      v0 = (uint32_t)chroma_v(ra[x0], ga[x0], ba[x0]);
      u1 = (uint32_t)chroma_u(ra[x1], ga[x1], ba[x1]);
      v1 = (uint32_t)chroma_v(ra[x1], ga[x1], ba[x1]);
      u2 = (uint32_t)chroma_u(ra[x2], ga[x2], ba[x2]);
      v2 = (uint32_t)chroma_v(ra[x2], ga[x2], ba[x2]);
      u3 = (uint32_t)chroma_u(ra[x3], ga[x3], ba[x3]);
      v3 = (uint32_t)chroma_v(ra[x3], ga[x3], ba[x3]);
      uint32_t *up = (uint32_t *)(urow + cx * 3);
      uint32_t *vp = (uint32_t *)(vrow + cx * 3);
      /* 4 画素で 12 バイト = 3 語。バイト順は
       *   [C0 y0 y0 C1 | y1 y1 C2 y2 | y2 C3 y3 y3]
       * cx が 4 の倍数のときだけ cx*3 が 4 バイト境界になる(12 の倍数)。 */
      up[0] = u0 | (y0 << 8) | (y0 << 16) | (u1 << 24);
      up[1] = y1 | (y1 << 8) | (u2 << 16) | (y2 << 24);
      up[2] = y2 | (u3 << 8) | (y3 << 16) | (y3 << 24);
      vp[0] = v0 | (y0 << 8) | (y0 << 16) | (v1 << 24);
      vp[1] = y1 | (y1 << 8) | (v2 << 16) | (y2 << 24);
      vp[2] = y2 | (v3 << 8) | (y3 << 16) | (y3 << 24);
    }
    for (; cx < cw; cx++) {
      const int x = xmap[2 * cx];
      const uint8_t y = yrow[2 * cx];
      uint8_t *p = urow + cx * 3;
      p[0] = (uint8_t)chroma_u(ra[x], ga[x], ba[x]);
      p[1] = y;
      p[2] = y;
      p = vrow + cx * 3;
      p[0] = (uint8_t)chroma_v(ra[x], ga[x], ba[x]);
      p[1] = y;
      p[2] = y;
    }
    return;
  }

  /* 奇数スケール。彩度ブロックが元画素 2 個またぐことがあるので 4 画素平均。 */
  for (; cx + 4 <= cw; cx += 4) {
    uint32_t ua[4], va[4], ya[4], yb[4];
    for (int k = 0; k < 4; k++) {
      const int xa = xmap[2 * (cx + k)], xb = xmap[2 * (cx + k) + 1];
      int R = ra[xa] + ra[xb] + rb[xa] + rb[xb];
      int G = ga[xa] + ga[xb] + gb[xa] + gb[xb];
      int B = ba[xa] + ba[xb] + bb[xa] + bb[xb];
      ua[k] = (uint32_t)chroma_u((R + 2) >> 2, (G + 2) >> 2, (B + 2) >> 2);
      va[k] = (uint32_t)chroma_v((R + 2) >> 2, (G + 2) >> 2, (B + 2) >> 2);
      ya[k] = yrow[2 * (cx + k)];
      yb[k] = yrow[2 * (cx + k) + 1];
    }
    uint32_t *up = (uint32_t *)(urow + cx * 3);
    uint32_t *vp = (uint32_t *)(vrow + cx * 3);
    up[0] = ua[0] | (ya[0] << 8) | (yb[0] << 16) | (ua[1] << 24);
    up[1] = ya[1] | (yb[1] << 8) | (ua[2] << 16) | (ya[2] << 24);
    up[2] = yb[2] | (ua[3] << 8) | (ya[3] << 16) | (yb[3] << 24);
    vp[0] = va[0] | (ya[0] << 8) | (yb[0] << 16) | (va[1] << 24);
    vp[1] = ya[1] | (yb[1] << 8) | (va[2] << 16) | (ya[2] << 24);
    vp[2] = yb[2] | (va[3] << 8) | (ya[3] << 16) | (yb[3] << 24);
  }
  for (; cx < cw; cx++) {
    const int xa = xmap[2 * cx], xb = xmap[2 * cx + 1];
    int R = (ra[xa] + ra[xb] + rb[xa] + rb[xb] + 2) >> 2;
    int G = (ga[xa] + ga[xb] + gb[xa] + gb[xb] + 2) >> 2;
    int B = (ba[xa] + ba[xb] + bb[xa] + bb[xb] + 2) >> 2;
    uint8_t *p = urow + cx * 3;
    p[0] = (uint8_t)chroma_u(R, G, B);
    p[1] = yrow[2 * cx];
    p[2] = yrow[2 * cx + 1];
    p = vrow + cx * 3;
    p[0] = (uint8_t)chroma_v(R, G, B);
    p[1] = yrow[2 * cx];
    p[2] = yrow[2 * cx + 1];
  }
}

void gbac_scale_ouyy(const uint16_t *src, int w, int h, int scale,
                     uint8_t *out) {
  if (!src || !out || w <= 0 || h <= 0) return;
  if (scale < 1) scale = 1;
  if (scale > GBAC_MAX_SCALE) scale = GBAC_MAX_SCALE;

  const int W = w * scale;
  const int H = h * scale;
  if (W > GBAC_YW_MAX || H > GBAC_YH_MAX) return; /* バッファ越えは守る */

  const int stride = W * 3 / 2;
  const int cw = W / 2;
  const int even = (scale & 1) == 0;

  /* 出力画素 → 元画素。割り算はここで 1 回だけ */
  {
    int x = 0, n = 0;
    for (int p = 0; p < W; p++) {
      s_xmap[p] = (uint16_t)x;
      if (++n == scale) {
        n = 0;
        x++;
      }
    }
  }

  s_cache.idx[0] = s_cache.idx[1] = s_cache.idx[2] = -1;

  int cur_a = -1, cur_b = -1, cur_sy = -1;
  int r = 0;
  while (r < H) {
    const int sy = r / scale;
    const int cy = r / 2;
    const int a = (2 * cy) / scale;
    const int b = (2 * cy + 1) / scale; /* H = h*scale なので h 越えはしない */

    if (a != cur_a || b != cur_b || sy != cur_sy) {
      const int sa = row_slot(&s_cache, src, w, a);
      const int sb = row_slot(&s_cache, src, w, b);
      if (sy != cur_sy) {
        const int slot = row_slot(&s_cache, src, w, sy);
        int64_t t1 = PROF_NOW();
        build_yrow(&s_cache, slot, w, scale, s_yrow);
        gbac_scale_prof[1] += PROF_NOW() - t1;
        cur_sy = sy;
      }
      {
        int64_t t2 = PROF_NOW();
        build_uv_rows(s_urow, s_vrow, s_yrow, s_xmap, cw, even,
                      s_cache.r[sa], s_cache.g[sa], s_cache.b[sa], s_cache.r[sb],
                      s_cache.g[sb], s_cache.b[sb]);
        gbac_scale_prof[2] += PROF_NOW() - t2;
      }
      cur_a = a;
      cur_b = b;
    }

    /* 同じ Y 行・同じ彩度行が続く行数だけ memcpy で埋める */
    int nrow = 1;
    while (r + nrow < H && (r + nrow) / scale == sy &&
           (2 * ((r + nrow) / 2)) / scale == a &&
           (2 * ((r + nrow) / 2) + 1) / scale == b)
      nrow++;
    {
      int64_t t3 = PROF_NOW();
      for (int k = 0; k < nrow; k++)
        memcpy(out + (size_t)(r + k) * stride, ((r + k) & 1) ? s_vrow : s_urow,
               stride);
      gbac_scale_prof[3] += PROF_NOW() - t3;
    }
    r += nrow;
  }
}
