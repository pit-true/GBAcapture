#include "sd_recorder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "audio_pipe.h"
#include "board.h"
#include "driver/gpio.h"
#include "driver/sdmmc_default_configs.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
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

/* SD の空き容量のしきい値。16Mbps + AAC 128kbps で毎秒約 2.1MB、
 * 15 分のスライス 1 本が約 1.9GB(FAT32 の 1 ファイル 4GB 上限より小さい)。
 * 開始時にこれ未満なら始めず、記録中に切ったら app_main が停止させる。 */
#define SD_START_FREE_MIN (400ull * 1024 * 1024)
#define SD_STOP_FREE_MIN  (250ull * 1024 * 1024)

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
static volatile bool g_low_space;
static volatile uint64_t g_last_free;   /* 最後に読んだ空きバイト数 */

/* FATFS の空きバイト数。取れなければ 0 を返す。
 * 0 は「不明」ではなく「足りない」として扱う。そのまま書き続けると
 * mp4 が途中で切れて再生できなくなるので、止める方向に使う。 */
static uint64_t sd_free_bytes(void)
{
    uint64_t total = 0;
    uint64_t freeb = 0;
    if (esp_vfs_fat_info(SD_BASE, &total, &freeb) != ESP_OK) {
        ESP_LOGW(TAG, "SD の空き容量が読めない");
        freeb = 0;
    }
    g_last_free = freeb;
    return freeb;
}

/* --- TF スロットの結線が回路図から読み切れないので総当たりする -----------
 * 回路図(Waveshare 公式 PDF)から確定できたこと
 *   - TF1 の 6 本は NLGPIO39〜44。P4 の slot0 IOMUX(CLK43/CMD44/D0-3=39〜42)と一致
 *   - P4 の HAL は slot0 を GPIO マトリクス非対応にしている
 *     (components/hal/esp32p4/include/hal/sdmmc_ll.h:
 *      SDMMC_LL_SLOT_SUPPORT_GPIO_MATRIX(0) == 0)
 *     → TF は slot0 の専用ピンに直結。IDF の既定 host.slot=1 だと別スロット
 *   - TF1 の隣に Q1 と R17/R22 があり、R17 側へ NLGPIO45(slot0 の D4)が来る。
 *     Q1 が TF の電源スイッチなら、この GPIO を駆動しないとカードに電気が来ない
 * 分かっていないこと
 *   - SD の IO 電源がオンチップ LDO(VO4)由来か。Waveshare 公式 09_sdmmc は
 *     LDO ID 4 を既定で有効にするし、MicroPython #18984 も同じ
 *     send_op_cond(1) returned 0x107 をこの原因で報告している
 *   - Q1 が High でオンか Low でオンか
 * 's'(とマウント失敗時の自動実行)で下の表を先頭から試し、
 * 最初にカードが見えた設定を覚えておく。 */
typedef struct {
    int slot;       /* SDMMC_HOST_SLOT_0 / SDMMC_HOST_SLOT_1 */
    int width;      /* 1 か 4 */
    int ldo;        /* オンチップ LDO の chan_id。0 = 使わない */
    int gate;       /* 電源ゲート扱いで駆動する GPIO。-1 = 触らない */
    int gate_lvl;   /* gate に出すレベル(0/1) */
    int freq;       /* kHz */
} sd_probe_t;

static const sd_probe_t k_sd_probe[] = {
    { SDMMC_HOST_SLOT_0, 4, 4, GBAC_PIN_SD_PWR, 1, SDMMC_FREQ_HIGHSPEED },
    { SDMMC_HOST_SLOT_0, 4, 4, GBAC_PIN_SD_PWR, 0, SDMMC_FREQ_HIGHSPEED },
    { SDMMC_HOST_SLOT_0, 4, 4, -1,              -1, SDMMC_FREQ_HIGHSPEED },
    { SDMMC_HOST_SLOT_0, 4, 0, GBAC_PIN_SD_PWR, 1, SDMMC_FREQ_HIGHSPEED },
    { SDMMC_HOST_SLOT_0, 4, 0, -1,              -1, SDMMC_FREQ_HIGHSPEED },
    { SDMMC_HOST_SLOT_1, 4, 4, GBAC_PIN_SD_PWR, 1, SDMMC_FREQ_HIGHSPEED },
    { SDMMC_HOST_SLOT_1, 4, 0, -1,              -1, SDMMC_FREQ_HIGHSPEED },  /* 今の構成 */
    { SDMMC_HOST_SLOT_0, 1, 4, GBAC_PIN_SD_PWR, 1, SDMMC_FREQ_HIGHSPEED },
    { SDMMC_HOST_SLOT_0, 4, 3, GBAC_PIN_SD_PWR, 1, SDMMC_FREQ_HIGHSPEED },
    { SDMMC_HOST_SLOT_0, 4, 4, GBAC_PIN_SD_PWR, 1, SDMMC_FREQ_DEFAULT },
};
#define K_SD_PROBE_N ((int)(sizeof(k_sd_probe) / sizeof(k_sd_probe[0])))

