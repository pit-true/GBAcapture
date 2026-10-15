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
 * 1フレーム = 1/59.7275 秒 = 16742.72µs。整数で切り捨てる。
 *
 * 映像は EP4/EP1 の 2 本へ分散されるためフレーム番号が 1 つ戻ることがある。
 * 差を (uint8_t)(now-prev) で取ると 1 戻りが 255 になり、そのまま進めると
 * PTS が 4.3 秒飛ぶ(実機 REC_0006.mp4 で 21.3 秒の録画の尺が 535 秒になった。
 * muxer は末尾−先頭の PTS をフレーム数で割って並すので全フレーム間隔が
 * 0.944 秒になった)。なので
 *   差 1..16    = 正常な進行
 *   差 17..128  = フレームごと飛んだ。欠落として受け入れる
 *   差 0        = 同じ番号。進めない
 *   差 129..255 = 後戻り(EP の到着順の前後)。古いフレームなので進めない
 * 進めないときは *pts_us を書かず 0 を返す。呼び出し側は 0 のフレームを
 * 完成扱いにしないこと。 */
int gbac_pts_push(gbac_pts_t *p, uint8_t frame_no, uint64_t *pts_us) {
  if (!p->have) {
    p->have = 1;
    p->frame_index = 0;
    p->last = frame_no;
    *pts_us = 0;
    return 1;
  }

  int d = gbac_frame_delta(p->last, frame_no); /* 0..255 */
  if (d == 0 || d >= 129) {
    return 0;
  }

  p->frame_index += (uint64_t)d;
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
