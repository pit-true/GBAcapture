#include "sd_recorder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "audio_pipe.h"
#include "board.h"
#include "driver/sdmmc_default_configs.h"
#include "driver/sdmmc_host.h"
#include "esp_audio_enc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gbacap.h"
#include "esp_aac_enc.h"
#include "impl/mp4_muxer.h"
#include "sdmmc_cmd.h"
#include "video_enc.h"

static const char *TAG = "rec";

#define SD_BASE     "/sd"
#define AAC_RATE    48000
#define AAC_BITRATE 128000

static sdmmc_card_t *g_card;
static bool g_mounted;
static bool g_registered;
static esp_muxer_handle_t g_muxer;
static int g_vidx = -1;
static int g_aidx = -1;
static esp_audio_enc_handle_t g_aac;
static uint8_t *g_avcc;
static int g_avcc_len;
static char g_base[24];
static volatile bool g_run;
static TaskHandle_t g_task;
static int g_split_min = 15;
static int g_w = 960;
static int g_h = 640;
static int g_in_bytes;
static int g_out_bytes;
static int g_pcm_fill;
static int16_t *g_pcm;
static uint8_t *g_aacbuf;
static uint32_t g_audio_samples;
static uint32_t g_vpkts;
static uint32_t g_apkts;
static uint32_t g_verr;
static uint32_t g_aerr;

esp_err_t gbac_sd_mount(void)
{
    if (g_mounted) {
        return ESP_OK;
    }
    esp_vfs_fat_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 64 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_err_t e = esp_vfs_fat_sdmmc_mount(SD_BASE, &host, &slot, &mcfg, &g_card);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "SD マウント失敗(%s)", esp_err_to_name(e));
        return e;
    }
    g_mounted = true;
    ESP_LOGI(TAG, "SD マウント完了 %s クラス=%d 容量=%lluMB", SD_BASE,
             (int)g_card->csd.card_class, (unsigned long long)(g_card->csd.capacity / 2048));
    return ESP_OK;
}

void gbac_sd_unmount(void)
{
    if (!g_mounted) {
        return;
    }
    esp_vfs_fat_sdmmc_unmount();
    g_mounted = false;
    g_card = NULL;
}

int gbac_sd_ready(void)
{
    return g_mounted ? 1 : 0;
}

static int sc_at(const uint8_t *d, int len, int i)
{
    if (i + 4 <= len && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) {
        return 4;
    }
    if (i + 3 <= len && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
        return 3;
    }
    return 0;
}

/* IDR の NAL から SPS(7)と PPS(8)を取り出し、Startcode+SPS+Startcode+PPS に並べる。 */
static void extract_sps_pps(const uint8_t *d, int len)
{
    if (g_avcc) {
        return;
    }
    uint8_t *out = malloc(2048);
    if (!out) {
        return;
    }
    int i = 0;
    int o = 0;
    int got = 0;
    while (i + 4 < len) {
        int sc = sc_at(d, len, i);
        if (!sc) {
            i++;
            continue;
        }
        int p = i + sc;
        int e = len;
        for (int k = p; k + 3 < len; k++) {
            if (sc_at(d, len, k)) {
                e = k;
                break;
            }
        }
        int t = d[p] & 0x1f;
        if ((t == 7 || t == 8) && o + (e - p) + 4 < 2048) {
            out[o++] = 0;
            out[o++] = 0;
            out[o++] = 0;
            out[o++] = 1;
            memcpy(out + o, d + p, (size_t)(e - p));
            o += e - p;
            got |= (t == 7) ? 1 : 2;
        }
        i = e;
    }
    if (got == 3) {
        g_avcc = out;
        g_avcc_len = o;
        ESP_LOGI(TAG, "SPS+PPS = %d バイト", o);
    } else {
        free(out);
    }
}