static sd_pwr_ctrl_handle_t g_pwr;
static sd_probe_t g_win;
static bool g_win_ok;
/* カードとは通信できたのに FAT が読めなかった設定。
 * FAT32 にフォーマットし直したあと、これを最初に試す。 */
static sd_probe_t g_alive;
static bool g_alive_ok;

static void sd_pwr_release(void)
{
    if (g_pwr) {
        sd_pwr_ctrl_del_on_chip_ldo(g_pwr);
        g_pwr = NULL;
    }
}

/* 電源まわり(LDO と電源ゲート)だけ先に適用する。マウントと診断で共通。 */
static void sd_apply_power(const sd_probe_t *p)
{
    sd_pwr_release();
    if (p->ldo > 0) {
        sd_pwr_ctrl_ldo_config_t lc = { .ldo_chan_id = p->ldo };
        if (sd_pwr_ctrl_new_on_chip_ldo(&lc, &g_pwr) != ESP_OK) {
            ESP_LOGW(TAG, "LDO%d が確保できない", p->ldo);
            g_pwr = NULL;
        }
    }
    /* 電源ゲート(推定 GPIO45)。前の試行で駆動していたかもしれないので
     * 触らない場合も一度 GPIO 解除して floating に戻す。 */
    gpio_reset_pin(GBAC_PIN_SD_PWR);
    if (p->gate >= 0) {
        gpio_set_direction(p->gate, GPIO_MODE_OUTPUT);
        gpio_set_level(p->gate, p->gate_lvl);
    }
    vTaskDelay(pdMS_TO_TICKS(30));   /* カードの VDD が安定するのを待つ */
}

static void sd_slot_setup(const sd_probe_t *p, sdmmc_slot_config_t *out, int width)
{
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = width;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    if (p->slot == SDMMC_HOST_SLOT_1) {
        /* slot1 は GPIO マトリクス経由。ピン番号は回路図どおり(slot0 と同じ) */
        slot.clk = GBAC_PIN_SD_CLK;
        slot.cmd = GBAC_PIN_SD_CMD;
        slot.d0 = GBAC_PIN_SD_D0;
        slot.d1 = GBAC_PIN_SD_D1;
        slot.d2 = GBAC_PIN_SD_D2;
        slot.d3 = GBAC_PIN_SD_D3;
    }
    *out = slot;
}

/* 1 回だけマウントを試みる。成功したらカードを *out_card に返すので、
 * 呼び出し側は esp_vfs_fat_sdcard_unmount するまで保持する。 */
static esp_err_t sd_try_mount(const sd_probe_t *p, sdmmc_card_t **out_card)
{
    sd_apply_power(p);
    esp_vfs_fat_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 64 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = p->slot;
    host.max_freq_khz = p->freq;
    host.pwr_ctrl_handle = g_pwr;
    sdmmc_slot_config_t slot;
    sd_slot_setup(p, &slot, p->width);
    return esp_vfs_fat_sdmmc_mount(SD_BASE, &host, &slot, &mcfg, out_card);
}

static void sd_log_card(void)
{
    /* IDF 5.5 の sdmmc_csd_t は csd_ver/mmc_ver/capacity/sector_size/
     * card_command_class/tr_speed。capacity はセクタ数なので sector_size を掛ける。 */
    ESP_LOGI(TAG, "SD マウント完了 %s クラス=%d 容量=%lluMB", SD_BASE,
             g_card->csd.card_command_class,
             (unsigned long long)g_card->csd.capacity *
                 (unsigned long long)g_card->csd.sector_size / (1024ULL * 1024ULL));
    ESP_LOGI(TAG, "SD 空き=%lluMB",
             (unsigned long long)(sd_free_bytes() / (1024ULL * 1024ULL)));
}

