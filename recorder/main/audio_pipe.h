/* 0xA6 の音声パケット(32.8kHz ステレオ、ADC 生 0..4095)を受け取り、
 * PC 側ビューアと同じ DC 除去 + ローパス + ゲイン → GBAC_AAC_RATE へリサンプルして
 * int16 ステレオ(左,右の順)で取り出せるようにする。
 * 書き込みは USB のクライアントタスク、読み出しはエンコードタスク。 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

/* AAC の標本化周波数。48000 から 32000 へ(2026-10-18)。
 * AAC は 1024 サンプルを 1 枚にするので、毎秒の枚数が 46.9 → 31.3 になり、
 * core1 の約半分を使っていた AAC エンコード(1 枚 11ms)が 2/3 になる。
 * nano の実レートは 32802Hz なので、リサンプルもほぼ 1:1 になって質が落ちない。
 * 32000 は AAC-LC の標準レート(mp4 の esds の周波数 index は 5)。 */
#define GBAC_AAC_RATE 32000

esp_err_t audio_pipe_init(int volume_pct);

/* 0xA6 パケットを 1 個渡す。USB のクライアントタスクの文脈で呼ぶ。 */
void audio_pipe_push(const uint8_t *pkt, int len);

/* GBAC_AAC_RATE のステレオを max_pairs ペアまで取り出す(out は 2*max_pairs 個の int16)。
 * 戻り値 = 実際に取り出したペア数(0 のことがある)。 */
int audio_pipe_read(int16_t *out, int max_pairs);

uint32_t audio_pipe_gaps(void);      /* seq が飛んだ回数(音声の欠落) */
uint32_t audio_pipe_overruns(void);  /* リングが満杯で捨てたパケット数 */
uint32_t audio_pipe_dropped_pairs(void); /* リング溢れで捨てたペア数(=サンプル数/ch) */
/* 入力側(32.8kHz 側)の計測。32,802 ペア/秒 = 683.4 パケット/秒に届いているかを見る。 */
uint32_t audio_pipe_pushed_pkts(void);   /* 解析成功で ring へ書いた+捨てたパケット数 */
uint32_t audio_pipe_pushed_pairs(void);  /* リングに書き込んだ入力ペア数 */
uint32_t audio_pipe_seq_lost(void);      /* seq の飛び幅の合計 = 失われたパケット数 */
void audio_pipe_reset(void);