static int slice_path(esp_muxer_slice_info_t *info, void *ctx)
{
    (void)ctx;
    int n;
    if (info->slice_index == 0) {
        n = snprintf(info->file_path, (size_t)info->len, "%s/%s.mp4", SD_BASE, g_base);
    } else {
        n = snprintf(info->file_path, (size_t)info->len, "%s/%s_p%d.mp4", SD_BASE, g_base,
                     info->slice_index + 1);
    }
    return (n > 0 && n < info->len) ? 0 : -1;
}

static esp_err_t open_muxer(void)
{
    if (!g_registered) {
        if (mp4_muxer_register() != ESP_MUXER_ERR_OK) {
            return ESP_FAIL;
        }
        g_registered = true;
    }
    mp4_muxer_config_t mc = { 0 };
    mc.base_config.muxer_type = ESP_MUXER_TYPE_MP4;
    mc.base_config.slice_duration = (uint32_t)g_split_min * 60000u;
    mc.base_config.url_pattern_ex = slice_path;
    mc.base_config.ram_cache_size = 128 * 1024;
    mc.display_in_order = true;
    g_muxer = esp_muxer_open((esp_muxer_config_t *)&mc, sizeof(mc));
    if (!g_muxer) {
        ESP_LOGE(TAG, "muxer open 失敗");
        return ESP_FAIL;
    }
    esp_muxer_video_stream_info_t vi = {
        .codec = ESP_MUXER_VDEC_H264,
        .width = (uint16_t)g_w,
        .height = (uint16_t)g_h,
        .fps = 60,
        .min_packet_duration = 16,
        .codec_spec_info = g_avcc,
        .spec_info_len = g_avcc_len,
    };
    if (esp_muxer_add_video_stream(g_muxer, &vi, &g_vidx) != ESP_MUXER_ERR_OK) {
        ESP_LOGE(TAG, "video stream 追加失敗");
        goto fail;
    }
    /* esp_audio_codec 2.6.2 の esp_audio_enc_info_t には codec_spec_info が
     * 無いので、AAC の AudioSpecificConfig は自前で組み立てる。
     * AAC-LC(AOT=2) + 48000Hz(freqIndex=3) + stereo(chanCfg=2)
     *   byte0 = (2<<3)|(3>>1) = 0x11
     *   byte1 = ((3&1)<<7)|(2<<3) = 0x90
     *   byte2 = 0x00
     * (44100Hz stereo が 0x12 0x10 になるのと同じ式)。mp4 の esds に入る。 */
    static const uint8_t k_asc[] = { 0x11, 0x90, 0x00 };
    esp_audio_enc_info_t ai = { 0 };
    if (esp_audio_enc_get_info(g_aac, &ai) != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "AAC info が取れない。48000Hz/2ch/16bit として書く");
    } else if (ai.sample_rate != AAC_RATE || ai.channel != 2) {
        ESP_LOGW(TAG, "AAC info が想定と違う(sr=%u ch=%u bps=%u)",
                 (unsigned)ai.sample_rate, (unsigned)ai.channel, (unsigned)ai.bit_per_sample);
    }
    esp_muxer_audio_stream_info_t au = {
        .codec = ESP_MUXER_ADEC_AAC,
        .channel = 2,
        .bits_per_sample = 16,
        .sample_rate = AAC_RATE,
        .min_packet_duration = 21,
        .codec_spec_info = (void *)k_asc,
        .spec_info_len = (int)sizeof(k_asc),
    };
    if (esp_muxer_add_audio_stream(g_muxer, &au, &g_aidx) != ESP_MUXER_ERR_OK) {
        ESP_LOGE(TAG, "audio stream 追加失敗");
        goto fail;
    }
    esp_muxer_meta_t meta = { .k = "title", .v = g_base };
    esp_muxer_meta_arr_t marr = { .meta_arr = &meta, .arr_num = 1 };
    mp4_muxer_set_meta(g_muxer, &marr);
    ESP_LOGI(TAG, "%s.mp4 開始 %dx%d split=%dmin", g_base, g_w, g_h, g_split_min);
    return ESP_OK;
fail:
    esp_muxer_close(g_muxer);
    g_muxer = NULL;
    return ESP_FAIL;
}

