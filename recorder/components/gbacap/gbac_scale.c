/* gbac_scale.c — 整数倍 nearest neighbor 拡大 + BT.601 フルレンジ O_UYY_E_VYY
 * 画素複製のみ(補間しない)。P4 の PPA は bilinear しか無いので使わない。
 *
 * 出力レイアウト(esp_h264 の HW エンコーダが要求する形式):
 *   1 行 = W*3/2 バイト。偶数行は [U Y Y U Y Y ...]、奇数行は [V Y Y V Y Y ...]。
 *   行 r の画素ペア cx の彩度は、拡大後平面の 2x2 ブロック
 *   {(2cx,2cy),(2cx+1,2cy),(2cx,2cy+1),(2cx+1,2cy+1)} の RGB 平均から作る。
 *
 * スタックを約 4.2KB 使う。呼び出し側は 8KB 以上のスタックを用意すること。 */
#include "gbacap.h"

#define GBAC_CW_MAX (GBAC_WIDTH * GBAC_MAX_SCALE / 2) /* 480 */
#define GBAC_YW_MAX (GBAC_WIDTH * GBAC_MAX_SCALE)     /* 960 */

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
  for (int x = 0; x < w; x++) {
    uint8_t rgb[3];
    gbac_rgb565_to888(s[x], rgb);
    c->r[oldest][x] = rgb[0];
    c->g[oldest][x] = rgb[1];
    c->b[oldest][x] = rgb[2];
  }
  c->idx[oldest] = row;
  return oldest;
}

/* 1 行を書く。crow = 彩度配列(W/2 個)。yrow = 拡大済み Y(W 個)。 */
static void write_row(uint8_t *dst, const uint8_t *crow, const uint8_t *yrow,
                      int W) {
  for (int cx = 0; cx < W / 2; cx++) {
    uint8_t *p = dst + cx * 3;
    p[0] = crow[cx];
    p[1] = yrow[cx * 2];
    p[2] = yrow[cx * 2 + 1];
  }
}

/* 元行 sy の Y を W 画素ぶん最近傍で複製する */
static void build_yrow(const row_cache_t *c, int slot, int w, int scale,
                       uint8_t *yrow) {
  for (int x = 0; x < w; x++) {
    uint8_t y = (uint8_t)luma(c->r[slot][x], c->g[slot][x], c->b[slot][x]);
    for (int dx = 0; dx < scale; dx++) yrow[x * scale + dx] = y;
  }
}

void gbac_scale_ouyy(const uint16_t *src, int w, int h, int scale,
                     uint8_t *out) {
  if (!src || !out || w <= 0 || h <= 0) return;
  if (scale < 1) scale = 1;
  if (scale > GBAC_MAX_SCALE) scale = GBAC_MAX_SCALE;

  const int W = w * scale;
  const int H = h * scale;
  const int stride = W * 3 / 2;
  const int cw = W / 2;

  row_cache_t cache;
  cache.idx[0] = cache.idx[1] = cache.idx[2] = -1;

  uint8_t urow[GBAC_CW_MAX], vrow[GBAC_CW_MAX];
  uint8_t yrow[GBAC_YW_MAX];

  int prev_a = -1, prev_b = -1, prev_y = -1;

  for (int cy = 0; 2 * cy < H; cy++) {
    /* ---- 彩度: 元行 a と b の、画素ペアにまたがる 4 画素の RGB 平均 ---- */
    int a = (2 * cy) / scale;
    int b = (2 * cy + 1) / scale;
    if (a >= h) a = h - 1;
    if (b >= h) b = h - 1;

    if (a != prev_a || b != prev_b) {
      int sa = row_slot(&cache, src, w, a);
      int sb = row_slot(&cache, src, w, b);
      for (int cx = 0; cx < cw; cx++) {
        int xa = (2 * cx) / scale;
        int xb = (2 * cx + 1) / scale;
        if (xa >= w) xa = w - 1;
        if (xb >= w) xb = w - 1;
        int R = cache.r[sa][xa] + cache.r[sa][xb] + cache.r[sb][xa] + cache.r[sb][xb];
        int G = cache.g[sa][xa] + cache.g[sa][xb] + cache.g[sb][xa] + cache.g[sb][xb];
        int B = cache.b[sa][xa] + cache.b[sa][xb] + cache.b[sb][xa] + cache.b[sb][xb];
        R = (R + 2) >> 2;
        G = (G + 2) >> 2;
        B = (B + 2) >> 2;
        urow[cx] = (uint8_t)chroma_u(R, G, B);
        vrow[cx] = (uint8_t)chroma_v(R, G, B);
      }
      prev_a = a;
      prev_b = b;
      /* 行キャッシュを入れ替えたので Y 行は作り直し */
      prev_y = -1;
    }

    /* ---- Y: 元行が変わった時だけ作り直す ---- */
    for (int k = 0; k < 2 && 2 * cy + k < H; k++) {
      int sy = (2 * cy + k) / scale;
      if (sy >= h) sy = h - 1;
      if (sy != prev_y) {
        int slot = row_slot(&cache, src, w, sy);
        build_yrow(&cache, slot, w, scale, yrow);
        prev_y = sy;
      }
      write_row(out + (size_t)(2 * cy + k) * stride, k == 0 ? urow : vrow, yrow,
                W);
    }
  }
}
