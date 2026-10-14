/* gbac_asm.c — ラインパケット → 160ラインのフレーム組立
 * host/gbacap/FrameAssembler.cs と同じ規則。ラインは frameNo/lineNo を持つため
 * 到着順に依存しない(3EP 分散のため順序保証がない)。 */
#include <string.h>

#include "gbacap.h"

void gbac_asm_init(gbac_asm_t *a) {
  a->present_count = 0;
  a->have_frame = 0;
  a->frame_no = 0;
  a->dropped_frames = 0;
  a->completed_frames = 0;
  memset(a->present, 0, sizeof(a->present));
}

static void asm_reset(gbac_asm_t *a) {
  a->present_count = 0;
  a->have_frame = 0;
  memset(a->present, 0, sizeof(a->present));
}

int gbac_asm_push(gbac_asm_t *a, const gbac_line_t *line) {
  if (a == 0 || line == 0) return -1;
  if (line->line_no >= GBAC_LINES_PER_FRAME) return -1;

  /* フレーム番号が変わったら未完成分は捨てる(C# の _lines.Count>0 で DroppedFrames++) */
  if (!a->have_frame || a->frame_no != line->frame_no) {
    if (a->present_count > 0) a->dropped_frames++;
    a->have_frame = 1;
    a->frame_no = line->frame_no;
    a->present_count = 0;
    memset(a->present, 0, sizeof(a->present));
  }

  if (!a->present[line->line_no]) {
    a->present[line->line_no] = 1;
    a->present_count++;
  }

  /* 同じラインの再送は後勝ち(同じセルへ重なるだけ) */
  uint16_t *dst = a->lines[line->line_no];
  for (int j = 0; j < GBAC_SAMPLES_PER_LINE; j++) {
    dst[j] = gbac_decode_rgb565(line->samples[j]);
  }

  if (a->present_count != GBAC_LINES_PER_FRAME) return 0;

  a->completed_frames++;
  asm_reset(a); /* lines の中身はそのまま(呼び出し側が読み出す) */
  return 1;
}
