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

  /* テーブル版 (gbac_decode_rgb565_fast) が参照実装と全入力で一致すること。
   * 16bit サンプルは出力ビットへ線形(OR)に配線されるので 8bit ずつの表で作れる。*/
  gbac_decode_tables_init();
  int diff = 0;
  for (uint32_t v = 0; v < 0x10000u; v++) {
    if (gbac_decode_rgb565_fast((uint16_t)v) != gbac_decode_rgb565((uint16_t)v)) diff++;
  }
  OK_EQ(diff, 0);
  OK_EQ(gbac_decode_rgb565_fast(0xFF7F), 0xFFFF);
  OK_EQ(gbac_decode_rgb565_fast(0x0447), 0xF800);
  OK_EQ(gbac_decode_rgb565_fast(0xF800), 0x07E0);
  OK_EQ(gbac_decode_rgb565_fast(0x0338), 0x001F);
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
  OK_EQ(a.dropped_lines, 0);
  OK_EQ(a.lines[5][7], 0xFFFF);

  /* frame 4 を 10ラインだけ受けてから frame 5 のラインが来た
   * → frame 4 は「欠けありで完成」扱い(gbac_asm_push は 2 を返す) */
  for (int i = 0; i < 10; i++) {
    ln.frame_no = 4;
    ln.line_no = (uint16_t)i;
    OK_EQ(gbac_asm_push(&a, &ln), 0);
  }
  ln.frame_no = 5;
  ln.line_no = 0;
  OK_EQ(gbac_asm_push(&a, &ln), 2);
  OK_EQ(a.completed_frames, 2);
  /* 呼び出し側は gbac_asm_start で次のフレームへ進む */
  gbac_asm_start(&a, 5);
  OK_EQ(a.present_count, 0);
  OK_EQ(gbac_asm_push(&a, &ln), 0);
  OK_EQ(a.present_count, 1);

  /* 古いフレーム番号のラインは捨てて数えるだけ */
  ln.frame_no = 4;
  ln.line_no = 20;
  OK_EQ(gbac_asm_push(&a, &ln), -2);
  OK_EQ(a.dropped_lines, 1);
  OK_EQ(a.present_count, 1);

  /* 範囲外 lineNo */
  ln.frame_no = 5;
  ln.line_no = GBAC_LINES_PER_FRAME;
  OK_EQ(gbac_asm_push(&a, &ln), -1);
  ln.line_no = 65535;
  OK_EQ(gbac_asm_push(&a, &ln), -1);

  /* 同じフレーム番号の再送は同じラインへ重なるだけ(後勝ち)。フレーム数は増えない */
  static uint16_t s2[GBAC_SAMPLES_PER_LINE];
  for (int j = 0; j < GBAC_SAMPLES_PER_LINE; j++) s2[j] = 0x0000;
  for (int rep = 0; rep < 2; rep++) {
    for (int i = 0; i < 10; i++) {
      ln.frame_no = 5;
      ln.line_no = (uint16_t)i;
      ln.samples = (rep == 0) ? s : s2;
      OK_EQ(gbac_asm_push(&a, &ln), 0);
    }
  }
  OK_EQ(a.present_count, 10);
  OK_EQ(a.completed_frames, 2);
  OK_EQ(a.lines[5][7], 0x0000);

  /* 欠けたラインは前のフレームの内容が残っている(frame 5 の 30 行目は
   * 0xFFFF のまま。0x0000 で埋め直していないことを見る) */
  ln.samples = s;
  OK_EQ(a.lines[30][7], 0xFFFF);

  /* EP が前後して frame 6 のラインが frame 5 の残りに先んじても大丈夫。
   * 6 側が来たら 5 は完成扱い(2 が返る)。5 の残りは古いので捨てられる */
  ln.frame_no = 6;
  ln.line_no = 1;
  OK_EQ(gbac_asm_push(&a, &ln), 2);
  OK_EQ(a.completed_frames, 3);
  gbac_asm_start(&a, 6);
  ln.frame_no = 5;
  ln.line_no = 2;
  OK_EQ(gbac_asm_push(&a, &ln), -2);
  OK_EQ(a.dropped_lines, 2);

  /* フレーム番号は u8。255 → 0 は「新しい」側、0 → 255 は古い側 */
  gbac_asm_start(&a, 255);
  ln.frame_no = 255;
  ln.line_no = 3;
  OK_EQ(gbac_asm_push(&a, &ln), 0);
  ln.frame_no = 0;
  ln.line_no = 4;
  OK_EQ(gbac_asm_push(&a, &ln), 2);
  gbac_asm_start(&a, 0);
  ln.frame_no = 255;
  ln.line_no = 5;
  OK_EQ(gbac_asm_push(&a, &ln), -2);
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

