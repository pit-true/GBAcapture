/* gbac_asm.c — ラインパケット → 160ラインのフレーム組立
 * ラインは frameNo/lineNo を持つが、映像は EP4/EP1 の 2 本へ分散されるため
 * 到着順は前後する(EP4 のフレーム N と EP1 のフレーム N+1 が交互に来る)。
 * そこでフレーム番号で整列させる:
 *   - 160 本揃った → 完成(1 を返す)
 *   - 今のフレームより新しい番号のラインが来た → 今のフレームは欠けが
 *     あっても完成扱いにして打ち切る(2 を返す。このラインは入れない)
 *   - 今のフレームより古い番号のライン → 捨てる(dropped_lines、-2 を返す)
 * 欠けたラインは lines の前内容が残る(黒で塗り直さない)。 */
#include <string.h>

#include "gbacap.h"

void gbac_asm_init(gbac_asm_t *a) {
  gbac_decode_tables_init(); /* 復号の高速版表(1KB)を準備 */
  a->present_count = 0;
  a->have_frame = 0;
  a->frame_no = 0;
  a->raw = 0;
  a->dropped_frames = 0;
  a->dropped_lines = 0;
  a->completed_frames = 0;
  memset(a->present, 0, sizeof(a->present));
}

void gbac_asm_set_raw(gbac_asm_t *a, int raw) {
  if (a) a->raw = raw ? 1 : 0;
}

/* 次のフレームへ進む。lines の中身は消さない(欠けた行は前の内容のまま使う) */
void gbac_asm_start(gbac_asm_t *a, uint8_t frame_no) {
  a->present_count = 0;
  a->have_frame = 1;
  a->frame_no = frame_no;
  memset(a->present, 0, sizeof(a->present));
}

/* u8 のフレーム番号をまたぎで比較する。a が b より新しければ正になる */
static int frame_diff(uint8_t a, uint8_t b) {
  return (int)(int8_t)(uint8_t)(a - b);
}

int gbac_asm_push(gbac_asm_t *a, const gbac_line_t *line) {
  if (a == 0 || line == 0) return -1;
  if (line->line_no >= GBAC_LINES_PER_FRAME) return -1;

  if (!a->have_frame) {
    a->have_frame = 1;
    a->frame_no = line->frame_no;
  }

  int d = frame_diff(line->frame_no, a->frame_no);
  if (d < 0) {
    a->dropped_lines++;   /* もう古いフレームのライン。捨てて数えるだけ */
    return -2;
  }
  if (d > 0) {
    /* 新しいフレーム番号が来た。今のフレームはここで打ち切り。
     * 欠けていても完成扱いにして、このラインは入れずに返す
     * (呼び出し側はフレームを出してから gbac_asm_start する) */
    if (a->present_count > 0) a->completed_frames++;
    return 2;
  }

  if (!a->present[line->line_no]) {
    a->present[line->line_no] = 1;
    a->present_count++;
  }

  /* 同じラインの再送は後勝ち(同じセルへ重なるだけ) */
  uint16_t *dst = a->lines[line->line_no];
  const uint16_t *src = line->samples;
  if (a->raw) {
    /* 復号は拡大側(直引き表)に任せ、ここは写すだけ(2026-10-18) */
    memcpy(dst, src, sizeof(uint16_t) * GBAC_SAMPLES_PER_LINE);
  } else {
    for (int j = 0; j < GBAC_SAMPLES_PER_LINE; j++) {
      dst[j] = gbac_decode_rgb565_fast(src[j]);
    }
  }

  if (a->present_count != GBAC_LINES_PER_FRAME) return 0;

  a->completed_frames++;
  /* 完成でフレームは閉じる。lines の中身はそのまま(呼び出し側が読み出し、
   * 欠けたラインは前フレームの内容として使える) */
  a->present_count = 0;
  a->have_frame = 0;
  memset(a->present, 0, sizeof(a->present));
  return 1;
}
