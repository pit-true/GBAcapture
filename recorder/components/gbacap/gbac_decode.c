/* gbac_decode.c — 16bitサンプル → RGB 復号 (host/gbacap/PixelDecoder.cs と同一)
 * ビット配置(不変条件 V1〜V8):
 *   赤 5bit = bit2,bit6,bit0,bit1,bit10 (bit2=LDR1=赤bit0, bit6=LDR2=赤bit1)
 *   緑 5bit = bit11..bit15
 *   青 5bit = bit9,bit8,bit5,bit4,bit3
 *   bit7 は未使用。5→8 展開は (v<<3)|(v>>2)。 */
#include "gbacap.h"

static int extract5(uint16_t s, const int *bits) {
  int v = 0;
  for (int i = 0; i < 5; i++) v |= ((s >> bits[i]) & 1u) << i;
  return v;
}

static const int k_red_bits[5] = {2, 6, 0, 1, 10};
static const int k_green_bits[5] = {11, 12, 13, 14, 15};
static const int k_blue_bits[5] = {9, 8, 5, 4, 3};

static int expand5to8(int v) { return (v << 3) | (v >> 2); }

void gbac_decode_rgb888(uint16_t sample, uint8_t *rgb) {
  rgb[0] = (uint8_t)expand5to8(extract5(sample, k_red_bits));
  rgb[1] = (uint8_t)expand5to8(extract5(sample, k_green_bits));
  rgb[2] = (uint8_t)expand5to8(extract5(sample, k_blue_bits));
}

uint16_t gbac_decode_rgb565(uint16_t sample) {
  int r = expand5to8(extract5(sample, k_red_bits));
  int g = expand5to8(extract5(sample, k_green_bits));
  int b = expand5to8(extract5(sample, k_blue_bits));
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* RGB565 → 8bit。緑は 6bit から (v<<2)|(v>>4) で 8bit へ広げる。 */
void gbac_rgb565_to888(uint16_t c, uint8_t *rgb) {
  int r = (c >> 11) & 0x1F;
  int g = (c >> 5) & 0x3F;
  int b = c & 0x1F;
  rgb[0] = (uint8_t)((r << 3) | (r >> 2));
  rgb[1] = (uint8_t)((g << 2) | (g >> 4));
  rgb[2] = (uint8_t)((b << 3) | (b >> 2));
}