/* ---------- 7b. AAC 1 フレームちょうどの部屋では詰まらない(実機バグ) ----------
 * 実機(r8)では g_pcm を g_in_bytes=4096B(1024 ペア)ちょうどだけ確保し、
 * そこへ audio_pipe_read を呼んでいた。リサンプルの出力は小数の残りが出るため
 * 1023 ペアまで進んだところで部屋が 1 ペアになり、入力個数が 0 に切り捨てられて
 * 打ち切り。以後 4096B に届かず AAC は 1 本きり(音声=1)になった。
 * 部屋を 2 フレーム分にして「1 フレーム分以上溜まったら出す」形にすると回る。 */
static int16_t s_rr_in[2 * 64];
static int16_t s_rr_out[2 * 4096];

/* audio_pipe.c の読み出し方針と同じ形(入力個数だけ新 API を使う) */
static int rr_read(gbac_resamp_t *rs, int max_pairs)
{
  int produced = 0;
  while (produced < max_pairs) {
    int room = max_pairs - produced;
    int n = gbac_resamp_in_pairs(room, rs);
    if (n == 0) {
      break;
    }
    if (n > 64) {
      n = 64;
    }
    int got = gbac_resamp_run(rs, s_rr_in, n, s_rr_out + produced * 2, room);
    if (got == 0) {
      break;
    }
    produced += got;
  }
  return produced;
}

/* 部屋 room を n 個の入力で埋める呼び出しを、部屋を 1 ペアずつ変えて繰り返す。
 * n 個の入力は n*48000/32802 +1 個まで出力するので、n が大きすぎると max_out で
 * 途中で打ち切られ、pos_q16 が span 未満のまま残る。次の呼び出しは新しい入力の
 * 先頭ではなく前の位置から読み始めるので、入力の大半を飛ばす
 * (r9: 入力 20.5 秒分に対し AAC 14.0 秒分 = 3 割欠け)。 */
static int16_t s_tt_in[2 * 1024];
static int16_t s_tt_out[2 * 1024];

static void test_resamp_tail(void)
{
  gbac_resamp_t rs;
  for (int i = 0; i < 1024; i++) {
    s_tt_in[i * 2] = 300;
    s_tt_in[i * 2 + 1] = -300;
  }
  for (int room = 2; room <= 1024; room++) {
    gbac_resamp_init(&rs, 32802, 48000);
    int n = gbac_resamp_in_pairs(room, &rs);
    OK(n > 0);
    int got = gbac_resamp_run(&rs, s_tt_in, n, s_tt_out, room);
    OK(got > 0);
    OK(got <= room);
    /* 入力を最後まで使い切っていなければならない。
     * 必要な出力個数は ceil(span/step)。max_out で途中で切れるとこれに届かず、
     * pos_q16 が span を越えずに残って次の入力の大半を飛ばす。 */
    uint32_t span = ((uint32_t)n) << 16;
    uint32_t need = (span + rs.step_q16 - 1) / rs.step_q16;
    OK(((uint32_t)got) >= need);
  }
}

/* 入力にランプ(ペア i の値 = i)を与えて、補間が入力を飛ばさないことを見る。
 * 飛ばすと出力の値が急に飛ぶ。比も 48000/32802 から外れる。 */
static int16_t s_ns_in[2 * 64];
static int16_t s_ns_out[2 * 1024];