/* カードと通信できたのに FAT が読めないときに、カードの正体を直接出す。
 * esp_vfs_fat_sdmmc_mount は失敗すると host を deinit するので、ここで組み直す。 */
static void sd_diag_card(const sd_probe_t *p)
{
    sd_apply_power(p);
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = p->slot;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;   /* 情報読みだけなので遅くてよい */
    host.pwr_ctrl_handle = g_pwr;
    sdmmc_slot_config_t slot;
    sd_slot_setup(p, &slot, 1);               /* 情報読みは 1bit で十分 */
    /* IDF 5.5 の sdmmc_host_init / deinit は slot を引数に取らない(slot は init_slot 側) */
    if (sdmmc_host_init() != ESP_OK) {
        ESP_LOGW(TAG, "SD 診断: host が初期化できない");
        return;
    }
    if (sdmmc_host_init_slot(host.slot, &slot) != ESP_OK) {
        ESP_LOGW(TAG, "SD 診断: slot%d が初期化できない", host.slot);
        sdmmc_host_deinit();
        return;
    }
    sdmmc_card_t card;
    memset(&card, 0, sizeof(card));
    esp_err_t e = sdmmc_card_init(&host, &card);
    if (e == ESP_OK) {
        unsigned long long mb = (unsigned long long)card.csd.capacity *
                                (unsigned long long)card.csd.sector_size / (1024ULL * 1024ULL);
        ESP_LOGI(TAG, "SD 診断: カードは応答した 名前=%.8s 容量=%lluMB OCR=0x%08x",
                 card.cid.name, mb, (unsigned)card.ocr);
        ESP_LOGE(TAG, "SD 診断: FAT が見つからない。PC で FAT32 にフォーマットしてね(exFAT/NTFS/未フォーマットは読めない)");
    } else {
        ESP_LOGW(TAG, "SD 診断: sdmmc_card_init=%s", esp_err_to_name(e));
    }
    sdmmc_host_deinit_slot(host.slot);
    sdmmc_host_deinit();
}

esp_err_t gbac_sd_mount(void)
{
    if (g_mounted) {
        return ESP_OK;
    }
    sd_probe_t p;
    if (g_win_ok) {
        p = g_win;
    } else if (g_alive_ok) {
        /* 通信はできる。FAT のせいなので、フォーマットし直せば行けるはず */
        p = g_alive;
    } else {
        p.slot = SDMMC_HOST_SLOT_1;
        p.width = 4;
        p.ldo = 0;
        p.gate = GBAC_SD_PWR_GPIO;
        p.gate_lvl = 1;
        p.freq = SDMMC_FREQ_HIGHSPEED;
    }
    esp_err_t e = sd_try_mount(&p, &g_card);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "SD マウント失敗(%s)", esp_err_to_name(e));
        return e;
    }
    g_mounted = true;
    sd_log_card();
    return ESP_OK;
}

/* 成功した設定を覚えて、マウント済みの状態で戻る */
static void sd_adopt(const sd_probe_t *p, sdmmc_card_t *card)
{
    g_win = *p;
    g_win_ok = true;
    g_card = card;
    g_mounted = true;
    ESP_LOGI(TAG, "SD: ここで行けた slot=%d width=%d ldo=%d gate=GPIO%d=%d freq=%dkHz",
             p->slot, p->width, p->ldo, p->gate, p->gate_lvl, p->freq);
    sd_log_card();
}