static void encode_audio(int bytes)
{
    esp_audio_enc_in_frame_t in = { .buffer = (uint8_t *)g_pcm, .len = (uint32_t)bytes };
    esp_audio_enc_out_frame_t out = { .buffer = g_aacbuf, .len = (uint32_t)g_out_bytes };
    esp_audio_err_t e = esp_audio_enc_process(g_aac, &in, &out);
    if (e == ESP_AUDIO_ERR_OK && out.encoded_bytes > 0 && g_muxer) {
        esp_muxer_audio_packet_t ap = {
            .data = g_aacbuf,
            .len = (int)out.encoded_bytes,
            .pts = (uint32_t)((uint64_t)1000 * g_audio_samples / AAC_RATE),
        };
        if (esp_muxer_add_audio_packet(g_muxer, g_aidx, &ap) == ESP_MUXER_ERR_OK) {
            g_apkts++;
        } else {
            g_aerr++;
        }
    } else {
        g_aerr++;
    }
    g_audio_samples += (uint32_t)(bytes / 4);
    g_pcm_fill -= bytes;
    if (g_pcm_fill > 0) {
        memmove(g_pcm, g_pcm + bytes / 2, (size_t)g_pcm_fill);
    }
}

static void pump_audio(bool flush)
{
    while (g_pcm_fill < g_in_bytes) {
        int room = g_in_bytes - g_pcm_fill;
        int got = audio_pipe_read(g_pcm + g_pcm_fill / 2, room / 4);
        if (got <= 0) {
            break;
        }
        g_pcm_fill += got * 4;
    }
    while (g_pcm_fill >= g_in_bytes) {
        encode_audio(g_in_bytes);
    }
    if (flush && g_pcm_fill > 0) {
        int have = g_pcm_fill;
        memset((uint8_t *)g_pcm + have, 0, (size_t)(g_in_bytes - have));
        encode_audio(g_in_bytes);
    }
}

static void mux_task(void *arg)
{
    (void)arg;
    while (g_run) {
        gbac_nal_t n;
        if (gbac_enc_pop(&n, pdMS_TO_TICKS(10))) {
            if (!g_muxer) {
                extract_sps_pps(n.buf, n.len);
                if (!g_avcc) {
                    ESP_LOGE(TAG, "SPS/PPS が見つからない NAL(%d バイト)", n.len);
                } else if (open_muxer() != ESP_OK) {
                    g_run = false;
                }
            }
            if (g_muxer) {
                uint32_t ms = (uint32_t)(n.pts_us / 1000);
                esp_muxer_video_packet_t vp = {
                    .data = n.buf,
                    .len = n.len,
                    .pts = ms,
                    .dts = ms,
                    .key_frame = n.key,
                };
                if (esp_muxer_add_video_packet(g_muxer, g_vidx, &vp) == ESP_MUXER_ERR_OK) {
                    g_vpkts++;
                } else {
                    g_verr++;
                    ESP_LOGE(TAG, "video packet 追加失敗");
                }
            }
            gbac_enc_free(&n);
        }
        if (g_muxer) {
            pump_audio(false);
        }
    }
    if (g_muxer) {
        pump_audio(true);
        esp_muxer_err_t me = esp_muxer_close(g_muxer);
        if (me != ESP_MUXER_ERR_OK) {
            ESP_LOGE(TAG, "muxer close 失敗(%d)", (int)me);
        }
        g_muxer = NULL;
    }
    ESP_LOGI(TAG, "muxer 終了 video=%u audio=%u err=%u/%u",
             (unsigned)g_vpkts, (unsigned)g_apkts, (unsigned)g_verr, (unsigned)g_aerr);
    g_task = NULL;
    vTaskDelete(NULL);
}

static void next_base(void)
{
    for (int i = 1; i <= 9999; i++) {
        char p[64];
        snprintf(p, sizeof(p), "%s/REC_%04d.mp4", SD_BASE, i);
        struct stat st;
        if (stat(p, &st) != 0) {
            snprintf(g_base, sizeof(g_base), "REC_%04d", i);
            return;
        }
    }
    snprintf(g_base, sizeof(g_base), "REC_9999");
}