static void test_resamp_no_skip(void)
{
  gbac_resamp_t rs;
  gbac_resamp_init(&rs, 32802, 48000);
  int gi = 0, produced = 0, consumed = 0, maxjump = 0, prev = 0, have_prev = 0;
  for (int c = 0; c < 400; c++) {
    int room = 2 + (c * 37) % 1023;
    int n = gbac_resamp_in_pairs(room, &rs);
    if (n > 64) {
      n = 64; /* audio_pipe_read と同じ上限 */
    }
    OK(n > 0);
    for (int i = 0; i < n; i++) {
      s_ns_in[i * 2] = (int16_t)(gi + i);
      s_ns_in[i * 2 + 1] = (int16_t)(gi + i);
    }
    gi += n;
    consumed += n;
    int got = gbac_resamp_run(&rs, s_ns_in, n, s_ns_out, room);
    OK(got > 0);
    for (int i = 0; i < got; i++) {
      int v = s_ns_out[i * 2];
      if (have_prev && v - prev > maxjump) {
        maxjump = v - prev;
      }
      prev = v;
      have_prev = 1;
      produced++;
    }
  }
  OK(maxjump <= 1); /* 補間なので 1 ペア分以上は飛ばさない */
  OK(produced * 32802 >= consumed * 48000 - 48000);
  OK(produced * 32802 <= consumed * 48000 + 48000);
}

