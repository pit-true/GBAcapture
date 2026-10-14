/* gbacap コンポーネントの単体テスト (TDD)。
 * PC 側 C# (host/gbacap) の挙動を C で再現できていることを確認する。
 * ビルド: recorder/test/run_tests.ps1 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "gbacap.h"

static int g_checks;
static int g_fails;

static void ok(int cond, const char *what, int line) {
  g_checks++;
  if (!cond) {
    g_fails++;
    printf("  FAIL(line %d) %s\n", line, what);
  }
}
#define OK(cond) ok((cond), #cond, __LINE__)
#define OK_EQ(a, b) ok_eq_ll((long long)(a), (long long)(b), #a, #b, __LINE__)
static void ok_eq_ll(long long got, long long want, const char *ea,
                     const char *eb, int line) {
  g_checks++;
  if (got != want) {
    g_fails++;
    printf("  FAIL(line %d) %s = %lld, expected %lld (%s)\n", line, ea, got,
           want, eb);
  }
}

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)(v >> 8);
}

/* ---------- 1. ラインパケット解析 ---------- */
static void test_line_parse(void) {
  uint8_t pkt[GBAC_PACKET_SIZE];
  gbac_line_t ln;
  memset(pkt, 0, sizeof(pkt));
  pkt[0] = GBAC_LINE_MAGIC;
  pkt[1] = 7;
  put16(pkt + 2, 133);
  for (int j = 0; j < GBAC_SAMPLES_PER_LINE; j++) put16(pkt + 4 + j * 2, (uint16_t)(0x1000 + j));

  OK_EQ(gbac_line_parse(pkt, sizeof(pkt), &ln), 1);
  OK_EQ(ln.frame_no, 7);
  OK_EQ(ln.line_no, 133);
  OK_EQ(ln.samples[0], 0x1000);
  OK_EQ(ln.samples[239], 0x10EF);

  pkt[0] = GBAC_AUDIO_MAGIC;
  OK_EQ(gbac_line_parse(pkt, sizeof(pkt), &ln), 0);
  pkt[0] = GBAC_LINE_MAGIC;
  OK_EQ(gbac_line_parse(pkt, GBAC_PACKET_SIZE - 2, &ln), 0);
  OK_EQ(gbac_line_parse(pkt, 4, &ln), 0);
}

/* ---------- 2. 音声パケット解析 ---------- */
static void test_audio_parse(void) {
  uint8_t pkt[GBAC_PACKET_SIZE];
  gbac_audio_t a;
  memset(pkt, 0, sizeof(pkt));
  pkt[0] = GBAC_AUDIO_MAGIC;
  pkt[1] = 5;
  put16(pkt + 2, 48);
  for (int j = 0; j < 48; j++) {
    put16(pkt + 4 + j * 4, (uint16_t)(100 + j));     /* 先頭 = 右 (SO1) */
    put16(pkt + 4 + j * 4 + 2, (uint16_t)(200 + j)); /* 次 = 左 (SO2) */
  }
  OK_EQ(gbac_audio_parse(pkt, sizeof(pkt), &a), 1);
  OK_EQ(a.seq, 5);
  OK_EQ(a.pairs, 48);
  OK_EQ(a.samples[0], 100);
  OK_EQ(a.samples[1], 200);
  OK_EQ(a.samples[95], 247);

  pkt[0] = GBAC_LINE_MAGIC;
  OK_EQ(gbac_audio_parse(pkt, sizeof(pkt), &a), 0);
  pkt[0] = GBAC_AUDIO_MAGIC;
  OK_EQ(gbac_audio_parse(pkt, 400, &a), 0); /* 48ペアに足りない */
}

/* ---------- 3. ピクセル復号 (PixelDecoder.cs と同一) ---------- */
static void test_decode(void) {
  uint8_t rgb[3];
  OK_EQ(gbac_decode_rgb565(0xFF7F), 0xFFFF); /* 白 */
  OK_EQ(gbac_decode_rgb565(0x0447), 0xF800); /* 赤のみ bit0,1,2,6,10 */
  OK_EQ(gbac_decode_rgb565(0xF800), 0x07E0); /* 緑のみ bit11..15 */
  OK_EQ(gbac_decode_rgb565(0x0338), 0x001F); /* 青のみ bit3,4,5,8,9 */
  OK_EQ(gbac_decode_rgb565(0x0000), 0x0000); /* 黒 */
  OK_EQ(gbac_decode_rgb565(0x0400), 0x8000); /* 赤bit4のみ */

  gbac_decode_rgb888(0xFF7F, rgb);
  OK_EQ(rgb[0], 255);
  OK_EQ(rgb[1], 255);
  OK_EQ(rgb[2], 255);
  gbac_decode_rgb888(0x0447, rgb);
  OK_EQ(rgb[0], 255);
  OK_EQ(rgb[1], 0);
  OK_EQ(rgb[2], 0);
  gbac_decode_rgb888(0x0400, rgb); /* 5bit 16 → (16<<3)|(16>>2) = 132 */
  OK_EQ(rgb[0], 132);
  gbac_decode_rgb888(0x0000, rgb);
  OK_EQ(rgb[0], 0);
  OK_EQ(rgb[1], 0);
  OK_EQ(rgb[2], 0);
}

