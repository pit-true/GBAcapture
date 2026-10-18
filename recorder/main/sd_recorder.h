/* microSD マウントと mp4 書き込み。
 * 動画は video_enc.c が作った NAL、音声は audio_pipe.c の PCM を
 * AAC にエンコードして esp_muxer に入れる。ファイル分割は muxer の
 * slice_duration + url_pattern_ex に任せる。 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cfg_nvs.h"
#include "esp_err.h"

esp_err_t gbac_sd_mount(void);
/* TF の結線(スロット番号 / IO 電源の LDO / 電源ゲート GPIO)が回路図で
 * 確定できないので、総当たりで生やす。成功するとマウントした状態で戻る。 */
esp_err_t gbac_sd_probe(void);
void gbac_sd_unmount(void);
int gbac_sd_ready(void);

/* PC に SD を見せる(USB MSC)ための足場。
 * esp_vfs_fat_sdcard_unmount はカードごと deinit するので使わない。
 * detach は FATFS の登録だけ外してカードは初期化済みのまま残す。
 * gbac_sd_card() は detach 後も NULL にならない(MSC 側へ渡す)。
 * sdmmc_card_t は匿名 struct の typedef なので前方宣言できない(ヘッダを呼ぶ)。 */
#include "sdmmc_cmd.h"

sdmmc_card_t *gbac_sd_card(void);
esp_err_t gbac_sd_fat_detach(void);
esp_err_t gbac_sd_fat_attach(void);

/* 録画開始。mp4 は /sd/REC_NNNN.mp4(分割は _p2, _p3 ...) */
esp_err_t gbac_rec_start(const gbac_cfg_t *cfg);
void gbac_rec_stop(void);
int gbac_rec_state(void);          /* 1 = 録画中 */

/* SD の空きがしきい値を割ったら true。mux_task が教えるので、
 * app_main 側が録画を止めて mp4 を正しく閉じる。 */
bool gbac_rec_low_space(void);

/* 最後に読んだ SD の空き(MB)。状態ログ用。0 なら不明。 */
uint64_t gbac_rec_sd_free_mb(void);

/* mux_task が今どこの呼び出しにいるかと、そこに留まってから経過した us。
 * NAL スロットが枯れたとき mux が詰まっているか切り分ける。 */
const char *gbac_rec_mux_where(void);
int64_t gbac_rec_mux_age_us(void);
/* mux タスクの呼び出しと SD 書き込みの統計を 1 行に。hang 中でも読む */
void gbac_rec_mux_stats(char *out, int out_sz);

/* 合成映像で bench_frames 枚だけ録画して HW エンコーダの実測を出す。
 * 完了するまで阻塞する。 */
esp_err_t gbac_rec_bench(const gbac_cfg_t *cfg, int bench_frames);

/* SD に mb MB 書き込んで実測 MB/s を出す。esp_muxer の ram_cache_size を
 * 決める材料と、mux が詰まる原因(SD 側か muxer 側か)の切り分け用。
 * 録画していないときだけ呼ぶこと。完了まで阻塞する。 */
esp_err_t gbac_sd_speedtest(int mb);

/* SD を FAT32 で作り直す(FAT 破損からの復旧)。録画中・MSC 中は断る */
esp_err_t gbac_sd_format(void);

/* FatFs を通さずカードの生セクタへ mb MB 書いて読み戻し比較する。
 * start_mb 地点(データ領域の後ろの方)に書くので、そこにあったファイルは壊れる。 */
esp_err_t gbac_sd_rawtest(int mb, int start_mb);
/* FAT 表を実カードから読む検査 */
esp_err_t gbac_sd_fatscan(void);
/* 生セクタの 1 セクタ単発書き込みをバッファ種類で比べる検査 */
esp_err_t gbac_sd_buf_battery(void);
/* 生セクタ書き込みがどこへ行ったか探す検査 */
esp_err_t gbac_sd_wrprobe(void);

/* いちばん番号の大きい REC_NNNN.mp4 を UART0(1.5Mbps)で PC へ流す。
 * 受け手は recorder/scripts/p4pull.py。録画中は断る。 */
esp_err_t gbac_sd_dump_latest(void);