/* 結線の総当たり。最初に生えた設定を覚えてそのままマウントした状態で戻る。 */
esp_err_t gbac_sd_probe(void)
{
    if (g_mounted) {
        return ESP_OK;
    }
    /* 前に通信できた設定を知っていれば(FAT が読めないだけだった)、それを最初に試す。
     * FAT32 にフォーマットし直してもらえれば、ここでそのまま生える。 */
    if (g_alive_ok) {
        sdmmc_card_t *card = NULL;
        ESP_LOGI(TAG, "SD: 前に通信できた設定で再挑戦 slot=%d width=%d ldo=%d gate=GPIO%d/%d",
                 g_alive.slot, g_alive.width, g_alive.ldo, g_alive.gate, g_alive.gate_lvl);
        if (sd_try_mount(&g_alive, &card) == ESP_OK) {
            sd_adopt(&g_alive, card);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "  -> 再挑戦も失敗");
    }
    for (int i = 0; i < K_SD_PROBE_N; i++) {
        const sd_probe_t *p = &k_sd_probe[i];
        sdmmc_card_t *card = NULL;
        ESP_LOGI(TAG, "SD 総当たり %d/%d slot=%d width=%d ldo=%d gate=GPIO%d/%d freq=%dkHz",
                 i + 1, K_SD_PROBE_N, p->slot, p->width, p->ldo,
                 p->gate, p->gate_lvl, p->freq);
        esp_err_t e = sd_try_mount(p, &card);
        if (e == ESP_OK) {
            sd_adopt(p, card);
            return ESP_OK;
        }
        if (e == ESP_FAIL && !g_alive_ok) {
            /* ESP_FAIL は f_mount の失敗。カードとはやりとりできている */
            g_alive = *p;
            g_alive_ok = true;
        }
        ESP_LOGW(TAG, "  -> %s", esp_err_to_name(e));
    }
    if (g_alive_ok) {
        sd_diag_card(&g_alive);
    } else {
        ESP_LOGE(TAG, "SD 総当たり: 全部タイムアウト。カード未挿入か電源系");
    }
    return ESP_FAIL;
}