/* ---------- 4. フレーム組立 (FrameAssembler.cs と同一) ---------- */
static void test_asm(void) {
  static uint16_t s[GBAC_SAMPLES_PER_LINE];
  gbac_asm_t a;
  gbac_line_t ln;
  for (int j = 0; j < GBAC_SAMPLES_PER_LINE; j++) s[j] = 0xFF7F;
  ln.samples = s;

  gbac_asm_init(&a);
  OK_EQ(a.dropped_frames, 0);
  OK_EQ(a.completed_frames, 0);

  int completed = 0;
  for (int i = 0; i < GBAC_LINES_PER_FRAME; i++) {
    ln.frame_no = 3;
    ln.line_no = (uint16_t)i;
    if (gbac_asm_push(&a, &ln) == 1) completed++;
  }
  OK_EQ(completed, 1);
  OK_EQ(a.completed_frames, 1);
  OK_EQ(a.present_count, 0); /* 完成でクリア */
  OK_EQ(a.lines[5][7], 0xFFFF);

  /* frame 4 を 10ラインだけ受けてから frame 5 へ → 未完成を破棄 */
  for (int i = 0; i < 10; i++) {
    ln.frame_no = 4;
    ln.line_no = (uint16_t)i;
    OK_EQ(gbac_asm_push(&a, &ln), 0);
  }
  ln.frame_no = 5;
  ln.line_no = 0;
  OK_EQ(gbac_asm_push(&a, &ln), 0);
  OK_EQ(a.dropped_frames, 1);
  OK_EQ(a.present_count, 1);

  /* 範囲外 lineNo */
  ln.line_no = GBAC_LINES_PER_FRAME;
  OK_EQ(gbac_asm_push(&a, &ln), -1);
  ln.line_no = 65535;
  OK_EQ(gbac_asm_push(&a, &ln), -1);

  /* 同じフレーム番号の再送は同じラインへ重なるだけ(後勝ち)。フレーム数は増えない */
  static uint16_t s2[GBAC_SAMPLES_PER_LINE];
  for (int j = 0; j < GBAC_SAMPLES_PER_LINE; j++) s2[j] = 0x0000;
  for (int rep = 0; rep < 2; rep++) {
    for (int i = 0; i < 10; i++) {
      ln.frame_no = 6;
      ln.line_no = (uint16_t)i;
      ln.samples = (rep == 0) ? s : s2;
      OK_EQ(gbac_asm_push(&a, &ln), 0);
    }
  }
  OK_EQ(a.present_count, 10);
  OK_EQ(a.completed_frames, 1);
  OK_EQ(a.dropped_frames, 2); /* frame 5 が未完成のまま破棄された */
  OK_EQ(a.lines[5][7], 0x0000);

  /* frame 6 未完成のまま frame 7 へ → 破棄がもう1増える */
  ln.samples = s;
  ln.frame_no = 7;
  ln.line_no = 0;
  OK_EQ(gbac_asm_push(&a, &ln), 0);
  OK_EQ(a.dropped_frames, 3);
}

/* ---------- 5. 拡大 + O_UYY_E_VYY(HW エンコーダの原生形式) ---------- */
/* 1 行 = W*3/2 バイト。偶数行は U が先頭、奇数行は V が先頭。
 * 並びは [C Y Y C Y Y ...] で C が画素ペア 1 個ぶん。 */
static int ou_y(const uint8_t *o, int w, int row, int col) {
  return o[row * (w * 3 / 2) + (col / 2) * 3 + 1 + (col & 1)];
}
static int ou_u(const uint8_t *o, int w, int cy, int cx) {
  return o[(2 * cy) * (w * 3 / 2) + cx * 3];
}
static int ou_v(const uint8_t *o, int w, int cy, int cx) {
  return o[(2 * cy + 1) * (w * 3 / 2) + cx * 3];
}

