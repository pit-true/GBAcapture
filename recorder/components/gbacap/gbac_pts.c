/* gbac_pts.c — フレーム番号のラップ処理、PTS、ファイル分割判定 */
#include "gbacap.h"

/* u8 のフレーム番号は 0..255 でラップする。now - prev を 0..255 で返す。 */
int gbac_frame_delta(uint8_t prev, uint8_t now) {
  return (int)(uint8_t)(now - prev);
}

void gbac_pts_init(gbac_pts_t *p) {
  p->have = 0;
  p->last = 0;
  p->frame_index = 0;
}

/* PTS はフレーム番号の進行度から決める(実時間ではない)。
 * 1フレーム = 1/59.7275 秒 = 16742.72µs。整数で切り捨てる。 */
int gbac_pts_push(gbac_pts_t *p, uint8_t frame_no, uint64_t *pts_us) {
  if (p->have && p->last == frame_no) return 0;

  if (!p->have) {
    p->have = 1;
    p->frame_index = 0;
  } else {
    p->frame_index += (uint64_t)gbac_frame_delta(p->last, frame_no);
  }
  p->last = frame_no;

  *pts_us = p->frame_index * 1000000ULL * GBAC_FPS_DEN / GBAC_FPS_NUM;
  return 1;
}

int gbac_split_new_file(const gbac_split_t *s, int64_t file_start_us,
                        int64_t now_us, int64_t file_bytes) {
  if (s->split_us > 0 && now_us - file_start_us >= s->split_us) return 1;
  if (s->max_bytes > 0 && file_bytes >= s->max_bytes) return 1;
  return 0;
}