static esp_err_t aac_open(void)
{
    /* esp_audio_codec 2.6.2 ではコーデックの登録は不要(組み込み)。
     * 設定のフィールド名は bit_per_sample(bits_per_sample ではない)。 */
    esp_aac_enc_config_t ac = {
        .sample_rate = AAC_RATE,
        .channel = 2,
        .bit_per_sample = 16,
        .bitrate = AAC_BITRATE,
        .adts_used = false,
    };
    esp_audio_enc_config_t cfg = {
        .type = ESP_AUDIO_TYPE_AAC,
        .cfg = &ac,
        .cfg_sz = sizeof(ac),
    };
    if (esp_audio_enc_open(&cfg, &g_aac) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "AAC エンコーダを開けない");
        return ESP_FAIL;
    }
    if (esp_audio_enc_get_frame_size(g_aac, &g_in_bytes, &g_out_bytes) != ESP_AUDIO_ERR_OK) {
        esp_audio_enc_close(g_aac);
        g_aac = NULL;
        return ESP_FAIL;
    }
    g_pcm = malloc((size_t)g_in_bytes + 8);
    g_aacbuf = malloc((size_t)g_out_bytes + 64);
    if (!g_pcm || !g_aacbuf) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "AAC in=%d out=%d バイト", g_in_bytes, g_out_bytes);
    return ESP_OK;
}

static void rec_free(void)
{
    free(g_pcm);
    free(g_aacbuf);
    free(g_avcc);
    g_pcm = NULL;
    g_aacbuf = NULL;
    g_avcc = NULL;
    g_avcc_len = 0;
    if (g_aac) {
        esp_audio_enc_close(g_aac);
        g_aac = NULL;
    }
    g_vidx = -1;
    g_aidx = -1;
}

static esp_err_t rec_start_common(const gbac_cfg_t *cfg, int bench_frames)
{
    if (!g_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (g_run) {
        return ESP_ERR_INVALID_STATE;
    }
    g_split_min = cfg->split_min;
    g_w = GBAC_WIDTH * cfg->scale;
    g_h = GBAC_HEIGHT * cfg->scale;
    next_base();
    audio_pipe_reset();
    g_pcm_fill = 0;
    g_audio_samples = 0;
    g_vpkts = g_apkts = g_verr = g_aerr = 0;
    if (aac_open() != ESP_OK) {
        rec_free();
        return ESP_FAIL;
    }
    esp_err_t e = gbac_enc_start(cfg, bench_frames);
    if (e != ESP_OK) {
        rec_free();
        return e;
    }
    g_run = true;
    xTaskCreatePinnedToCore(mux_task, "mux", 8192, NULL, 4, &g_task, 0);
    return ESP_OK;
}

esp_err_t gbac_rec_start(const gbac_cfg_t *cfg)
{
    return rec_start_common(cfg, 0);
}

void gbac_rec_stop(void)
{
    if (!g_run) {
        return;
    }
    gbac_enc_stop();
    g_run = false;
    for (int i = 0; i < 300 && g_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    rec_free();
    ESP_LOGI(TAG, "録画停止 %s.mp4 video=%u audio=%u", g_base,
             (unsigned)g_vpkts, (unsigned)g_apkts);
}

int gbac_rec_state(void)
{
    return g_run ? 1 : 0;
}

esp_err_t gbac_rec_bench(const gbac_cfg_t *cfg, int bench_frames)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = rec_start_common(cfg, bench_frames);
    if (e != ESP_OK) {
        return e;
    }
    while (g_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    rec_free();
    int64_t us = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "ベンチ %d 枚 %.2f 秒 -> %.1f fps, video=%u err=%u",
             bench_frames, (double)us / 1e6, (double)bench_frames * 1e6 / (double)us,
             (unsigned)g_vpkts, (unsigned)g_verr);
    return ESP_OK;
}