static void test_scale(void) {
  /* A: 2x2 白, scale 2 → 4x4。Y=255、U/V=128 */
  static uint16_t src[4];
  static uint8_t out[64 * 32 * 3 / 2];
  for (int i = 0; i < 4; i++) src[i] = 0xFFFF;
  memset(out, 0xAA, sizeof(out));
  gbac_scale_ouyy(src, 2, 2, 2, out);
  for (int row = 0; row < 4; row++)
    for (int col = 0; col < 4; col++) OK_EQ(ou_y(out, 4, row, col), 255);
  for (int cy = 0; cy < 2; cy++)
    for (int cx = 0; cx < 2; cx++) {
      OK_EQ(ou_u(out, 4, cy, cx), 128);
      OK_EQ(ou_v(out, 4, cy, cx), 128);
    }
  /* 行の先頭バイトは偶数行=U、奇数行=V */
  OK_EQ(out[0], 128);
  OK_EQ(out[6], 128);

  /* B: 1x2 (赤, 青), scale 4 → 8x4。最近傍 + chroma 一致 */
  src[0] = 0xF800; /* 赤 */
  src[1] = 0x001F; /* 青 */
  memset(out, 0xAA, sizeof(out));
  gbac_scale_ouyy(src, 2, 1, 4, out);
  for (int row = 0; row < 4; row++)
    for (int col = 0; col < 8; col++)
      OK_EQ(ou_y(out, 8, row, col), col < 4 ? 76 : 28);
  for (int cy = 0; cy < 2; cy++)
    for (int cx = 0; cx < 4; cx++) {
      OK_EQ(ou_u(out, 8, cy, cx), cx < 2 ? 85 : 255);
      OK_EQ(ou_v(out, 8, cy, cx), cx < 2 ? 255 : 107);
    }
  /* 行全体が埋まっている(裏面の 0xAA が残っていない) */
  for (int i = 0; i < 8 * 4 * 3 / 2; i++) OK(out[i] != 0xAA);

  /* C: 黒/白 の画素複製境界が正確 */
  src[0] = 0x0000;
  src[1] = 0xFFFF;
  memset(out, 0xAA, sizeof(out));
  gbac_scale_ouyy(src, 2, 1, 4, out);
  for (int row = 0; row < 4; row++)
    for (int col = 0; col < 8; col++) OK_EQ(ou_y(out, 8, row, col), col < 4 ? 0 : 255);
  /* 黒/白は彩度 0 → U/V は 128 で揃う */
  for (int cx = 0; cx < 4; cx++) {
    OK_EQ(ou_u(out, 8, 0, cx), 128);
    OK_EQ(ou_v(out, 8, 0, cx), 128);
  }

  /* D: 奇数スケール(3)は chroma 4画素平均 → 赤+青はマゼンタ */
  src[0] = 0xF800;
  src[1] = 0x001F;
  memset(out, 0xAA, sizeof(out));
  gbac_scale_ouyy(src, 2, 1, 3, out);
  for (int row = 0; row < 3; row++)
    for (int col = 0; col < 6; col++) OK_EQ(ou_y(out, 6, row, col), col < 3 ? 76 : 28);
  OK_EQ(ou_u(out, 6, 0, 0), 85);
  OK_EQ(ou_v(out, 6, 0, 0), 255);
  OK_EQ(ou_u(out, 6, 0, 1), 171); /* RGB平均 (128,0,128) */
  OK_EQ(ou_v(out, 6, 0, 1), 182);
  OK_EQ(ou_u(out, 6, 0, 2), 255);
  OK_EQ(ou_v(out, 6, 0, 2), 107);
}

/* ---------- 6. 音声加工 (ViewerForm.cs と同一連鎖) ---------- */
/* DC 追従の係数が 0.0005 なので、2048 に収束するには 2万サンプル要る。
 * (0.9995^20000 = 4.5e-5 → dc = 2047.91) */
