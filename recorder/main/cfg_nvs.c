#include "cfg_nvs.h"

#include <stdbool.h>
#include <stdlib.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cfg";

#define NVS_NS        "gbac"
#define KEY_SCALE     "scale"
#define KEY_BITRATE   "bitrate"
#define KEY_SPLIT     "split_min"
#define KEY_VOLUME    "volume"
#define KEY_VER       "ver"
/* 既定は 4 倍(960x640、16Mbps)。2026-10-18 夜に nano v0.34 + 上下 2 コア分割で
 * 4 倍が 59.7fps で録れるようになった(t13)。ver 2 までは 2 倍が既定だったので、
 * NVS の ver が古ければ 1 回だけ 4 倍へ寄せる。2 倍に戻すにはコンソールの '2'。 */
#define CFG_VER       3

void gbac_cfg_default(gbac_cfg_t *c)
{
    c->scale = 4;
    c->bitrate_kbps = 16000;
    c->split_min = 15;
    c->volume_pct = 100;
}

void gbac_cfg_clamp(gbac_cfg_t *c)
{
    if (c->scale < 1 || c->scale > 4) {
        c->scale = 4;
    }
    if (c->bitrate_kbps < 1000 || c->bitrate_kbps > 40000) {
        c->bitrate_kbps = 16000;
    }
    if (c->split_min < 1 || c->split_min > 60) {
        c->split_min = 15;
    }
    if (c->volume_pct < 0 || c->volume_pct > 400) {
        c->volume_pct = 100;
    }
}

/* 起動時に 1 回だけ呼ぶ。消えていたら作り直す(録画設定だけなので許容)。 */
static void nfs_init_once(void)
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS を初期化し直す");
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "NVS 起動失敗(%s)。既定値で動く", esp_err_to_name(e));
    }
}

void gbac_cfg_load(gbac_cfg_t *c)
{
    gbac_cfg_default(c);
    nfs_init_once();

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "設定なし。既定値(scale %d, %dkbps, %d分)",
                 (int)c->scale, (int)c->bitrate_kbps, (int)c->split_min);
        return;
    }
    int32_t ver = 0;
    nvs_get_i32(h, KEY_VER, &ver);
    nvs_get_i32(h, KEY_SCALE, &c->scale);
    nvs_get_i32(h, KEY_BITRATE, &c->bitrate_kbps);
    nvs_get_i32(h, KEY_SPLIT, &c->split_min);
    nvs_get_i32(h, KEY_VOLUME, &c->volume_pct);
    nvs_close(h);

    gbac_cfg_clamp(c);
    if (ver < CFG_VER) {
        ESP_LOGW(TAG, "設定を移行(ver %d → %d)。scale=%d bitrate=%dkbps → 4x 16000kbps",
                 (int)ver, CFG_VER, (int)c->scale, (int)c->bitrate_kbps);
        c->scale = 4;
        c->bitrate_kbps = 16000;
        gbac_cfg_save(c);
    }
    ESP_LOGI(TAG, "設定 scale=%d bitrate=%dkbps split=%dmin vol=%d%%",
             (int)c->scale, (int)c->bitrate_kbps, (int)c->split_min, (int)c->volume_pct);
}

void gbac_cfg_save(const gbac_cfg_t *c)
{
    nfs_init_once();

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS を開けない");
        return;
    }
    nvs_set_i32(h, KEY_SCALE, c->scale);
    nvs_set_i32(h, KEY_BITRATE, c->bitrate_kbps);
    nvs_set_i32(h, KEY_SPLIT, c->split_min);
    nvs_set_i32(h, KEY_VOLUME, c->volume_pct);
    nvs_set_i32(h, KEY_VER, CFG_VER);
    esp_err_t e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "NVS 保存失敗(%s)", esp_err_to_name(e));
    }
}
