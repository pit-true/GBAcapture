/* NVS に保存する設定。docs/recorder-plan-esp32p4.md の NVS キーと同じ名前。 */
#pragma once

typedef struct {
    int scale;         /* 1〜4。既定 4(240x160 → 960x640) */
    int bitrate_kbps;  /* H.264 の目標ビットレート。既定 16000 */
    int split_min;     /* 1 ファイルの長さ(分)。既定 15 */
    int volume_pct;    /* 音声ゲイン %。既定 100 */
} gbac_cfg_t;

void gbac_cfg_default(gbac_cfg_t *c);

/* NVS から読んで既定値とマージする。NVS が無ければ既定値のまま。 */
void gbac_cfg_load(gbac_cfg_t *c);

/* 範囲外は既定値へ戻す。 */
void gbac_cfg_clamp(gbac_cfg_t *c);

void gbac_cfg_save(const gbac_cfg_t *c);
