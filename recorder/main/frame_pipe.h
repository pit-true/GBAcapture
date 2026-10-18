/* USB で受けた 0xA5 行パケットを 1 フレーム(240x160 の RGB565)に組み立てる。
 * バッファは 2 枚。片方を rx タスクが埋めている間に、もう片方を
 * スケール/エンコードタスクが読む。両方埋まっている間にフレームが完成したら、
 * そのフレームは捨てて計数する(dropped_frames)。
 * フレーム番号が前に進んだ時点で、行が欠けていても完成扱いにして出す
 * (欠けた行は前フレームの内容が残る)。 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t frame_pipe_init(void);

/* 0xA5 の行パケットを 1 個渡す。core1 の rx タスクの文脈で呼ぶ。 */
void frame_pipe_push(const uint8_t *pkt, int len);

/* 完成したフレームを 1 枚受け取る。
 * 戻り値 = 240x160 の nano サンプルそのまま(行優先、復号前。2026-10-18)。
 *          拡大側が gbac_scale_ouyy_raw で復号込みに引く。NULL なら無い/待ち時間切れ。
 * 使い終わったら必ず frame_pipe_release() を呼ぶ。 */
const uint16_t *frame_pipe_take(uint64_t *pts_us, TickType_t wait_ticks);
void frame_pipe_release(void);

uint32_t frame_pipe_completed(void);   /* 完成したフレーム数 */
uint32_t frame_pipe_dropped(void);     /* 読み手が空けられず捨てたフレーム */
uint32_t frame_pipe_dropped_lines(void); /* 古いフレーム番号だったので捨てたライン */
void frame_pipe_stats_reset(void);