#define WARM 20000
static void test_audio_fx(void) {
  static uint16_t in[2 * (WARM + 1)];
  static int16_t out[2 * (WARM + 1)]; /* fx_process は 2*pairs 個書く */
  gbac_audio_fx_t fx;

  for (int i = 0; i < WARM; i++) {
    in[i * 2] = 2048;     /* 右 */
    in[i * 2 + 1] = 2048; /* 左 */
  }
  in[WARM * 2] = 2560;     /* 右 +512 */
  in[WARM * 2 + 1] = 1536; /* 左 -512 */

  gbac_audio_fx_init(&fx, 1.0f);
  gbac_audio_fx_process(&fx, in, WARM, out); /* 暖機。出力は使わない */
  gbac_audio_fx_process(&fx, in + WARM * 2, 1, out);
  /* LPF B0=0.6309 → ∓322.9、ゲイン 2 → ∓645。
   * DC 追従は float32 では 2047.76 あたりで停止する(dc への加算が float の
   * 刻み 2.44e-4 を下回るため)。C# も同じ float なので同じ値になる。
   * 出力は [左, 右] */
  OK_EQ(out[0], -645);
  OK_EQ(out[1], 645);

  /* クランプ */
  gbac_audio_fx_init(&fx, 100.0f);
  gbac_audio_fx_process(&fx, in, WARM, out);
  gbac_audio_fx_process(&fx, in + WARM * 2, 1, out);
  OK_EQ(out[0], -32768);
  OK_EQ(out[1], 32767);
}

/* ---------- 7. リサンプル 32802 → 48000 ---------- */
static void test_resamp(void) {
  gbac_resamp_t rs;
  static int16_t in[2 * 48];
  static int16_t out[2 * 200];
  int total = 0;

  gbac_resamp_init(&rs, 32802, 48000);
  OK(rs.step_q16 >= 44780 && rs.step_q16 <= 44790);

  for (int i = 0; i < 48; i++) {
    in[i * 2] = 1000;    /* 左 */
    in[i * 2 + 1] = -1000; /* 右 */
  }
  for (int chunk = 0; chunk < 10; chunk++) {
    int n = gbac_resamp_run(&rs, in, 48, out, 200);
    OK(n > 0);
    for (int i = 0; i < n; i++) {
      OK_EQ(out[i * 2], 1000);     /* 一定信号は補間で変わらない */
      OK_EQ(out[i * 2 + 1], -1000);
    }
    total += n;
  }
  /* 10*48*48000/32802 = 701.8 ペア */
  OK(total >= 700 && total <= 704);
}

/* ---------- 8. PTS ---------- */
static void test_pts(void) {
  gbac_pts_t p;
  uint64_t us = 12345;
  gbac_pts_init(&p);
  OK_EQ(gbac_pts_push(&p, 0, &us), 1);
  OK_EQ(us, 0);
  OK_EQ(gbac_pts_push(&p, 0, &us), 0); /* 同じフレーム番号は無視 */
  OK_EQ(gbac_pts_push(&p, 1, &us), 1);
  OK_EQ(us, 16742);
  OK_EQ(gbac_pts_push(&p, 2, &us), 1);
  OK_EQ(us, 33485);

  OK_EQ(gbac_frame_delta(255, 0), 1);
  OK_EQ(gbac_frame_delta(10, 8), 254);
  OK_EQ(gbac_frame_delta(5, 5), 0);

  /* 255 → 0 のラップでフレームが進む */
  gbac_pts_init(&p);
  OK_EQ(gbac_pts_push(&p, 255, &us), 1);
  OK_EQ(us, 0);
  OK_EQ(gbac_pts_push(&p, 0, &us), 1);
  OK_EQ(us, 16742);
}

/* ---------- 9. ファイル分割 ---------- */
static void test_split(void) {
  gbac_split_t s;
  s.split_us = 900 * 1000000LL;
  s.max_bytes = 0;
  OK_EQ(gbac_split_new_file(&s, 0, 900 * 1000000LL - 1, 0), 0);
  OK_EQ(gbac_split_new_file(&s, 0, 900 * 1000000LL, 0), 1);
  s.max_bytes = 1024;
  OK_EQ(gbac_split_new_file(&s, 0, 1000, 1023), 0);
  OK_EQ(gbac_split_new_file(&s, 0, 1000, 1024), 1);
  s.split_us = 0;
  s.max_bytes = 0;
  OK_EQ(gbac_split_new_file(&s, 0, 1LL << 40, 1LL << 40), 0);
}

#define RUN(f)             \
  do {                     \
    printf("-- %s\n", #f); \
    f();                   \
  } while (0)

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  RUN(test_line_parse);
  RUN(test_audio_parse);
  RUN(test_decode);
  RUN(test_asm);
  RUN(test_scale);
  RUN(test_audio_fx);
  RUN(test_resamp);
  RUN(test_pts);
  RUN(test_split);
  printf("%d checks, %d failures\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
