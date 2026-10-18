/* 拡大(240x160 のサンプル → 480x320 か 960x640 の O_UYY_E_VYY)+ H.264 HW エンコード。
 * 完成した NAL(1 フレーム分)をキュー経由で mux タスクへ渡す。 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "cfg_nvs.h"

typedef struct {
    uint8_t *buf;      /* プール内のバッファ。使い終わったら gbac_enc_free() */
    int      len;
    uint64_t pts_us;
    bool     key;
} gbac_nal_t;

/* bench_frames > 0 のときは USB を使わず、合成パターンを bench_frames 枚
 * エンコードして止める(HW エンコーダの実測用。docs/recorder-plan-esp32p4.md の手順 2)。 */
esp_err_t gbac_enc_start(const gbac_cfg_t *cfg, int bench_frames);
/* 起動直後の一番最初に 1 回だけ呼ぶ。H.264 HW エンコーダ(internal RAM に
 * 約 69KB の連続ブロックが要る)と GDMA のステージを、heap が荒れる前に
 * 確保しておく。録画のたびに開くと断片化して失敗する(2026-10-19 r99/r100)。 */
esp_err_t gbac_enc_early_init(const gbac_cfg_t *cfg);
void gbac_enc_stop(void);
int gbac_enc_running(void);
/* まだ muxer に入れていない NAL の数。ベンチの終了判定に使う */
int gbac_enc_queued(void);
/* 直前の録画のスケール内訳をログに出す(1 枚あたり µs)。停止ログの続きで使う */
void gbac_enc_prof_report(void);

/* 次のフレームを IDR にする。mp4 のスライスが変わった直後に呼ぶと、
 * 新しいファイルの先頭が IDR になり再生側が迷わない。 */
void gbac_enc_force_idr(void);

/* microSD がなくても HW エンコーダだけ測る。完成 NAL は読み捨てるので
 * SD・muxer 不要。enc_max が 16.6ms を超えると 60fps に載らない。 */
void gbac_enc_bench(const gbac_cfg_t *cfg, int bench_frames);

/* 完成 NAL を 1 個取り出す。使い終わったら必ず gbac_enc_free() */
int gbac_enc_pop(gbac_nal_t *out, TickType_t wait_ticks);
void gbac_enc_free(gbac_nal_t *n);

uint32_t gbac_enc_frames(void);    /* エンコードに成功したフレーム数 */
uint32_t gbac_enc_errors(void);    /* 失敗した回数 */
uint32_t gbac_enc_scale_us_max(void);
uint32_t gbac_enc_enc_us_max(void);