void gbac_sd_unmount(void)
{
    if (!g_mounted) {
        return;
    }
    esp_vfs_fat_sdcard_unmount(SD_BASE, g_card);
    g_mounted = false;
    g_card = NULL;
    sd_pwr_release();
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
        /* muxer がいつ切るかはソースが見えないので保証できない。
         * 切った直後のフレームを IDR にしておくと、新しいファイルの先頭が
         * IDR になり、P が混ざっていても再生側が次の IDR まで待たずに済む。 */
        gbac_enc_force_idr();
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
    /* AAC の AudioSpecificConfig は esp_audio_codec 2.5.x が enc_info で返す
     * (AAC-LC 48000Hz stereo なら 0x11 0x90 0x00)。返さない版向けの
     * フォールバックも持っておく。mp4 の esds に入る値。 */
    static const uint8_t k_asc[] = { 0x11, 0x90, 0x00 };
    const void *asc = k_asc;
    int asc_len = (int)sizeof(k_asc);
    esp_audio_enc_info_t ai = { 0 };
    if (esp_audio_enc_get_info(g_aac, &ai) != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "AAC info が取れない。48000Hz/2ch/16bit として書く");
    } else {
        if (ai.sample_rate != AAC_RATE || ai.channel != 2) {
            ESP_LOGW(TAG, "AAC info が想定と違う(sr=%u ch=%u bps=%u br=%u)",
                     (unsigned)ai.sample_rate, (unsigned)ai.channel,
                     (unsigned)ai.bits_per_sample, (unsigned)ai.bitrate);
        }
        if (ai.codec_spec_info && ai.spec_info_len > 0) {
            asc = ai.codec_spec_info;
            asc_len = (int)ai.spec_info_len;
        }
    }
    esp_muxer_audio_stream_info_t au = {
        .codec = ESP_MUXER_ADEC_AAC,
        .channel = 2,
        .bits_per_sample = 16,
        .sample_rate = AAC_RATE,
        .min_packet_duration = 21,
        .codec_spec_info = (void *)asc,
        .spec_info_len = asc_len,
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

/* NAL を 1 本 muxer に入れる。まだ muxer がなければ SPS/PPS を拾ってから開く。 */
static void write_video(const gbac_nal_t *n)
{
    if (!g_muxer) {
        extract_sps_pps(n->buf, n->len);
        if (!g_avcc) {
            ESP_LOGE(TAG, "SPS/PPS が見つからない NAL(%d バイト)", n->len);
            return;
        }
        if (open_muxer() != ESP_OK) {
            g_run = false;
            return;
        }
    }
    uint32_t ms = (uint32_t)(n->pts_us / 1000);
    esp_muxer_video_packet_t vp = {
        .data = n->buf,
        .len = n->len,
        .pts = ms,
        .dts = ms,
        .key_frame = n->key,
    };
    if (esp_muxer_add_video_packet(g_muxer, g_vidx, &vp) == ESP_MUXER_ERR_OK) {
        g_vpkts++;
    } else {
        g_verr++;
        ESP_LOGE(TAG, "video packet 追加失敗");
    }
}

static void mux_task(void *arg)
{
    (void)arg;
    int64_t last_space = esp_timer_get_time();
    while (true) {
        gbac_nal_t n;
        if (gbac_enc_pop(&n, pdMS_TO_TICKS(10))) {
            write_video(&n);
            gbac_enc_free(&n);
        }
        if (g_muxer) {
            pump_audio(false);
        }
        /* 5 秒に 1 回、空き容量を見る。250MB は 16Mbps で約 100 秒分。
         * ここで教えたあと app_main が録画を止めて mp4 を正しく閉じる。 */
        if (esp_timer_get_time() - last_space > 5 * 1000 * 1000) {
            last_space = esp_timer_get_time();
            uint64_t f = sd_free_bytes();
            if (f < SD_STOP_FREE_MIN && !g_low_space) {
                g_low_space = true;
                ESP_LOGE(TAG, "SD の空きが %lluMB まで減った。録画を止める",
                         (unsigned long long)(f / (1024 * 1024)));
            }
        }
        if (!g_run) {
            /* 停止指示。エンコード済みでキューに残った NAL を書き切ってから閉じる。
             * これをしないと mp4 の末尾が 0.1 秒分だけ欠ける。 */
            while (gbac_enc_pop(&n, 0)) {
                write_video(&n);
                gbac_enc_free(&n);
            }
            break;
        }
        if (!gbac_enc_running() && gbac_enc_queued() == 0) {
            /* ベンチはエンコーダが枚数に達して自分で止まる。
             * g_run は立たないので、ここで抜けないと永久に終わらない。 */
            break;
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
    ESP_LOGI(TAG, "muxer 終了 video=%u audio=%u err=%u/%u stack 残り=%uB",
             (unsigned)g_vpkts, (unsigned)g_apkts, (unsigned)g_verr, (unsigned)g_aerr,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
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
    /* esp_audio_codec 2.5.x はコーデックを登録してから開く。
     * esp_audio_enc_register_default() は全コーデックをプリビルドライブラリから
     * 引き込んでサイズになるので、AAC だけ登録する(2 回目以降はそのまま)。 */
    static bool s_reg;
    if (!s_reg) {
        if (esp_aac_enc_register() != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "AAC コーデックの登録に失敗");
            return ESP_FAIL;
        }
        s_reg = true;
    }
    esp_aac_enc_config_t ac = {
        .sample_rate = AAC_RATE,
        .channel = 2, /* ESP_AUDIO_DUAL */
        .bits_per_sample = 16,
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
    uint64_t freeb = sd_free_bytes();
    if (freeb < SD_START_FREE_MIN) {
        ESP_LOGE(TAG, "SD の空きが %lluMB しかない。%dMB 以上空けてから録画して",
                 (unsigned long long)(freeb / (1024 * 1024)),
                 (int)(SD_START_FREE_MIN / (1024 * 1024)));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "SD 空き=%lluMB", (unsigned long long)(freeb / (1024 * 1024)));
    g_low_space = false;
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
    /* AAC 符号化 + muxer + FATFS でスタックを使う。enc と同じく 12KB にしておく */
    xTaskCreatePinnedToCore(mux_task, "mux", 12288, NULL, 4, &g_task, 0);
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
    /* mux_task にループを抜けて mp4 を閉じさせてからエンコーダを壊す。
     * 逆順にすると、mux_task がまだ読む NAL の作り手を先に消してしまう。 */
    g_run = false;
    for (int i = 0; i < 300 && g_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    gbac_enc_stop();
    rec_free();
    ESP_LOGI(TAG, "録画停止 %s.mp4 video=%u audio=%u", g_base,
             (unsigned)g_vpkts, (unsigned)g_apkts);
}

bool gbac_rec_low_space(void)
{
    return g_low_space;
}

uint64_t gbac_rec_sd_free_mb(void)
{
    return g_last_free / (1024ULL * 1024ULL);
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
    /* muxer が閉じ終わったのでエンコーダを壊してよい。
     * 呼ばないと esp_h264 のハンドルが残ったままになり、次の開始が失敗する。 */
    gbac_enc_stop();
    rec_free();
    int64_t us = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "ベンチ %d 枚 %.2f 秒 -> %.1f fps, video=%u err=%u",
             bench_frames, (double)us / 1e6, (double)bench_frames * 1e6 / (double)us,
             (unsigned)g_vpkts, (unsigned)g_verr);
    return ESP_OK;
}