static void test_resamp_room(void)
{
  gbac_resamp_t rs;
  for (int i = 0; i < 64; i++) {
    s_rr_in[i * 2] = 1000;
    s_rr_in[i * 2 + 1] = -1000;
  }

  /* 入力個数: 部屋が小さすぎたら 0(呼びに出るな)を返す */
  gbac_resamp_init(&rs, 32802, 48000);
  OK_EQ(gbac_resamp_in_pairs(0, &rs), 0);
  OK_EQ(gbac_resamp_in_pairs(1, &rs), 0);
  OK_EQ(gbac_resamp_in_pairs(2, &rs), 1);
  OK_EQ(gbac_resamp_in_pairs(1024, &rs), 699);

  /* 部屋 1 フレームちょうど: 1 本も作れない(r8 の症状) */
  gbac_resamp_init(&rs, 32802, 48000);
  int fill = 0, enc = 0, calls = 0;
  for (int it = 0; it < 50; it++) {
    int got = rr_read(&rs, 1024 - fill);
    if (got == 0) {
      break;
    }
    calls++;
    fill += got;
    if (fill >= 1024) {
      enc++;
      fill -= 1024;
    }
  }
  OK_EQ(enc, 0);
  OK_EQ(fill, 1023); /* 1024 ペアに 1 ペア届かない */
  /* 残り 1 ペアを埋める入力個数が 0 なので、次も 0 を返して永久に止まる */
  OK_EQ(rr_read(&rs, 1024 - fill), 0);

  /* 部屋 2 フレーム分: AAC が回り続ける(直し方) */
  gbac_resamp_init(&rs, 32802, 48000);
  fill = 0;
  enc = 0;
  calls = 0;
  for (int it = 0; it < 200; it++) {
    int got = rr_read(&rs, 2048 - fill);
    if (got == 0) {
      break;
    }
    calls++;
    fill += got;
    while (fill >= 1024) {
      enc++;
      fill -= 1024;
    }
  }
  OK_EQ(calls, 200);
  OK(enc >= 8); /* 200 回 × 約 93 ペア ÷ 1024 ≒ 18 本 */
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

/* 8b. PTS の後戻り防止。
 * 映像は EP4/EP1 の 2 本へ分散されるのでフレーム番号が 1 つ戻ることがある。
 * 差を (uint8_t)(now-prev) で取ると 1 戻りが 255 になり、frame_index が
 * 255 進んで PTS が 4.3 秒飛ぶ。実機 REC_0006.mp4 で 21.3 秒の録画の尺が
 * 535 秒になった原因(2026-10-16)。
 * 規則: 差 1..128 は進める(17 以上は欠落として受け入れる)、
 *       差 0 と 129..255(後戻り)は 0 を返し *pts_us を触らない。 */
static void test_pts_back(void) {
  const uint64_t sentinel = 0xDEADBEEFULL;
  gbac_pts_t p;
  uint64_t us;

  for (int d = 1; d <= 255; d++) {
    gbac_pts_init(&p);
    OK_EQ(gbac_pts_push(&p, 7, &us), 1);
    OK_EQ(us, 0);
    us = sentinel;
    int r = gbac_pts_push(&p, (uint8_t)(7 + d), &us);
    uint64_t want = (uint64_t)d * 1000000ULL * GBAC_FPS_DEN / GBAC_FPS_NUM;
    if (d <= 128) {
      OK_EQ(r, 1);
      OK_EQ(us, want);
    } else {
      OK_EQ(r, 0);
      OK_EQ(us, sentinel); /* 後戻りのときは PTS を書かない */
    }
  }

  /* 差 0(同じ番号)も書かない */
  gbac_pts_init(&p);
  OK_EQ(gbac_pts_push(&p, 7, &us), 1);
  us = sentinel;
  OK_EQ(gbac_pts_push(&p, 7, &us), 0);
  OK_EQ(us, sentinel);

  /* 後戻りのあとで正常な進行が復活する */
  gbac_pts_init(&p);
  OK_EQ(gbac_pts_push(&p, 100, &us), 1);
  OK_EQ(us, 0);
  OK_EQ(gbac_pts_push(&p, 99, &us), 0); /* 1 戻り = 差 255 */
  OK_EQ(gbac_pts_push(&p, 101, &us), 1);
  OK_EQ(us, 16742);
  /* 境界: 差 16 は正常、差 17 は欠落(gap)として受け入れる。
   * 受け入れたらフレーム番号が進んだ分だけ PTS も進む(100→117 は 17 段)。
   * 前の push(101) で frame_index は 1 なので、117 では 1+16=17 段。 */
  OK_EQ(gbac_pts_push(&p, 117, &us), 1);
  OK_EQ(us, 17ULL * 1000000ULL * GBAC_FPS_DEN / GBAC_FPS_NUM);
  OK_EQ(gbac_pts_push(&p, 134, &us), 1);
  OK_EQ(us, 34ULL * 1000000ULL * GBAC_FPS_DEN / GBAC_FPS_NUM);
  /* 21 秒分のフレーム番号が進んだとして、PTS は約 21 秒になる */
  OK_EQ(gbac_pts_push(&p, (uint8_t)(134 + 120), &us), 1);
  OK_EQ(us, 154ULL * 1000000ULL * GBAC_FPS_DEN / GBAC_FPS_NUM);
  OK(us < 22 * 1000000ULL);
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

/* ---------- 10. 実解像度 240x160 → 960x640 の OUYY ---------- */
static uint16_t g_src[GBAC_WIDTH * GBAC_HEIGHT];
static uint8_t g_ouyy[GBAC_OUYY_SIZE(GBAC_WIDTH, GBAC_HEIGHT, GBAC_MAX_SCALE)];

/* 白 1 画素を流すと、拡大後は scale x scale の白いブロックだけが 255 で
 * 他は全部 0 のはず。最近傍(画素複製)以外の補間を混ぜていない検査。
 * 書き残し(0xAA)を拾うのも兼ねる。 */
static int block_ok(int sx, int sy, int scale) {
  const int W = GBAC_WIDTH * scale, H = GBAC_HEIGHT * scale;
  long bad = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      int want = (x / scale == sx && y / scale == sy) ? 255 : 0;
      if (ou_y(g_ouyy, W, y, x) != want) bad++;
    }
  for (int i = 0; i < GBAC_OUYY_SIZE(GBAC_WIDTH, GBAC_HEIGHT, scale); i++)
    if (g_ouyy[i] == 0xAA) bad++;
  if (bad)
    printf("  white(%d,%d) scale=%d で %d 箇所が想定と違う\n", sx, sy, scale, (int)bad);
  return bad == 0;
}

static void test_scale_real(void) {
  /* esp_h264 はエンコード後に g_encout へ esp_cache_msync(DIR_M2C) をかける。
   * この方向は UNALIGNED フラグが使えず、アドレスとサイズがキャッシュラインの
   * 倍数でないと ESP_ERR_INVALID_ARG を返して無効化が飛ぶ。P4 の L2 キャッシュ
   * ラインは既定 128B。ラインが 64B でも 128B でも安全なことをここで固定する。
   * 倍数でないと NAL が化けるのにエラーは出ない(silent corruption)。 */
  OK_EQ(GBAC_OUYY_STRIDE(GBAC_WIDTH, 4), 960 * 3 / 2);
  OK_EQ(GBAC_OUYY_SIZE(GBAC_WIDTH, GBAC_HEIGHT, 4), 921600);
  OK_EQ(GBAC_OUYY_SIZE(GBAC_WIDTH, GBAC_HEIGHT, 4) % 128, 0);
  OK_EQ(GBAC_OUYY_SIZE(GBAC_WIDTH, GBAC_HEIGHT, 2) % 128, 0);
  OK_EQ(GBAC_OUYY_SIZE(GBAC_WIDTH, GBAC_HEIGHT, 1) % 128, 0);
  /* 720x480 に落とす運用(64B ラインでも安全) */
  OK_EQ(GBAC_OUYY_SIZE(GBAC_WIDTH, GBAC_HEIGHT, 3) % 64, 0);

  int pos[][2] = {{0, 0}, {239, 159}, {120, 80}, {1, 0}, {0, 1}, {238, 158}};
  for (int i = 0; i < 6; i++) {
    for (int j = 0; j < GBAC_WIDTH * GBAC_HEIGHT; j++) g_src[j] = 0x0000;
    g_src[pos[i][1] * GBAC_WIDTH + pos[i][0]] = 0xFFFF;
    memset(g_ouyy, 0xAA, sizeof(g_ouyy));
    gbac_scale_ouyy(g_src, GBAC_WIDTH, GBAC_HEIGHT, 4, g_ouyy);
    OK(block_ok(pos[i][0], pos[i][1], 4));
  }

  /* scale 1 と 2 でも複製位置がずれていない */
  for (int j = 0; j < GBAC_WIDTH * GBAC_HEIGHT; j++) g_src[j] = 0x0000;
  g_src[100 * GBAC_WIDTH + 77] = 0xFFFF;
  memset(g_ouyy, 0xAA, sizeof(g_ouyy));
  gbac_scale_ouyy(g_src, GBAC_WIDTH, GBAC_HEIGHT, 1, g_ouyy);
  OK(block_ok(77, 100, 1));
  memset(g_ouyy, 0xAA, sizeof(g_ouyy));
  gbac_scale_ouyy(g_src, GBAC_WIDTH, GBAC_HEIGHT, 2, g_ouyy);
  OK(block_ok(77, 100, 2));

  /* 全面赤 0xF800 = RGB(255,0,0) → Y=76、U=85、V=255。
   * 行の先頭バイトが偶数行で U、奇数行で V になっているか(640 行全部見る)。 */
  for (int j = 0; j < GBAC_WIDTH * GBAC_HEIGHT; j++) g_src[j] = 0xF800;
  memset(g_ouyy, 0xAA, sizeof(g_ouyy));
  gbac_scale_ouyy(g_src, GBAC_WIDTH, GBAC_HEIGHT, 4, g_ouyy);
  const int stride = GBAC_OUYY_STRIDE(GBAC_WIDTH, 4);
  int bad_uv = 0;
  for (int row = 0; row < GBAC_HEIGHT * 4; row++)
    if (g_ouyy[row * stride] != ((row & 1) ? 255 : 85)) bad_uv++;
  OK_EQ(bad_uv, 0);
  OK_EQ(ou_y(g_ouyy, 960, 300, 500), 76);
  OK_EQ(ou_u(g_ouyy, 960, 150, 240), 85);
  OK_EQ(ou_v(g_ouyy, 960, 150, 240), 255);
  int left = 0;
  for (int i = 0; i < GBAC_OUYY_SIZE(GBAC_WIDTH, GBAC_HEIGHT, 4); i++)
    if (g_ouyy[i] == 0xAA) left++;
  OK_EQ(left, 0);
}

/* ---------- 10b. scale=4 の全面突き合わせ(参照実装との差分 0) ----------
 * gbac_scale.c のヘッダコメントに書いた仕様(2x2 ブロックの RGB 平均から彩度、
 * Y は最近傍)を素直に実装した参照値と 921,600 バイト全部を突き合わせる。
 * scale=4 専用の速い経路を入れたとき、1 バイトも変わっていないことをこれで担保する。 */
static int ref_clamp(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static int ref_luma(int r, int g, int b) { return (77 * r + 150 * g + 29 * b) >> 8; }
static int ref_cu(int r, int g, int b) {
  return ref_clamp(128 + ((-43 * r - 85 * g + 128 * b + 128) >> 8));
}
static int ref_cv(int r, int g, int b) {
  return ref_clamp(128 + ((128 * r - 107 * g - 21 * b + 128) >> 8));
}

static void test_scale_ref4(void) {
  const int scale = 4, w = GBAC_WIDTH, h = GBAC_HEIGHT;
  const int W = w * scale, H = h * scale, stride = W * 3 / 2;
  static uint8_t ref[GBAC_OUYY_SIZE(GBAC_WIDTH, GBAC_HEIGHT, 4)];

  /* 元画像は擬似ランダム。彩度のクランプ(0/255)も踏ませる */
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint16_t c = (uint16_t)((((x * 5 + y * 3) & 31) << 11) |
                              (((x * 7 + y * 11) & 63) << 5) |
                              ((x * 13 + y * 2) & 31));
      if (((x + y) % 17) == 0) c = 0xFFFF;
      if (((x * 3 + y * 7) % 23) == 0) c = 0x0000;
      if (((x * 5 + y * 11) % 29) == 0) c = 0x001F; /* 青一色。U が 256 を超えて 255 へ */
      g_src[y * w + x] = c;
    }
  }

  for (int r = 0; r < H; r++) {
    uint8_t *row = ref + (size_t)r * stride;
    const int cy = r / 2;
    for (int cx = 0; cx < W / 2; cx++) {
      int R = 0, G = 0, B = 0;
      for (int k = 0; k < 4; k++) {
        int ox = 2 * cx + (k & 1), oy = 2 * cy + (k >> 1);
        uint8_t rgb[3];
        gbac_rgb565_to888(g_src[(oy / scale) * w + (ox / scale)], rgb);
        R += rgb[0];
        G += rgb[1];
        B += rgb[2];
      }
      int ar = (R + 2) >> 2, ag = (G + 2) >> 2, ab = (B + 2) >> 2;
      uint8_t *p = row + cx * 3;
      p[0] = (uint8_t)((r & 1) ? ref_cv(ar, ag, ab) : ref_cu(ar, ag, ab));
      for (int k = 0; k < 2; k++) {
        uint8_t rgb[3];
        gbac_rgb565_to888(g_src[(r / scale) * w + ((2 * cx + k) / scale)], rgb);
        p[1 + k] = (uint8_t)ref_luma(rgb[0], rgb[1], rgb[2]);
      }
    }
  }

  memset(g_ouyy, 0xAA, sizeof(g_ouyy));
  gbac_scale_ouyy(g_src, w, h, scale, g_ouyy);
  int diff = 0;
  for (int i = 0; i < (int)sizeof(ref); i++)
    if (ref[i] != g_ouyy[i]) diff++;
  if (diff) printf("  scale=4 が参照実装と %d バイト違う\n", diff);
  OK_EQ(diff, 0);
}

