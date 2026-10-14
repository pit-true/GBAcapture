/* gbacap.h — nanoCH32V305 が送るパケットの解析と、録画に必要な純ロジック。
 * ESP-IDF 非依存。PC 側 C# (host/gbacap) と同じ挙動を C で再現する。
 * 不変条件は docs/capture-logic-invariants.md に従う。 */
#ifndef GBACAP_H
#define GBACAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- パケット共通 (FW v0.33) ---- */
#define GBAC_LINE_MAGIC 0xA5u
#define GBAC_AUDIO_MAGIC 0xA6u
#define GBAC_PACKET_SIZE 484

/* ---- 映像 ---- */
#define GBAC_SAMPLES_PER_LINE 240
#define GBAC_LINES_PER_FRAME 160
#define GBAC_WIDTH 240
#define GBAC_HEIGHT 160
#define GBAC_MAX_SCALE 4

/* 1ラインパケット: [0xA5][frameNo u8][lineNo u16LE][240 x u16LE] */
typedef struct {
  uint8_t frame_no;
  uint16_t line_no;
  const uint16_t *samples; /* GBAC_SAMPLES_PER_LINE 個。pkt 内を指す */
} gbac_line_t;

/* 戻り値: 1=成功, 0=不正パケット(マジック不一致・長さ不足) */
int gbac_line_parse(const uint8_t *pkt, size_t len, gbac_line_t *out);

/* ---- 音声 ----
 * [0xA6][seq u8][pairCount u16LE][0] + pairCount x (u16 右, u16 左) + 0詰め
 * ADC 生 12bit(0..4095)。SO1=右, SO2=左 は pantest ROM で実測確定。 */
typedef struct {
  uint8_t seq;
  uint16_t pairs;
  const uint16_t *samples; /* 2*pairs 個。先頭が右 */
} gbac_audio_t;

int gbac_audio_parse(const uint8_t *pkt, size_t len, gbac_audio_t *out);

/* ---- ピクセル復号 (host/gbacap/PixelDecoder.cs と同一) ----
 * サンプルの bit2=赤bit0(LDR1), bit6=赤bit1(LDR2)。bit7 は未使用。 */
uint16_t gbac_decode_rgb565(uint16_t sample);
void gbac_decode_rgb888(uint16_t sample, uint8_t *rgb /* [3] */);
/* 復号済みの RGB565 を 8bit へ(緑は6bit→8bit) */
void gbac_rgb565_to888(uint16_t c, uint8_t *rgb /* [3] */);

/* ---- フレーム組立 (host/gbacap/FrameAssembler.cs と同一) ----
 * ラインは frameNo/lineNo を持つので到着順に依存しない。
 * 160ライン揃った時点でフレーム完成。途中で見掛けた frameNo が変わったら
 * 未完成フレームを破棄して dropped_frames に数える。 */
typedef struct {
  uint16_t lines[GBAC_LINES_PER_FRAME][GBAC_SAMPLES_PER_LINE]; /* RGB565 */
  uint8_t present[GBAC_LINES_PER_FRAME];
  int present_count;
  int have_frame;
  uint8_t frame_no;
  uint32_t dropped_frames;
  uint32_t completed_frames;
} gbac_asm_t;

void gbac_asm_init(gbac_asm_t *a);
/* 戻り値: 1=フレーム完成(lines に格納済み), 0=まだ未完, -1=lineNo が範囲外 */
int gbac_asm_push(gbac_asm_t *a, const gbac_line_t *line);

/* ---- 整数倍 nearest neighbor 拡大 + O_UYY_E_VYY 変換 ----
 * src: w x h の RGB565。out は W=w*scale x H=h*scale の packed YUV420。
 *   1 行の長さ = W*3/2 バイト。偶数行は [U Y Y U Y Y ...]、奇数行は [V Y Y ...]。
 *   (esp_h264 の HW エンコーダは P4 rev v3 以降この形式しか受け付けない)
 * out の必要サイズ = GBAC_OUYY_SIZE(w, h, scale)。
 * scale は 1..GBAC_MAX_SCALE。拡大は画素複製のみ(補間しない)。
 * chroma は対応する 4 画素の RGB を平均してから変換する。
 * YUV は BT.601 フルレンジ。 */
#define GBAC_OUYY_STRIDE(w, scale) ((w) * (scale) * 3 / 2)
#define GBAC_OUYY_SIZE(w, h, scale) (GBAC_OUYY_STRIDE(w, scale) * (h) * (scale))
void gbac_scale_ouyy(const uint16_t *src, int w, int h, int scale,
                     uint8_t *out);

/* ---- 音声加工 (host/GbaCap.Viewer/ViewerForm.cs と同じ連鎖) ----
 * DCオフセット追従(係数0.0005) → 2次バターワースLPF(fc 13kHz@32.768kHz)
 * → ゲイン(2 * volume) → int16。入力 [右,左] → 出力 [左,右]。 */
typedef struct {
  float dc_l, dc_r;
  float x1_l, x2_l, y1_l, y2_l;
  float x1_r, x2_r, y1_r, y2_r;
  float volume;
} gbac_audio_fx_t;

void gbac_audio_fx_init(gbac_audio_fx_t *fx, float volume);
/* out は 2*pairs 個の int16 を受けられること */
void gbac_audio_fx_process(gbac_audio_fx_t *fx, const uint16_t *in, int pairs,
                           int16_t *out);

/* ---- 音声リサンプル (32802Hz → 48000Hz、線形補間) ---- */
typedef struct {
  uint32_t step_q16; /* 出力1サンプル進む間の入力サンプル数(Q16) */
  uint32_t pos_q16;
  int16_t last_l, last_r;
  int have_last;
} gbac_resamp_t;

void gbac_resamp_init(gbac_resamp_t *rs, int in_rate, int out_rate);
/* in: 2*pairs int16 [左,右]。out に最大 max_out ペア。戻り値: 生成ペア数 */
int gbac_resamp_run(gbac_resamp_t *rs, const int16_t *in, int pairs,
                    int16_t *out, int max_out);

/* ---- PTS ---- */
#define GBAC_FPS_NUM 597275 /* 59.7275fps */
#define GBAC_FPS_DEN 10000

/* u8 のフレーム番号は 0..255 でラップする。now - prev (0..255) */
int gbac_frame_delta(uint8_t prev, uint8_t now);

typedef struct {
  int have;
  uint8_t last;
  uint64_t frame_index;
} gbac_pts_t;

void gbac_pts_init(gbac_pts_t *p);
/* 戻り値: 1=新しいフレーム(pts_us に µs), 0=同じフレーム番号(無視) */
int gbac_pts_push(gbac_pts_t *p, uint8_t frame_no, uint64_t *pts_us);

/* ---- ファイル分割 ---- */
typedef struct {
  int64_t split_us;  /* 0 で時間分割なし */
  int64_t max_bytes; /* 0 でサイズ分割なし */
} gbac_split_t;

/* 1=新しいファイルへ切り替える */
int gbac_split_new_file(const gbac_split_t *s, int64_t file_start_us,
                        int64_t now_us, int64_t file_bytes);

#ifdef __cplusplus
}
#endif
#endif /* GBACAP_H */
