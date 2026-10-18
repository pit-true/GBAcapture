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

/* seq(8bit 循環)の飛び幅 = 失われたパケット数。連続なら 0。
 * 重複や逆行(半周以上戻る)は飛びと数えないので 0。 */
int gbac_seq_gap(uint8_t prev, uint8_t now);

/* ---- ピクセル復号 (host/gbacap/PixelDecoder.cs と同一) ----
 * サンプルの bit2=赤bit0(LDR1), bit6=赤bit1(LDR2)。bit7 は未使用。 */
uint16_t gbac_decode_rgb565(uint16_t sample);
void gbac_decode_rgb888(uint16_t sample, uint8_t *rgb /* [3] */);
/* 高速版。RGB565 出力は入力ビットの OR 合成(線形)なので、下位8bit/上位8bit
 * ずつの寄与を表(計1KB)にしておいて 2 参照 + OR で求められる。
 * 全 65536 入力で gbac_decode_rgb565 と一致することをテストで保証する。
 * 使う前に gbac_decode_tables_init() を呼ぶこと(gbac_asm_init が呼ぶ)。*/
void gbac_decode_tables_init(void);
uint16_t gbac_decode_rgb565_fast(uint16_t sample);
/* 復号済みの RGB565 を 8bit へ(緑は6bit→8bit) */
void gbac_rgb565_to888(uint16_t c, uint8_t *rgb /* [3] */);

/* ---- フレーム組立 ----
 * ラインは frameNo/lineNo を持つが、映像は EP4/EP1 の 2 本へ分散されるため
 * 到着順は前後する(EP4 のフレーム N と EP1 のフレーム N+1 が交互に来る)。
 * フレーム番号で整列させる:
 *   - 160ライン揃ったら完成
 *   - 今のフレームより新しい番号のラインが来たら、今のフレームは欠けが
 *     あっても完成扱いにして打ち切る(欠けた行は lines の前内容が残る)
 *   - 今のフレームより古い番号のラインは捨てて dropped_lines に数える */
typedef struct {
  uint16_t lines[GBAC_LINES_PER_FRAME][GBAC_SAMPLES_PER_LINE]; /* RGB565 */
  uint8_t present[GBAC_LINES_PER_FRAME];
  int present_count;
  int have_frame;
  uint8_t frame_no;
  uint32_t dropped_frames;   /* 枠が足りなくて出せなかったフレーム */
  uint32_t dropped_lines;    /* 古いフレーム番号だったので捨てたライン */
  uint32_t completed_frames;
} gbac_asm_t;

void gbac_asm_init(gbac_asm_t *a);
/* 戻り値:
 *   1 = 160ライン揃ってフレーム完成(lines に格納済み)
 *   0 = まだ未完(このラインは今のフレームへ入った)
 *   2 = このラインは今のフレームより新しい番号。今のフレームはここで
 *       打ち切り(lines を読み出してよい)。このラインはまだ入れていない
 *  -1 = lineNo が範囲外
 *  -2 = このラインは今のフレームより古い。捨てた */
int gbac_asm_push(gbac_asm_t *a, const gbac_line_t *line);
/* 次のフレームへ進む(frame_no だけ合わせ、lines の中身は消さない) */
void gbac_asm_start(gbac_asm_t *a, uint8_t frame_no);

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

/* gbac_scale_ouyy の内訳の実測(マイクロ秒、加算)。
 *   0 = RGB565 展開  1 = Y 行作り  2 = 彩度+パック  3 = 行コピー
 * 呼び出しをまたいで増え続ける。ベンチで枚数を割って 1 枚あたりを出す。
 * ホスト(gcc)では計測しないので常に 0。 */
extern int64_t gbac_scale_prof[4];

/* RGB565 → (Y,U,V) の表を作る(2026-10-19 監督指示)。
 * Y・U・V は r・g・b の 1 次式なので、シフト前の積和を成分ごとに分けて持つ。
 *   Y_raw = TYr[r5] + TYg[g6] + TYb[b5](U_raw, V_raw も同じ形)
 * 個数は 32 + 64 + 32 = 128 を 3 成分ぶん = 384 個の int32 = 1.5KB。
 * static で internal RAM に置くので表引きは PSRAM に触れない。
 * scale=4 の専用経路は 1 画素の計算(展開+luma+彩度 2 つ)を表引き 1 回に替える。
 * 起動時に 1 回呼ぶと充填を録画前に済ませられる。
 * 呼ばなくても最初の gbac_scale_ouyy で遅延初期化する。
 * 表の中身は gbac_rgb565_to888 + luma + chroma_u/chroma_v と 1 バイトも違わない
 * (test_ouyy65k が 65536 色すべてを突き合わせる)。戻り値は常に 0。 */
int gbac_scale_init_lut(void);

/* 1 画素ぶんの (Y,U,V) を byte0:Y byte1:U byte2:V で返す(テストから見る)。 */
uint32_t gbac_scale_ouyy_px(uint32_t rgb565);

/* ---- PSRAM へ書く手をアプリから差し込む(2026-10-18) ----
 * stages に internal RAM のステージ n 個(2..GBAC_STAGE_MAX=4)を渡すと、
 * scale=4 の専用経路は 4 元行ぶん(出力行 16 本 = GBAC_OUYY_STRIDE(w,4)*4*4 バイト)を
 * そこへ組み立て、flush(その 4 行ぶんの出力先, stage, バイト数) を
 * 1 グループ(元行 4 本)ごとに呼ぶ。全行組み終わったら finish() を呼ぶ。
 * ステージは循環。n 個あると同時進行は n-1 個まで。
 * アプリは flush の直前に「残りを n-2 個以下」まで待てば、次の組み立て先
 * (n-1 個前に使ったステージ)の書き出しと衝突しない。
 * NULL を渡すと従来どおり 1 元行ずつ memcpy で PSRAM へ書く。
 * stage_bytes が足りないときも従来経路。単一タスクからの呼び出し前提。 */
typedef void (*gbac_scale_flush_t)(uint8_t *dst, const uint8_t *stage, size_t n);
typedef void (*gbac_scale_finish_t)(void);
void gbac_scale_set_stage(uint8_t *const *stages, int n, size_t stage_bytes,
                          gbac_scale_flush_t flush, gbac_scale_finish_t finish);

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
/* 出力部屋 room_pairs ペアを埋めるのに必要な入力ペア数。
 * 部屋が 1 ペアだと in/out 倍が 0 に切り捨てられて何も作れないので 0 を返す。
 * 0 を返したら呼び出し側は読み出しに出ないこと(届かない待ち方で止まる)。 */
int gbac_resamp_in_pairs(int room_pairs, const gbac_resamp_t *rs);
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
/* 戻り値:
 *   1 = PTS を進めた(*pts_us に µs を書く)
 *   0 = 同じ番号、または後戻り(EP の到着順の前後)。*pts_us は触らない。
 *       呼び出し側はこのフレームを完成扱いにせず捨てること。
 * 差 1..16 は正常、17..128 は欠落として受け入れ、0 と 129..255 は進めない。*/
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
