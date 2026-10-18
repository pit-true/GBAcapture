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
static int row_slot(row_cache_t *c, const uint16_t *src, int w, int row,
                    int raw) {
  int oldest = 0;
  for (int i = 0; i < 3; i++) {
    if (c->idx[i] == row) return i;
    if (c->idx[i] < c->idx[oldest]) oldest = i;
  }
  const uint16_t *s = src + (size_t)row * w;
  int64_t t0 = PROF_NOW();
  for (int x = 0; x < w; x++) {
    uint8_t rgb[3];
    /* raw(nano のサンプルそのまま)なら復号してから展開する */
    gbac_rgb565_to888(raw ? gbac_decode_rgb565_fast(s[x]) : s[x], rgb);
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

/* ---- scale=4 専用の最速経路(2026-10-17) ----
 * scale=4 では
 *   - 彩度の 2x2 ブロックは 1 個の元画素に潰れる(偶数スケールの性質)
 *   - 彩度行 a, b も Y 行 sy も r/4 の元行に一致する(4 行ぶんまったく同じ)
 *   - 元画素 x は出力の画素ペア 2x と 2x+1 を両方埋める
 *     → 1 元画素ぶん 1 行に 6 バイト [C Y Y C Y Y]
 * なので元行 1 本から 4 出力行 [U V U V] を internal RAM のブロックに組み、
 * PSRAM へは 5760 バイト 1 回の memcpy で書く(汎用経路は 1440 バイト×640 回
 * だった)。memcpy の実効帯域は internal→PSRAM で 4KiB chunk なら 183MB/s、
 * 1.4KiB だと 88MB/s(ch32-riscv-ug/wch-protocols E020 実測)。
 * 彩度も 1 元画素ぶんだけ計算する(汎用経路は画素ペアごとに計算し、
 * 同じ元画素の彩度を 2 回計算していた)。
 * 出力は汎用経路とバイト単位で一致。test_scale_ref4 で 921,600 バイト全部を
 * 参照実装と突き合わせて担保している。
 * 内訳カウンタは変換+luma+彩度が prof[0](rgb 欄)、internal RAM 内の複製が
 * prof[1](y 欄)、PSRAM への書き込みが prof[2](uv 欄)。prof[3](copy 欄)は 0 のまま。 */
static uint8_t s_block[4 * GBAC_YW_MAX * 3 / 2] __attribute__((aligned(16)));

/* ---- RGB565 → (Y,U,V) の表(2026-10-18) ----
 * Y・U・V は r・g・b の 1 次式なので、シフト前の積和を成分ごとの表に分ける。
 *   Y_raw = TYr[r5] + TYg[g6] + TYb[b5]   (U_raw, V_raw も同じ形)
 * rgb565→888 の展開((r5<<3)|(r5>>2) など)と係数掛けを表の中で済ませる。
 * 32 + 64 + 32 = 128 個 × 3 成分 = 384 個の int32 = 1.5KB。
 * static で internal RAM に置くので表引きは PSRAM に触れない。
 * 65536 色 x 4 バイト = 256KB の表は PSRAM に置いたが、L2(128KB)より大きく
 * 表引き 1 回が PSRAM 読みになって rgb が 12.9ms/枚になった(r105)。
 * シフト前の和は元の式と同一なので出力は 1 バイトも変わらない。
 * test_ouyy65k が 65536 色すべてを既存の式と突き合わせて担保する。 */
static int32_t TYr[32], TYg[64], TYb[32];
static int32_t TUr[32], TUg[64], TUb[32];
static int32_t TVr[32], TVg[64], TVb[32];
static int s_tables_ready;

int gbac_scale_init_lut(void) {
  for (int i = 0; i < 32; i++) {
    int r = (i << 3) | (i >> 2);
    TYr[i] = 77 * r;
    TUr[i] = -43 * r;
    TVr[i] = 128 * r;
  }
  for (int i = 0; i < 64; i++) {
    int g = (i << 2) | (i >> 4);
    TYg[i] = 150 * g;
    TUg[i] = -85 * g;
    TVg[i] = -107 * g;
  }
  for (int i = 0; i < 32; i++) {
    int b = (i << 3) | (i >> 2);
    TYb[i] = 29 * b;
    TUb[i] = 128 * b;
    TVb[i] = -21 * b;
  }
  s_tables_ready = 1;
  return 0;
}

/* 1 画素ぶんの (Y,U,V) を byte0:Y byte1:U byte2:V で返す。
 * 和だけ表、+128 → >>8 → +128 → 上側クランプは luma/chroma_u8/chroma_v8 と同じ。
 * Y は 0..255 に収まる(最大 256*255 >> 8 = 255)のでクランプしない。
 * U・V の生値は 1..256 なので下側の比較はいらない(clamp8 と同じ)。 */
static inline uint32_t ouyy_of(uint32_t c) {
  int r5 = (int)((c >> 11) & 0x1Fu), g6 = (int)((c >> 5) & 0x3Fu),
      b5 = (int)(c & 0x1Fu);
  uint32_t y = (uint32_t)((TYr[r5] + TYg[g6] + TYb[b5]) >> 8);
  int u = 128 + (int)((TUr[r5] + TUg[g6] + TUb[b5] + 128) >> 8);
  int v = 128 + (int)((TVr[r5] + TVg[g6] + TVb[b5] + 128) >> 8);
  if (u > 255) u = 255;
  if (v > 255) v = 255;
  return y | ((uint32_t)u << 8) | ((uint32_t)v << 16);
}

/* ホストテスト(test_ouyy65k)が表引きの経路を 1 画素ぶん外から見るため */
uint32_t gbac_scale_ouyy_px(uint32_t c) {
  if (!s_tables_ready) gbac_scale_init_lut();
  return ouyy_of(c);
}

/* ---- nano のサンプル → (Y,U,V) の直引き表(2026-10-18) ----
 * 録画中は USB ホスト(割り込み + usbh タスク)が core0 の 6〜7 割を使い、
 * core1 では rx タスクの復号(1 ライン 19us)と AAC が 6 割を使っていた。
 * 復号と色変換を 1 回の表引きにまとめると、rx は memcpy だけになり、
 * 拡大の 1 画素も 9 回の表引き + 加算 + クランプから 2 回の表引きになる。
 * サンプルの bit7 は未使用(gbac_decode.c の V1〜V8)なので 15 ビットに詰める。
 * Y(uint8) 32KB + UV(uint16、下位 U・上位 V) 64KB = 96KB。static の .bss は
 * internal RAM なので表引きが PSRAM を触らない。 */
static uint8_t s_rawY[32768];
static uint16_t s_rawUV[32768];
static int s_raw_ready;

int gbac_scale_init_raw_lut(void) {
  if (!s_tables_ready) gbac_scale_init_lut();
  gbac_decode_tables_init(); /* 汎用経路(奇数スケール)の raw 復号に使う */
  for (uint32_t i = 0; i < 32768u; i++) {
    uint16_t s = (uint16_t)(((i >> 7) << 8) | (i & 0x7Fu));
    uint32_t e = ouyy_of(gbac_decode_rgb565(s));
    s_rawY[i] = (uint8_t)(e & 0xFFu);
    s_rawUV[i] = (uint16_t)(e >> 8);
  }
  s_raw_ready = 1;
  return 0;
}

static inline uint32_t raw_of(uint32_t s) {
  uint32_t i = GBAC_RAW_IDX(s);
  return (uint32_t)s_rawY[i] | ((uint32_t)s_rawUV[i] << 8);
}

uint32_t gbac_scale_raw_px(uint16_t sample) {
  if (!s_raw_ready) gbac_scale_init_raw_lut();
  return raw_of(sample);
}

/* アプリが渡した internal RAM のステージ(scale=4 で 4 元行ぶん)。
 * NULL のあいだは s_block + memcpy の従来経路。
 * 個数はアプリが決める(2026-10-18 に 1 組 4 個を 2 組)。
 * 4 元行グループを組み終わるたびに flush を呼ぶが、そのステージへの
 * 次の書き込みは「そのステージを使った DMA が終わるまで」待たせる。
 * 個数が多いほど DMA の待ちが見えなくなる。
 * 作業台(ctx)ごとに持つので、2 コアから同時に動かしても共有しない。 */
static gbac_scale_ctx_t s_def;   /* gbac_scale_ouyy / set_stage の既定の作業台 */

void gbac_scale_ctx_set(gbac_scale_ctx_t *c, uint8_t *const *stages, int n,
                        size_t stage_bytes, gbac_scale_flush_t flush,
                        gbac_scale_finish_t finish) {
  c->stage_n = (n > GBAC_STAGE_MAX) ? GBAC_STAGE_MAX : (n < 0 ? 0 : n);
  for (int i = 0; i < GBAC_STAGE_MAX; i++) {
    c->stage[i] = (stages != NULL && i < c->stage_n) ? stages[i] : NULL;
  }
  c->stage_bytes = stage_bytes;
  c->stage_rot = 0;
  c->flush = flush;
  c->finish = finish;
  if (c->prof == NULL) c->prof = gbac_scale_prof;
}

void gbac_scale_set_stage(uint8_t *const *stages, int n, size_t stage_bytes,
                          gbac_scale_flush_t flush, gbac_scale_finish_t finish) {
  gbac_scale_ctx_set(&s_def, stages, n, stage_bytes, flush, finish);
}

void gbac_scale_ctx_raw(gbac_scale_ctx_t *c, int raw) {
  c->raw = raw ? 1 : 0;
}

/* 内訳の加算。ctx->prof は gbac_scale_ctx_set で必ず向こうを向く。 */
#define PROF_ADD(c, i, d) ((c)->prof[(i)] += (d))

/* src の 1 行ぶんを一旦 internal RAM に写してから表を引く(2026-10-18)。
 * src は PSRAM のフレームバッファ。表引きと交互に読むと PSRAM の取り合いに
 * 巻き込まれて rgb が倍になった(r105 で 6.9 → 12.9ms)。
 * 1 行 480 バイトを先に 1 回だけ連続読みすると、内側のループは internal だけ。
 * 写し先は ctx ごと(2 コアで共有しない)。 */

/* 元行 1 本ぶんから出力行 2 本 [U 行] [V 行] を作る(1 元画素 6 バイト)。
 * 呼び出しと rgb[3] の受け渡しを消すためここで展開する。 */
static void build_row_x4(gbac_scale_ctx_t *c, const uint16_t *s, int w,
                         uint8_t *rowu, uint8_t *rowv) {
  int64_t t0 = PROF_NOW();
  uint16_t *tmp = c->srcrow;
  if (w <= GBAC_WIDTH) {
    memcpy(tmp, s, (size_t)w * sizeof(uint16_t));
    s = tmp;
  }
  uint32_t *wu = (uint32_t *)rowu;
  uint32_t *wv = (uint32_t *)rowv;
  int x = 0;
  /* 2 画素で 12 バイト = 語 3 個。x が偶数のとき語境界に揃う。
   * [C Y Y C Y Y | C Y Y C Y Y] */
#define X4_PAIR(e0, e1)                                                  \
  do {                                                                   \
    uint32_t y0 = (e0) & 0xFFu, u0 = ((e0) >> 8) & 0xFFu, v0 = (e0) >> 16; \
    uint32_t y1 = (e1) & 0xFFu, u1 = ((e1) >> 8) & 0xFFu, v1 = (e1) >> 16; \
    *wu++ = (uint32_t)(u0 | (y0 << 8) | (y0 << 16) | (u0 << 24));       \
    *wu++ = (uint32_t)(y0 | (y0 << 8) | (u1 << 16) | (y1 << 24));       \
    *wu++ = (uint32_t)(y1 | (u1 << 8) | (y1 << 16) | (y1 << 24));       \
    *wv++ = (uint32_t)(v0 | (y0 << 8) | (y0 << 16) | (v0 << 24));       \
    *wv++ = (uint32_t)(y0 | (y0 << 8) | (v1 << 16) | (y1 << 24));       \
    *wv++ = (uint32_t)(y1 | (v1 << 8) | (y1 << 16) | (y1 << 24));       \
  } while (0)
  if (c->raw) {
    /* サンプル直引き(復号込み)。1 画素 2 回の表引きで Y/U/V がそろう */
    for (; x + 2 <= w; x += 2) {
      uint32_t e0 = raw_of(s[x]);
      uint32_t e1 = raw_of(s[x + 1]);
      X4_PAIR(e0, e1);
    }
  } else {
    /* 表引き 1 回で Y/U/V がそろう(2026-10-18)。 */
    for (; x + 2 <= w; x += 2) {
      uint32_t e0 = ouyy_of(s[x]);
      uint32_t e1 = ouyy_of(s[x + 1]);
      X4_PAIR(e0, e1);
    }
  }
#undef X4_PAIR
  if (x < w) { /* 奇数幅のとき最後の 1 画素(実運用では来ない) */
    uint32_t e = c->raw ? raw_of(s[x]) : ouyy_of(s[x]);
    uint32_t y = e & 0xFFu, u = (e >> 8) & 0xFFu, v = e >> 16;
    uint8_t *p = rowu + x * 6;
    p[0] = (uint8_t)u; p[1] = (uint8_t)y; p[2] = (uint8_t)y;
    p[3] = (uint8_t)u; p[4] = (uint8_t)y; p[5] = (uint8_t)y;
    p = rowv + x * 6;
    p[0] = (uint8_t)v; p[1] = (uint8_t)y; p[2] = (uint8_t)y;
    p[3] = (uint8_t)v; p[4] = (uint8_t)y; p[5] = (uint8_t)y;
  }
  PROF_ADD(c, 0, PROF_NOW() - t0);
}

static void scale_ouyy_x4(gbac_scale_ctx_t *c, const uint16_t *src, int w,
                          int h, uint8_t *out, int row0, int row1) {
  (void)h; /* 範囲は row0/row1 で決まる。呼び出し側が h 以下に切る */
  const int stride = w * 6; /* w * 4 * 3 / 2 */
  const int blk = 4 * stride;  /* 元行 1 本 = 出力行 4 本 */
  const int grp = 4 * blk;     /* 元行 4 本 = 出力行 16 本 */

  /* 表は起動時に gbac_scale_init_lut で作っておく。作り損ねていてもここで作る
   * (ホストテストは gbac_scale_init_lut を呼ばずに同じ経路を通る)。
   * 確保に失敗したあいだは build_row_x4 が計算経路で動く。 */
  if (!s_tables_ready) gbac_scale_init_lut();

  /* ステージが渡されていれば 4 元行ぶんをそこに組み、flush でまとめて
   * 書き出す(DMA 前提)。なければ s_block に 1 元行ずつ組み memcpy で書く。
   * 段の巡回は呼び出しをまたぐカウンターで回す。同じ段を触る間隔は
   * 必ず n 回あき、アプリの「残りを n-2 個以下まで待つ」で安全に守れる。 */
  if (c->stage_n >= 2 && c->stage[0] && c->stage[1] && c->flush &&
      c->stage_bytes >= (size_t)grp && ((row1 - row0) & 3) == 0) {
    for (int g = row0; g < row1; g += 4) {
      uint8_t *stg = c->stage[c->stage_rot];
      if (++c->stage_rot >= c->stage_n) c->stage_rot = 0;
      for (int k = 0; k < 4; k++) {
        uint8_t *b = stg + (size_t)k * blk;
        build_row_x4(c, src + (size_t)(g + k) * w, w, b, b + stride);
        int64_t t1 = PROF_NOW();
        memcpy(b + 2 * stride, b, 2 * stride); /* [U V] を [U V U V] に */
        PROF_ADD(c, 1, PROF_NOW() - t1);
      }
      int64_t t2 = PROF_NOW();
      c->flush(out + (size_t)g * blk, stg, (size_t)grp);
      PROF_ADD(c, 2, PROF_NOW() - t2);
    }
    if (c->finish) c->finish();
    return;
  }

  /* 段を使わない経路。s_block は共有だが、この経路を使うタスクは 1 つだけ
   * (段の確保に失敗したらアプリは分割をやめる)。 */
  uint8_t *rowu = s_block;
  uint8_t *rowv = s_block + stride;

  for (int sy = row0; sy < row1; sy++) {
    const uint16_t *s = src + (size_t)sy * w;
    build_row_x4(c, s, w, rowu, rowv);

    int64_t t1 = PROF_NOW();
    memcpy(s_block + 2 * stride, s_block, 2 * stride); /* [U V] を [U V U V] に */
    int64_t t2 = PROF_NOW();
    memcpy(out + (size_t)sy * blk, s_block, blk);
    /* 専用経路では prof[1] が internal RAM 内の複製(460,800B/枚)、
     * prof[2] が PSRAM への書き込み(921,600B/枚)。どちらが壁かを見る。 */
    PROF_ADD(c, 1, t2 - t1);
    PROF_ADD(c, 2, PROF_NOW() - t2);
  }
}

/* ---- scale=2 専用経路(2026-10-18 の仕様変更で既定) ----
 * 録画は 480x320。PC 側で ffmpeg の最近傍 2 倍をやって 960x640 にする。
 * 4 倍と違ってデータは 1/4。彩度は 2x2 ブロック = 元画素 1 個なので、
 * 1 元画素が 1 個の彩度と 4 個の輝度を作る(にじまない)。
 * 元行 1 本 → 出力行 2 本 [U 行][V 行]。1 元画素ぶん 3 バイト [C Y Y]。
 * 組み立ても DMA への書き出しも scale=4 と同じ形(元行 4 本ぶんを段に組む)。
 * 出力は汎用経路とバイト単位で一致。test_scale_ref2 が 230,400 バイト全部を
 * 参照実装と突き合わせて担保する。 */
static void build_row_x2(gbac_scale_ctx_t *c, const uint16_t *s, int w,
                         uint8_t *rowu, uint8_t *rowv) {
  int64_t t0 = PROF_NOW();
  uint16_t *tmp = c->srcrow;
  if (w <= GBAC_WIDTH) {
    memcpy(tmp, s, (size_t)w * sizeof(uint16_t));
    s = tmp;
  }
  uint32_t *wu = (uint32_t *)rowu;
  uint32_t *wv = (uint32_t *)rowv;
  int x = 0;
  /* 4 画素で 12 バイト = 語 3 個。x が 4 の倍数のとき語境界に揃う。
   * [C0 Y0 Y0 C1 | Y1 Y1 C2 Y2 | Y2 C3 Y3 Y3] */
#define X2_QUAD(e0, e1, e2, e3)                                            \
  do {                                                                     \
    uint32_t y0 = (e0) & 0xFFu, u0 = ((e0) >> 8) & 0xFFu, v0 = (e0) >> 16; \
    uint32_t y1 = (e1) & 0xFFu, u1 = ((e1) >> 8) & 0xFFu, v1 = (e1) >> 16; \
    uint32_t y2 = (e2) & 0xFFu, u2 = ((e2) >> 8) & 0xFFu, v2 = (e2) >> 16; \
    uint32_t y3 = (e3) & 0xFFu, u3 = ((e3) >> 8) & 0xFFu, v3 = (e3) >> 16; \
    *wu++ = (uint32_t)(u0 | (y0 << 8) | (y0 << 16) | (u1 << 24));         \
    *wu++ = (uint32_t)(y1 | (y1 << 8) | (u2 << 16) | (y2 << 24));         \
    *wu++ = (uint32_t)(y2 | (u3 << 8) | (y3 << 16) | (y3 << 24));         \
    *wv++ = (uint32_t)(v0 | (y0 << 8) | (y0 << 16) | (v1 << 24));         \
    *wv++ = (uint32_t)(y1 | (y1 << 8) | (v2 << 16) | (y2 << 24));         \
    *wv++ = (uint32_t)(y2 | (v3 << 8) | (y3 << 16) | (y3 << 24));         \
  } while (0)
  if (c->raw) {
    /* サンプル直引き(復号込み)。録画の本線はこちら(2026-10-18) */
    for (; x + 4 <= w; x += 4) {
      uint32_t e0 = raw_of(s[x]), e1 = raw_of(s[x + 1]);
      uint32_t e2 = raw_of(s[x + 2]), e3 = raw_of(s[x + 3]);
      X2_QUAD(e0, e1, e2, e3);
    }
  } else {
    for (; x + 4 <= w; x += 4) {
      uint32_t e0 = ouyy_of(s[x]), e1 = ouyy_of(s[x + 1]);
      uint32_t e2 = ouyy_of(s[x + 2]), e3 = ouyy_of(s[x + 3]);
      X2_QUAD(e0, e1, e2, e3);
    }
  }
#undef X2_QUAD
  for (; x < w; x++) { /* 4 の倍数でない残り(実運用では来ない) */
    uint32_t e = c->raw ? raw_of(s[x]) : ouyy_of(s[x]);
    uint8_t y = (uint8_t)(e & 0xFFu), u = (uint8_t)((e >> 8) & 0xFFu), v = (uint8_t)(e >> 16);
    uint8_t *p = rowu + x * 3;
    p[0] = u; p[1] = y; p[2] = y;
    p = rowv + x * 3;
    p[0] = v; p[1] = y; p[2] = y;
  }
  PROF_ADD(c, 0, PROF_NOW() - t0);
}

static void scale_ouyy_x2(gbac_scale_ctx_t *c, const uint16_t *src, int w,
                          int h, uint8_t *out, int row0, int row1) {
  (void)h;
  const int stride = w * 3;  /* 出力行 = (w*2) 画素 * 3 / 2 */
  const int blk = 2 * stride; /* 元行 1 本 = 出力行 2 本 */

  if (!s_tables_ready) gbac_scale_init_lut();

  /* 段を使う経路。段の巡回は呼び出しをまたぐカウンターで回す(scale=4 と同じ約束)。
   * 1 回の flush は元行 4 本(5,760B)か、段が足りれば 8 本(11,520B)。
   * DMA は 1 回あたりの固定費が大きく、r91 実測で 5,760B 刻みは 11,520B 刻みの
   * 半分の速さだった。160 行は 8 でも割れる。 */
  if (c->stage_n >= 2 && c->stage[0] && c->stage[1] && c->flush &&
      c->stage_bytes >= (size_t)(4 * blk) && ((row1 - row0) & 3) == 0) {
    int rows = 4;
    if (c->stage_bytes >= (size_t)(8 * blk) && ((row1 - row0) & 7) == 0) rows = 8;
    const int grp = rows * blk;
    for (int g = row0; g < row1; g += rows) {
      uint8_t *stg = c->stage[c->stage_rot];
      if (++c->stage_rot >= c->stage_n) c->stage_rot = 0;
      for (int k = 0; k < rows; k++) {
        uint8_t *b = stg + (size_t)k * blk;
        build_row_x2(c, src + (size_t)(g + k) * w, w, b, b + stride);
      }
      int64_t t2 = PROF_NOW();
      c->flush(out + (size_t)g * blk, stg, (size_t)grp);
      PROF_ADD(c, 2, PROF_NOW() - t2);
    }
    if (c->finish) c->finish();
    return;
  }

  /* 段を使わない経路。s_block は 5,760 バイトあるので 1 元行ぶん(1,440B)が入る */
  for (int sy = row0; sy < row1; sy++) {
    uint8_t *b = s_block;
    build_row_x2(c, src + (size_t)sy * w, w, b, b + stride);
    int64_t t2 = PROF_NOW();
    memcpy(out + (size_t)sy * blk, b, blk);
    PROF_ADD(c, 2, PROF_NOW() - t2);
  }
}

void gbac_scale_ouyy_rows(gbac_scale_ctx_t *c, const uint16_t *src, int w, int h,
                          int scale, uint8_t *out, int row0, int row1) {
  if (!src || !out || w <= 0 || h <= 0) return;
  /* 内訳カウンタは既定の作業台でも必ず実体へ向ける(ctx_set を呼ばずに
   * gbac_scale_ouyy だけ使う経路がある。2026-10-18 ホストテストが
   * prof NULL でクラッシュした) */
  if (c->prof == NULL) c->prof = gbac_scale_prof;
  if (scale < 1) scale = 1;
  if (scale > GBAC_MAX_SCALE) scale = GBAC_MAX_SCALE;
  if (row0 < 0) row0 = 0;
  if (row1 > h) row1 = h;
  if (row0 >= row1) return;
  /* 直引き表は起動時に作っておく。作り損ねていてもここで作る */
  if (c->raw && !s_raw_ready) gbac_scale_init_raw_lut();

  const int W = w * scale;
  const int H = h * scale;
  if (W > GBAC_YW_MAX || H > GBAC_YH_MAX) return; /* バッファ越えは守る */
  if (scale == 4 && (w & 1) == 0) {
    /* scale=4 は専用経路(上のコメント参照)。w が偶数のときだけ語が揃う。
     * 元行の途中からでも作れる(彩度は 2x2 ブロックが元画素 1 個に潰れる) */
    scale_ouyy_x4(c, src, w, h, out, row0, row1);
    return;
  }
  if (scale == 2 && (w & 3) == 0) {
    /* scale=2 も専用経路。行のまたぎがないので元行の途中から作れる。
     * w が 4 の倍数のときだけ [C Y Y] の語が揃う */
    scale_ouyy_x2(c, src, w, h, out, row0, row1);
    return;
  }
  /* 汎用経路は行のまたぎ(彩度が隣り合う 2 元行を見る)があるので分割しない。
   * 分割してくる呼び出しはアプリ側で scale=4 のときだけ。 */
  if (row0 != 0 || row1 != h) return;

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
      const int sa = row_slot(&s_cache, src, w, a, c->raw);
      const int sb = row_slot(&s_cache, src, w, b, c->raw);
      if (sy != cur_sy) {
        const int slot = row_slot(&s_cache, src, w, sy, c->raw);
        int64_t t1 = PROF_NOW();
        build_yrow(&s_cache, slot, w, scale, s_yrow);
        PROF_ADD(c, 1, PROF_NOW() - t1);
        cur_sy = sy;
      }
      {
        int64_t t2 = PROF_NOW();
        build_uv_rows(s_urow, s_vrow, s_yrow, s_xmap, cw, even,
                      s_cache.r[sa], s_cache.g[sa], s_cache.b[sa], s_cache.r[sb],
                      s_cache.g[sb], s_cache.b[sb]);
        PROF_ADD(c, 2, PROF_NOW() - t2);
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
      PROF_ADD(c, 3, PROF_NOW() - t3);
    }
    r += nrow;
  }
}

/* 既定の作業台で枠全体を作る(テスト・ベンチ・分割しないアプリ) */
void gbac_scale_ouyy(const uint16_t *src, int w, int h, int scale,
                     uint8_t *out) {
  gbac_scale_ouyy_rows(&s_def, src, w, h, scale, out, 0, h);
}

/* src が nano のサンプルそのまま(復号前)のときの拡大。
 * 既定の作業台の raw を立てて同じ経路を通す。出力は復号してから
 * gbac_scale_ouyy したものとバイト単位で同じ(test_scale_raw)。 */
void gbac_scale_ouyy_raw(const uint16_t *src, int w, int h, int scale,
                         uint8_t *out) {
  s_def.raw = 1;
  gbac_scale_ouyy_rows(&s_def, src, w, h, scale, out, 0, h);
  s_def.raw = 0;
}