/* ---------- 10c. scale=2 の全面突き合わせ(480x320 の既定経路) ----------
 * 2026-10-19 の仕様変更で録画は 480x320。PC 側で ffmpeg の最近傍 2 倍をやって
 * 960x640 にする(彩度 1 個 = GBA 1 画素なのでにじまない)。
 * scale=2 の専用経路を入れたので、参照実装・汎用経路と 230,400 バイト全部で
 * 一致することを確かめる。段(DMA の代わり)を通した場合も同じ出力か見る。 */
static uint8_t g_stage2[4][5760] __attribute__((aligned(64)));
static uint8_t *g_st2[4] = {g_stage2[0], g_stage2[1], g_stage2[2], g_stage2[3]};
static void st2_flush(uint8_t *dst, const uint8_t *stg, size_t n) { memcpy(dst, stg, n); }
static void st2_finish(void) {}

static void test_scale_ref2(void) {
  const int scale = 2, w = GBAC_WIDTH, h = GBAC_HEIGHT;
  const int W = w * scale, H = h * scale, stride = W * 3 / 2;
  static uint8_t ref[GBAC_OUYY_SIZE(GBAC_WIDTH, GBAC_HEIGHT, 2)];

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint16_t c = (uint16_t)((((x * 11 + y * 5) & 31) << 11) |
                              (((x * 3 + y * 13) & 63) << 5) |
                              ((x * 7 + y * 17) & 31));
      if (((x + y) % 13) == 0) c = 0xFFFF;
      if (((x * 5 + y * 3) % 19) == 0) c = 0x0000;
      if (((x * 11 + y * 7) % 29) == 0) c = 0x001F; /* U が 256 を超えて 255 へ */
      g_src[y * w + x] = c;
    }
  }

  for (int r = 0; r < H; r++) {
    uint8_t *row = ref + (size_t)r * stride;
    const int cy = r / 2;
    for (int cx = 0; cx < W / 2; cx++) {
      int R = 0, G = 0, B = 0;
      for (int k = 0; k < 4; k++) {
        int ox = 2 * cx + (k & 1), oy = 2 * cy + (k >> 1);
        uint8_t rgb[3];
        gbac_rgb565_to888(g_src[(oy / scale) * w + (ox / scale)], rgb);
        R += rgb[0];
        G += rgb[1];
        B += rgb[2];
      }
      int ar = (R + 2) >> 2, ag = (G + 2) >> 2, ab = (B + 2) >> 2;
      uint8_t *p = row + cx * 3;
      p[0] = (uint8_t)((r & 1) ? ref_cv(ar, ag, ab) : ref_cu(ar, ag, ab));
      for (int k = 0; k < 2; k++) {
        uint8_t rgb[3];
        gbac_rgb565_to888(g_src[(r / scale) * w + ((2 * cx + k) / scale)], rgb);
        p[1 + k] = (uint8_t)ref_luma(rgb[0], rgb[1], rgb[2]);
      }
    }
  }

  int diff = 0;
  memset(g_ouyy, 0xAA, sizeof(g_ouyy));
  gbac_scale_ouyy(g_src, w, h, scale, g_ouyy);
  for (int i = 0; i < (int)sizeof(ref); i++)
    if (ref[i] != g_ouyy[i]) diff++;
  if (diff) printf("  scale=2 が参照実装と %d バイト違う\n", diff);
  OK_EQ(diff, 0);

  /* 段を通す経路(実機では flush が GDMA 発行)。出力は同じでなければならない */
  diff = 0;
  gbac_scale_set_stage(g_st2, 4, 5760, st2_flush, st2_finish);
  memset(g_ouyy, 0xAA, sizeof(g_ouyy));
  gbac_scale_ouyy(g_src, w, h, scale, g_ouyy);
  gbac_scale_set_stage(NULL, 0, 0, NULL, NULL);
  for (int i = 0; i < (int)sizeof(ref); i++)
    if (ref[i] != g_ouyy[i]) diff++;
  if (diff) printf("  scale=2 の段経路が参照実装と %d バイト違う\n", diff);
  OK_EQ(diff, 0);

  /* 元行の上下分割(2 コアで組むときの形)。合わせて全枠と同じ */
  diff = 0;
  static gbac_scale_ctx_t ctx;
  memset(g_ouyy, 0xAA, sizeof(g_ouyy));
  gbac_scale_ouyy_rows(&ctx, g_src, w, h, scale, g_ouyy, 0, h / 2);
  gbac_scale_ouyy_rows(&ctx, g_src, w, h, scale, g_ouyy, h / 2, h);
  for (int i = 0; i < (int)sizeof(ref); i++)
    if (ref[i] != g_ouyy[i]) diff++;
  if (diff) printf("  scale=2 の上下分割が参照実装と %d バイト違う\n", diff);
  OK_EQ(diff, 0);
}

