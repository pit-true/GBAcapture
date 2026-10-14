/* microSD マウントと mp4 書き込み。
 * 動画は video_enc.c が作った NAL、音声は audio_pipe.c の PCM を
 * AAC にエンコードして esp_muxer に入れる。ファイル分割は muxer の
 * slice_duration + url_pattern_ex に任せる。 */
#pragma once

#include "cfg_nvs.h"
#include "esp_err.h"

esp_err_t gbac_sd_mount(void);
void gbac_sd_unmount(void);
int gbac_sd_ready(void);

/* 録画開始。mp4 は /sd/REC_NNNN.mp4(分割は _p2, _p3 ...) */
esp_err_t gbac_rec_start(const gbac_cfg_t *cfg);
void gbac_rec_stop(void);
int gbac_rec_state(void);          /* 1 = 録画中 */

/* 合成映像で bench_frames 枚だけ録画して HW エンコーダの実測を出す。
 * 完了するまで阻塞する。 */
esp_err_t gbac_rec_bench(const gbac_cfg_t *cfg, int bench_frames);