/* ---------- 11. 表引き(Y・U・V)が既存の式と 65536 色すべてで一致 ----------
 * 拡大の 1 画素ぶんの計算を「成分ごとの表の和」に置き換えた(2026-10-19 監督指示)。
 * シフト前の和が同じなら出力は 1 バイトも変わらないはずなので、全色で確かめる。
 * 1 色でも違えば mp4 の画質が変わる。ref_luma / ref_cu / ref_cv は gbac_scale.c の
 * 元の式をここに写したもの。 */
static void test_ouyy65k(void) {
  int bad = 0;
  for (uint32_t c = 0; c < 65536u; c++) {
    uint8_t rgb[3];
    gbac_rgb565_to888((uint16_t)c, rgb);
    uint32_t y = (uint32_t)ref_luma(rgb[0], rgb[1], rgb[2]);
    uint32_t u = (uint32_t)ref_cu(rgb[0], rgb[1], rgb[2]);
    uint32_t v = (uint32_t)ref_cv(rgb[0], rgb[1], rgb[2]);
    uint32_t want = y | (u << 8) | (v << 16);
    uint32_t got = gbac_scale_ouyy_px(c);
    if (got != want) {
      if (bad < 5) printf("  ouyy(%04X) got %06X want %06X\n", c, got, want);
      bad++;
    }
  }
  OK_EQ(bad, 0);
}

/* ---------- 12. mp4 へ渡す pts が ms である前提 ---------- */
/* esp_muxer のヘッダには pts の単位が書かれていない(min_packet_duration だけ
 * が明示的に ms)。公式例は上位の esp_capture 経由で生の API を示さない。
 * そこで映像も音声も ms で揃えている前提を、単調増加と重複なしとして
 * ここで固定する。単位を間違えると A/V 同期が倍率でずれる。 */
/* seq(8bit 循環)の飛び幅 = 失われたパケット数。
 * 重複や逆行は飛びと数えない(0)。r9 で音声 6 秒分がどこへ行ったか測るため。 */
static void test_seq_gap(void)
{
  OK_EQ(gbac_seq_gap(0, 1), 0);
  OK_EQ(gbac_seq_gap(0, 2), 1);
  OK_EQ(gbac_seq_gap(255, 0), 0);
  OK_EQ(gbac_seq_gap(255, 1), 1);
  OK_EQ(gbac_seq_gap(10, 10), 0); /* 重複 */
  OK_EQ(gbac_seq_gap(10, 9), 0);  /* 逆行 */
  OK_EQ(gbac_seq_gap(100, 110), 9);
  OK_EQ(gbac_seq_gap(0, 128), 127);
}

static void test_pts_ms(void) {
  gbac_pts_t p;
  gbac_pts_init(&p);
  uint64_t us = 0;
  uint32_t prev = 0;
  int dup = 0, back = 0;
  for (int i = 1; i <= 600; i++) {
    OK_EQ(gbac_pts_push(&p, (uint8_t)(i & 0xFF), &us), 1);
    uint32_t ms = (uint32_t)(us / 1000);
    if (i > 1) {
      if (ms == prev) dup++;
      if (ms < prev) back++;
    }
    prev = ms;
  }
  OK_EQ(dup, 0);
  OK_EQ(back, 0);
  /* 600 枚 @ 59.7275fps = 10028ms。ベンチの mp4 を mp4check.py で読んだ時の
   * 期待値でもある(mdhd の長さがこれと違えば pts の単位が違う) */
  OK_EQ(prev, 10028);

  /* AAC は 1024 サンプル = 21.33ms。ms に整数化しても重複しない */
  uint32_t aprev = 0;
  int adup = 0;
  for (uint32_t i = 0, smp = 0; i < 3000; i++, smp += 1024) {
    uint32_t ms = (uint32_t)((uint64_t)1000 * smp / 48000);
    if (i && ms <= aprev) adup++;
    aprev = ms;
  }
  OK_EQ(adup, 0);
  OK_EQ(aprev, 63978);
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
  RUN(test_resamp_room);
  RUN(test_resamp_tail);
  RUN(test_resamp_no_skip);
  RUN(test_pts);
  RUN(test_pts_back);
  RUN(test_split);
  RUN(test_scale_real);
  RUN(test_scale_ref4);
  RUN(test_scale_ref2);
  RUN(test_ouyy65k);
  RUN(test_seq_gap);
  RUN(test_pts_ms);
  printf("%d checks, %d failures\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
