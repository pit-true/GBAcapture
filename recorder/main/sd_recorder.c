#include "sd_recorder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audio_pipe.h"
#include "board.h"
#include "driver/gpio.h"
#include "driver/sdmmc_default_configs.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp_audio_enc.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "vfs_fat_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
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
/* PC に SD を見せて(MSC)いて、こちらの FATFS 登録だけ外れている状態。
 * カードは初期化済みのまま PC 側が読むので、ここで SD を触ると二重アクセスになる。 */
static bool g_detached;
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
static int g_pcm_cap;           /* g_pcm の容量(バイト)。1 フレーム分以上必要 */
static int g_pcm_fill;
static int16_t *g_pcm;
static uint8_t *g_aacbuf;
static uint32_t g_audio_samples;
static uint32_t g_vpkts;
static uint32_t g_apkts;
static uint32_t g_verr;
static uint32_t g_aerr;
/* 停止ログで mp4 の長さを実時間と比べるため、先頭と末尾の pts(ms)を控える */
static uint32_t g_vpts_first, g_vpts_last;
static uint32_t g_apts_first, g_apts_last;
/* 音声が 1 パケットしか入らない問題を追うためのカウンタ */
static uint32_t g_a_reads;      /* audio_pipe_read を呼んだ回数 */
static uint32_t g_a_pairs;      /* リングから読み出したペア数 */
static uint32_t g_a_enc;        /* AAC を呼んだ回数 */
static uint32_t g_a_zero;       /* AAC が 0 バイトで返した回数 */
static uint32_t g_a_drop;       /* リング溢れで捨てられた音声ペア数 */
/* mux タスクがどこで回っているか切り分けるため、esp_muxer と AAC の
 * 呼び出しごとの回数・最大・合計(us)を数える。停止ログに出す。 */
static uint32_t g_add_v_n, g_add_v_max, g_add_v_sum;
static uint32_t g_add_a_n, g_add_a_max, g_add_a_sum;
static uint32_t g_aac_n, g_aac_max, g_aac_sum;
static uint32_t g_flush_us, g_close_us;
static uint32_t g_nal_dbg;        /* muxer へ渡す NAL の中身を実測ログに出す回数 */
static int g_pts_dbg;           /* 先頭 20 本の pts をログする */
static int g_last_num;          /* 前回使った REC_ の番号(stat が信用できないときも進める) */
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

/* SD のクロックは 20MHz で固定して試す。40MHz では録画中(USB 受信と
 * エンコーダが同時に動いているとき)に書き込みへ EIO が出た。
 * 速度テストだけ通っても録画中に死ぬので、余裕を見る。 */
static const sd_probe_t k_sd_probe[] = {
    { SDMMC_HOST_SLOT_0, 4, 4, GBAC_PIN_SD_PWR, 1, SDMMC_FREQ_DEFAULT },
    { SDMMC_HOST_SLOT_0, 4, 4, GBAC_PIN_SD_PWR, 0, SDMMC_FREQ_DEFAULT },
    { SDMMC_HOST_SLOT_0, 4, 4, -1,              -1, SDMMC_FREQ_DEFAULT },
    { SDMMC_HOST_SLOT_0, 4, 0, GBAC_PIN_SD_PWR, 1, SDMMC_FREQ_DEFAULT },
    { SDMMC_HOST_SLOT_0, 4, 0, -1,              -1, SDMMC_FREQ_DEFAULT },
    { SDMMC_HOST_SLOT_1, 4, 4, GBAC_PIN_SD_PWR, 1, SDMMC_FREQ_DEFAULT },
    { SDMMC_HOST_SLOT_1, 4, 0, -1,              -1, SDMMC_FREQ_DEFAULT },  /* 今の構成 */
    { SDMMC_HOST_SLOT_0, 1, 4, GBAC_PIN_SD_PWR, 1, SDMMC_FREQ_DEFAULT },
    { SDMMC_HOST_SLOT_0, 4, 3, GBAC_PIN_SD_PWR, 1, SDMMC_FREQ_DEFAULT },
    { SDMMC_HOST_SLOT_0, 4, 4, GBAC_PIN_SD_PWR, 1, SDMMC_FREQ_HIGHSPEED },
    /* 信号品質が怪しいときの確認。遅くして読めるなら配線・耐量の問題 */
    { SDMMC_HOST_SLOT_0, 1, 4, GBAC_PIN_SD_PWR, 1, 4000 },
    { SDMMC_HOST_SLOT_0, 1, 4, GBAC_PIN_SD_PWR, 1, 1000 },
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

/* セクタを 1 個読んで、先頭 16 バイトの hex と署名を出す。
 * 信号が怪しければ中身はランダム、exFAT/NTFS/GPT なら署名がちゃんと見える。
 * 戻り値は MBR の第 1 分区の開始 LBA(分区がなければ 0)。 */
static unsigned sd_dump_sect(sdmmc_card_t *card, unsigned lba)
{
    uint8_t b[512];
    if (sdmmc_read_sectors(card, b, lba, 1) != ESP_OK) {
        ESP_LOGE(TAG, "SD 診断: LBA%u が読めない", lba);
        return 0;
    }
    char hex[3 * 16 + 1];
    int k = 0;
    for (int i = 0; i < 16; i++) k += snprintf(hex + k, sizeof(hex) - k, "%02x ", b[i]);
    ESP_LOGI(TAG, "SD診断 LBA%u 先16B: %s", lba, hex);
    /* 署名らしき文字列を拾う */
    static const char *kSig[] = { "EXFAT   ", "NTFS    ", "FAT32   ", "FAT16   ",
                                  "FAT12   ", "EFI PART", "MBR     " };
    for (unsigned i = 0; i < sizeof(kSig) / sizeof(kSig[0]); i++) {
        for (int off = 0; off + 8 <= 512; off++) {
            if (memcmp(b + off, kSig[i], 8) == 0) {
                ESP_LOGI(TAG, "SD診断 LBA%u: \"%s\" を offset 0x%x で発見", lba, kSig[i], off);
                break;
            }
        }
    }
    const unsigned start = b[0x1c6] | (b[0x1c7] << 8) | (b[0x1c8] << 16) | (b[0x1c9] << 24);
    ESP_LOGI(TAG, "SD診断 LBA%u: 末尾=0x%02x%02x 分区種別(0x1c2)=0x%02x 開始LBA(0x1c6)=%u",
             lba, b[510], b[511], b[0x1c2], start);
    return (b[510] == 0x55 && b[511] == 0xaa) ? start : 0;
}

static void sd_diag_card(const sd_probe_t *p)
{
    sd_apply_power(p);
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = p->slot;
    host.max_freq_khz = 4000;               /* 情報読みは低速で確実に */
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
    esp_err_t e = ESP_FAIL;
    for (int attempt = 0; attempt < 4 && e != ESP_OK; attempt++) {
        memset(&card, 0, sizeof(card));
        e = sdmmc_card_init(&host, &card);
    }
    if (e == ESP_OK) {
        unsigned long long mb = (unsigned long long)card.csd.capacity *
                                (unsigned long long)card.csd.sector_size / (1024ULL * 1024ULL);
        ESP_LOGI(TAG, "SD 診断: カードは応答した 名前=%.8s 容量=%lluMB OCR=0x%08x",
                 card.cid.name, mb, (unsigned)card.ocr);
        /* FAT がない原因を切り分ける。MBR → 分区の開始セクタの順に読む。 */
        unsigned p1 = sd_dump_sect(&card, 0);
        sd_dump_sect(&card, 1);
        if (p1 > 1 && p1 < 0x100000u) sd_dump_sect(&card, p1);
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
    if (g_detached) {
        ESP_LOGE(TAG, "PC に SD を見せている最中。SD を再初期化しない('m' で戻す)");
        return ESP_ERR_INVALID_STATE;
    }
    sd_probe_t p;
    if (g_win_ok) {
        p = g_win;
    } else if (g_alive_ok) {
        /* 通信はできる。FAT のせいなので、フォーマットし直せば行けるはず */
        p = g_alive;
    } else {
        /* 実機で確定した結線(2026-10-15)。総当たりしなくてもこれで行ける。
         *   slot0(IOMUX: CLK43/CMD44/D0-3=39〜42)・4bit・IO 電源はオンチップ LDO VO4
         *   ・GPIO45 は駆動しない(LOW にしても構わないが触らないのが実測の勝ち筋)
         *   ・20MHz(SDMMC_FREQ_DEFAULT。40MHz だと録画中に SD 書き込みが EIO になった
         *     実測 2026-10-18。必要なのは約 2MB/s で 20MHz・4bit で十分) */
        p.slot = SDMMC_HOST_SLOT_0;
        p.width = 4;
        p.ldo = 4;
        p.gate = GBAC_PIN_SD_PWR;
        p.gate_lvl = 0;
        p.freq = SDMMC_FREQ_DEFAULT;
    }
    /* 起動直後はカードの電源が立ち上がりかけで、1 回目だけ FAT が読めない
     * ことがある(実測 2026-10-17: 起動時は f_mount=FR_NO_FILESYSTEM、
     * 's' の再試行では同じ設定で読めた)。電源を入れ直して 3 回まで試す。 */
    esp_err_t e = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        e = sd_try_mount(&p, &g_card);
        if (e == ESP_OK) {
            break;
        }
        g_card = NULL;
        ESP_LOGW(TAG, "SD マウント失敗(%s) %d/3 回。電源を入れ直して再試行",
                 esp_err_to_name(e), attempt);
        sd_pwr_release();
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "既定の結線で生えないので総当たりする");
        return gbac_sd_probe();
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
    if (g_detached) {
        ESP_LOGE(TAG, "PC に SD を見せている最中。SD を再初期化しない('m' で戻す)");
        return ESP_ERR_INVALID_STATE;
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

/* ---- PC へ SD を見せる(MSC)ときの引き渡し ----------------------------
 * esp_vfs_fat_sdcard_unmount は SD カードそのものを deinit してしまうので、
 * ここでは FATFS の登録だけを外してカードは初期化済みのまま残す。
 * esp_tinyusb の MSC ストレージ(tinyusb_msc_storage_init_sdmmc)は
 * 初期化済みの sdmmc_card_t をそのまま読むので、これで引き継げる。 */
sdmmc_card_t *gbac_sd_card(void)
{
    return g_card;
}

esp_err_t gbac_sd_fat_detach(void)
{
    if (!g_mounted || g_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    BYTE pdrv = ff_diskio_get_pdrv_card(g_card);
    if (pdrv == 0xff) {
        ESP_LOGE(TAG, "FAT 切り離し: このカードのドライブが見つからない");
        return ESP_ERR_INVALID_STATE;
    }
    char drv[3] = { (char)('0' + pdrv), ':', 0 };
    esp_vfs_fat_unregister_path(SD_BASE);
    f_mount(NULL, drv, 0);
    ff_diskio_unregister(pdrv);
    g_mounted = false;
    g_detached = true;
    ESP_LOGI(TAG, "SD: FATFS の登録だけ外した(カードは初期化済みのまま pdrv=%c)", drv[0]);
    return ESP_OK;
}

esp_err_t gbac_sd_fat_attach(void)
{
    if (g_mounted) {
        return ESP_OK;
    }
    if (g_card == NULL) {
        ESP_LOGE(TAG, "FAT 再接続: カードが初期化されていない");
        return ESP_ERR_INVALID_STATE;
    }
    BYTE pdrv = 0xff;
    if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == 0xff) {
        ESP_LOGE(TAG, "FATFS のドライブが空いていない");
        return ESP_FAIL;
    }
    ff_diskio_register_sdmmc(pdrv, g_card);
    ff_sdmmc_set_disk_status_check(pdrv, false);
    char drv[3] = { (char)('0' + pdrv), ':', 0 };
    FATFS *fs = NULL;
    esp_err_t e = esp_vfs_fat_register(SD_BASE, drv, 4, &fs);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_fat_register %s", esp_err_to_name(e));
        ff_diskio_unregister(pdrv);
        return e;
    }
    FRESULT fr = f_mount(fs, drv, 1);
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "f_mount が FR_OK でない(%d)", (int)fr);
        esp_vfs_fat_unregister_path(SD_BASE);
        ff_diskio_unregister(pdrv);
        return ESP_FAIL;
    }
    g_mounted = true;
    g_detached = false;
    sd_log_card();
    return ESP_OK;
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

/* esp_muxer の既定のファイル書き込みはプリビルドで中を読めない。
 * 自前の writer に差し替えて、何回・何バイト・どこへ(seek)・何 ms かけたかを数える。
 * mux が add_video_packet の中で回っている疑いを、SD 書き込みの実測で切り分ける。 */
static uint32_t g_fw_n, g_fw_max, g_fw_sum;
static uint64_t g_fw_bytes;
static uint32_t g_fw_maxlen;      /* 1 回の on_write で頼まれたバイト数の最大 */
static uint32_t g_fw_err;         /* fwrite が頼まれた分だけ書けなかった回数 */
static bool g_sd_fail;            /* SD 書き込みが壊れた。真なら録画を止める */
static uint32_t g_fw_dbg;         /* 先頭何回を実測ログに出すか */
static uint64_t g_seek_n;
static uint64_t g_seek_last;

static void *mux_w_open(char *path)
{
    ESP_LOGI(TAG, "mux writer open %s", path);
    return fopen(path, "wb");
}

/* SD へは内部 RAM の小さなバッファ経由で書く。
 * muxer のキャッシュは PSRAM にある。録画中は拡大とエンコーダが PSRAM を
 * 使い尽くしているので、SDMMC の DMA が PSRAM から読むと間に合わず
 * EIO になる疑いをここで切る。 */
#define MUX_BOUNCE 16384
static uint8_t g_bounce[MUX_BOUNCE] __attribute__((aligned(64)));

static int mux_w_write(void *w, void *buf, int len)
{
    FILE *f = (FILE *)w;
    int64_t t0 = esp_timer_get_time();
    int done = 0;
    bool bad = false;
    while (done < len) {
        int chunk = len - done;
        if (chunk > MUX_BOUNCE) {
            chunk = MUX_BOUNCE;
        }
        memcpy(g_bounce, (const uint8_t *)buf + done, (size_t)chunk);
        int64_t tw = esp_timer_get_time();
        size_t n = fwrite(g_bounce, 1, (size_t)chunk, f);
        uint32_t d = (uint32_t)(esp_timer_get_time() - tw);
        g_fw_n++;
        g_fw_sum += d;
        g_fw_bytes += (uint64_t)chunk;
        if (d > g_fw_max) {
            g_fw_max = d;
        }
        if ((size_t)chunk != n) {
            bad = true;
            break;
        }
        done += (int)n;
    }
    uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
    if ((uint32_t)len > g_fw_maxlen) {
        g_fw_maxlen = (uint32_t)len;
    }
    if (bad) {
        g_fw_err++;
        g_sd_fail = true;         /* 何度やっても書けない。muxer にエラーを返して
                                   * 無限リトライさせず、録画を止める */
        if (g_fw_err <= 3) {
            /* カードとバスが生きているか、その場で直接確かめる。
             * ここが成功するなら SD のhardwareではなく FatFs の状態がおかしい。 */
            esp_err_t st = g_card ? sdmmc_get_status(g_card) : ESP_ERR_INVALID_STATE;
            esp_err_t rd = g_card ? sdmmc_read_sectors(g_card, g_bounce, 0, 1)
                                  : ESP_ERR_INVALID_STATE;
            ESP_LOGE(TAG, "mux writer 書き込み失敗 len=%d wrote=%d pos=%lld errno=%d。録画を止める",
                     len, done, (long long)ftell(f), errno);
            ESP_LOGE(TAG, "その場の SD 直接検査 status=%s read_sector0=%s",
                     esp_err_to_name(st), esp_err_to_name(rd));
        }
        return -1;
    }
    if (g_fw_dbg < 8) {
        ESP_LOGI(TAG, "mux write %u len=%d wrote=%d pos=%lld %uus",
                 (unsigned)g_fw_dbg + 1, len, done, (long long)ftell(f), dt);
        g_fw_dbg++;
    }
    return done;
}

static int mux_w_seek(void *w, uint64_t pos)
{
    g_seek_n++;
    g_seek_last = pos;
    return fseek((FILE *)w, (long)pos, SEEK_SET) == 0 ? 0 : -1;
}

static int mux_w_close(void *w)
{
    return fclose((FILE *)w) == 0 ? 0 : -1;
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
    static esp_muxer_file_writer_t fw = {
        .on_open = mux_w_open,
        .on_write = mux_w_write,
        .on_seek = mux_w_seek,
        .on_close = mux_w_close,
    };
    esp_muxer_err_t we = esp_muxer_set_file_writer(g_muxer, &fw);
    if (we != ESP_MUXER_ERR_OK) {
        ESP_LOGW(TAG, "file writer の差し替えに失敗(%d)。既定の書き込みで進む", (int)we);
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

static void encode_audio(int bytes);
static int pump_audio(bool flush);

/* AAC 符号化は mux(SD 書き込み)から切り離す。
 * 2026-10-19 r98 で mux が SD 書き込みで止まっている間 AAC を作れず、
 * 音声リングが溢れて(ringovf=10662)mp4 の音が 353 本しか入らなかった。
 * aac タスクは PCM を読み続けて符号化し、mux は出来上がった AAC を
 * mp4 に足すだけにする。 */
#define A_QSLOTS 4
typedef struct {
    int idx;
    int len;
    uint32_t pts;
} aac_item_t;
static uint8_t *g_abuf[A_QSLOTS];
static QueueHandle_t g_aq_free;
static QueueHandle_t g_aq_full;
static TaskHandle_t g_aac_task;
static volatile bool g_aac_run;
static uint32_t g_a_qdrop;

static void encode_audio(int bytes)
{
    esp_audio_enc_in_frame_t in = { .buffer = (uint8_t *)g_pcm, .len = (uint32_t)bytes };
    esp_audio_enc_out_frame_t out = { .buffer = g_aacbuf, .len = (uint32_t)g_out_bytes };
    int64_t ta = esp_timer_get_time();
    esp_audio_err_t e = esp_audio_enc_process(g_aac, &in, &out);
    uint32_t da = (uint32_t)(esp_timer_get_time() - ta);
    g_aac_n++;
    g_aac_sum += da;
    if (da > g_aac_max) {
        g_aac_max = da;
    }
    g_a_enc++;
    if (e == ESP_AUDIO_ERR_OK && out.encoded_bytes == 0) {
        g_a_zero++;
    }
    if (e == ESP_AUDIO_ERR_OK && out.encoded_bytes > 0) {
        /* リング溢れで捨てた分も時間として数える。捨てた分を無視すると
         * 音声の pts が実際の時刻より前にずれ、映像と音がずれる。 */
        uint32_t drop = audio_pipe_dropped_pairs();
        uint32_t ms = (uint32_t)((uint64_t)1000 * (g_audio_samples + drop) / AAC_RATE);
        aac_item_t it = { .idx = -1, .len = (int)out.encoded_bytes, .pts = ms };
        if (g_aq_free != NULL &&
            xQueueReceive(g_aq_free, &it.idx, pdMS_TO_TICKS(20)) == pdTRUE) {
            memcpy(g_abuf[it.idx], g_aacbuf, (size_t)out.encoded_bytes);
            if (xQueueSend(g_aq_full, &it, 0) != pdTRUE) {
                xQueueSend(g_aq_free, &it.idx, 0);
                g_aerr++;
                g_a_qdrop++;
            }
        } else {
            g_aerr++;
            g_a_qdrop++;
        }
    } else if (e != ESP_AUDIO_ERR_OK) {
        g_aerr++;
    }
    g_audio_samples += (uint32_t)(bytes / 4);
    g_pcm_fill -= bytes;
    if (g_pcm_fill > 0) {
        memmove(g_pcm, g_pcm + bytes / 2, (size_t)g_pcm_fill);
    }
}

static int pump_audio(bool flush)
{
    int made = 0;
    /* mux_task のループは SD 書き込みで 1 回 200ms 止まることもある。
     * 1 回につき AAC を 1 本しか作らないと 46.9 本/秒に足りず、
     * mp4 の音声が数本しか入らない。読み出せるだけ読み出して符号化する。
     * 部屋は g_pcm_cap(2 フレーム分)の空きだけ渡す。1 フレームちょうどの部屋だと、
     * リサンプルの出力に小数の残りが出て 1023 ペアまで進んだところで部屋が 1 ペアになり、
     * 部屋を埋める入力が 0 に切り捨てられて AAC に 1 本も渡らなかった(r8 の 音声=1)。 */
    for (;;) {
        while (g_pcm_fill + g_in_bytes <= g_pcm_cap) {
            int room = g_pcm_cap - g_pcm_fill;
            int got = audio_pipe_read(g_pcm + g_pcm_fill / 2, room / 4);
            if (got <= 0) {
                break;
            }
            g_a_reads++;
            g_a_pairs += (uint32_t)got;
            g_pcm_fill += got * 4;
        }
        if (g_pcm_fill < g_in_bytes) {
            break;
        }
        encode_audio(g_in_bytes); /* 余りは encode_audio が先頭へ寄せる */
        made++;
    }
    if (flush && g_pcm_fill > 0) {
        int have = g_pcm_fill;
        memset((uint8_t *)g_pcm + have, 0, (size_t)(g_in_bytes - have));
        encode_audio(g_in_bytes);
        made++;
    }
    return made;
}

/* mux が AAC を mp4 に足す。符号化は aac タスクがやっている。 */
static void drain_audio_q(void)
{
    aac_item_t it;
    while (g_muxer && xQueueReceive(g_aq_full, &it, 0) == pdTRUE) {
        esp_muxer_audio_packet_t ap = {
            .data = g_abuf[it.idx],
            .len = it.len,
            .pts = it.pts,
        };
        int64_t tb = esp_timer_get_time();
        esp_muxer_err_t ae = esp_muxer_add_audio_packet(g_muxer, g_aidx, &ap);
        uint32_t db = (uint32_t)(esp_timer_get_time() - tb);
        g_add_a_n++;
        g_add_a_sum += db;
        if (db > g_add_a_max) {
            g_add_a_max = db;
        }
        if (ae == ESP_MUXER_ERR_OK) {
            if (g_apkts == 0) {
                g_apts_first = it.pts;
            }
            g_apts_last = it.pts;
            g_apkts++;
        } else {
            g_aerr++;
        }
        xQueueSend(g_aq_free, &it.idx, 0);
    }
}

/* PCM を読んで AAC にするタスク。mux は SD 書き込みで 200ms 止まることがあるので、
 * それに引っ張られないよう別コア(core0)で回す。2026-10-19 r98 の音声欠け対策。 */
static void aac_task(void *arg)
{
    (void)arg;
    while (g_aac_run) {
        int n = pump_audio(false);
        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    pump_audio(true); /* 残った PCM を 1 本にまとめてから抜ける */
    ESP_LOGI(TAG, "aac タスク終了 stack 残り=%uB",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    g_aac_task = NULL;
    vTaskDelete(NULL);
}

/* 下に定義。フォーマット後にもジオメトリを出すので前に必要 */
static uint32_t fat_dump_state(void);

/* SD を FAT32 で作り直す。復旧手段というより検査手段にもなる。
 * 実測(2026-10-18): 生セクタ書き込みは全条件で ESP_OK なのにカードに残らない。
 * f_mkfs が指定した 64KB クラスタ(csize=128)ではなく、読み直すと 16KB(csize=32)
 * のまま。つまりフォーマットの内容も保存されていない。
 * esp_vfs_fat_sdcard_format が unmount → f_mkfs → remount をやってくれる。
 * 録画中と MSC 中は呼ばない。 */
esp_err_t gbac_sd_format(void)
{
    if (!g_card) {
        ESP_LOGE(TAG, "SD がマウントされていない");
        return ESP_FAIL;
    }
    if (g_detached) {
        ESP_LOGE(TAG, "PC に SD を見せている最中。フォーマットできない('m' で戻す)");
        return ESP_ERR_INVALID_STATE;
    }
    if (g_task) {
        ESP_LOGE(TAG, "録画中。先に 't' で止める");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "SD を FAT32 で作り直す。中身は全部消える");
    esp_err_t err = esp_vfs_fat_sdcard_format(SD_BASE, g_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "フォーマット失敗 %s", esp_err_to_name(err));
        return err;
    }
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(SD_BASE, &total, &freeb) == ESP_OK) {
        ESP_LOGI(TAG, "SD 作り直し完了 total=%lluMB free=%lluMB",
                 (unsigned long long)(total >> 20), (unsigned long long)(freeb >> 20));
    }
    /* カードから読み直したジオメトリ。csize が指定どおり 128(64KB)なら
     * フォーマットがカードに保存された。32(16KB)のままなら保存されていない。 */
    fat_dump_state();
    return ESP_OK;
}

/* FatFs のボリューム内部の状態を出す。FAT 表のジオメトリとウィンドウ位置。
 * 書き込みが FR_INT_ERR で止まったとき、どこでおかしいかを見るため。 */
static uint32_t fat_dump_state(void)
{
    FATFS *fs = NULL;
    DWORD nclst = 0;
    FRESULT fr = f_getfree("0:", &nclst, &fs);
    if (fr != FR_OK || !fs) {
        ESP_LOGE(TAG, "f_getfree fr=%d", (int)fr);
        return 0;
    }
    int ssize;
#if FF_MAX_SS != FF_MIN_SS
    ssize = fs->ssize;
#else
    ssize = FF_MAX_SS;
#endif
    ESP_LOGE(TAG, "FAT type=%d ssize=%d csize=%d n_fats=%d n_fatent=%u fsize=%u fatbase=%llu database=%llu winsect=%llu wflag=%d last_clst=%u free_clst=%u",
             (int)fs->fs_type, ssize, (int)fs->csize, (int)fs->n_fats,
             (unsigned)fs->n_fatent, (unsigned)fs->fsize,
             (unsigned long long)fs->fatbase, (unsigned long long)fs->database,
             (unsigned long long)fs->winsect, (int)fs->wflag,
             (unsigned)fs->last_clst, (unsigned)fs->free_clst);
    return (uint32_t)fs->winsect;
}

/* 64 バイトを 16 進 1 行に(3 文字 × 64) */
static void hex64(const uint8_t *p, char *out)
{
    for (int i = 0; i < 64; i++) {
        sprintf(out + i * 3, "%02x ", p[i]);
    }
    out[64 * 3 - 1] = '\0';
}

/* FatFs を通さずカードから生セクタを 1 つ読み、DMA 整列バッファと
 * 4 バイトずらしたバッファの 2 とおりで中身を出す。FatFs のウィンドウ
 * (fs->win)と同じセクタなので、FatFs が見ている内容と実カードの内容を
 * 比べられる。FAT 表のセクタが読めていない疑いを確かめるため。 */
static void fat_sector_probe(uint32_t sect)
{
    FATFS *fs = NULL;
    DWORD nc = 0;
    FRESULT frg = f_getfree("0:", &nc, &fs);
    if (frg != FR_OK || !fs) {
        ESP_LOGE(TAG, "probe %u: FATFS が取れない fr=%d", sect, (int)frg);
        return;
    }
    uint8_t *al = heap_caps_aligned_alloc(64, 512, MALLOC_CAP_DMA);
    uint8_t *raw = heap_caps_aligned_alloc(64, 520, MALLOC_CAP_DMA);
    if (!al || !raw) {
        ESP_LOGE(TAG, "probe %u: バッファ確保失敗", sect);
        free(al);
        free(raw);
        return;
    }
    uint8_t *un = raw + 4;      /* DMA 整列(16B)を崩す */
    esp_err_t e1 = sdmmc_read_sectors(g_card, al, sect, 1);
    esp_err_t e2 = sdmmc_read_sectors(g_card, un, sect, 1);
    char ha[64 * 3], hu[64 * 3], hw[64 * 3];
    hex64(al, ha);
    hex64(un, hu);
    hex64(fs->win, hw);
    ESP_LOGE(TAG, "sec %u 整列 err=%d %s", sect, (int)e1, ha);
    ESP_LOGE(TAG, "sec %u 非整列 err=%d %s", sect, (int)e2, hu);
    ESP_LOGE(TAG, "sec %u win(winsect=%llu) %s", sect,
             (unsigned long long)fs->winsect, hw);
    int d_au = 0, d_aw = 0;
    for (int i = 0; i < 512; i++) {
        if (al[i] != un[i]) {
            d_au++;
        }
        if (al[i] != fs->win[i]) {
            d_aw++;
        }
    }
    ESP_LOGE(TAG, "sec %u 比較 整列vs非整列 不一致 %d/512  整列vs win 不一致 %d/512",
             sect, d_au, d_aw);
    uint32_t bc = (uint32_t)(sect - (uint32_t)fs->fatbase) * 128u;
    const uint32_t *e = (const uint32_t *)al;
    ESP_LOGE(TAG, "sec %u FAT32 ent[%u..%u]: %08x %08x %08x %08x %08x %08x %08x %08x",
             sect, bc, bc + 7, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
    free(al);
    free(raw);
}

/* FAT 表を実カードからまとめて読み、0 でないエントリを出す。
 * FatFs が書いた FAT がカードに届いているのか、届いているならどこへ行ったのかを見る。 */
static void fat_scan_nonzero(uint32_t maxrep)
{
    FATFS *fs = NULL;
    DWORD nc = 0;
    if (f_getfree("0:", &nc, &fs) != FR_OK || !fs) {
        ESP_LOGE(TAG, "FAT scan: FATFS が取れない");
        return;
    }
    uint8_t *b = heap_caps_aligned_alloc(64, 32768, MALLOC_CAP_DMA);
    if (!b) {
        ESP_LOGE(TAG, "FAT scan: 32KB が確保できない");
        return;
    }
    const uint32_t CH_SEC = 64;
    for (uint32_t copy = 0; copy < (uint32_t)fs->n_fats; copy++) {
        uint32_t base = (uint32_t)fs->fatbase + copy * (uint32_t)fs->fsize;
        uint32_t nz = 0, rep = 0;
        for (uint32_t s = 0; s < (uint32_t)fs->fsize; s += CH_SEC) {
            uint32_t cnt = (uint32_t)fs->fsize - s;
            if (cnt > CH_SEC) {
                cnt = CH_SEC;
            }
            if (sdmmc_read_sectors(g_card, b, base + s, cnt) != ESP_OK) {
                ESP_LOGE(TAG, "FAT scan copy%u: sec %u 読み失敗", copy, base + s);
                break;
            }
            const uint32_t *e = (const uint32_t *)b;
            for (uint32_t i = 0; i < cnt * 128u; i++) {
                if (e[i] == 0) {
                    continue;
                }
                nz++;
                if (rep < maxrep) {
                    ESP_LOGE(TAG, "FAT copy%u sec=%u off=%u ent=%u val=%08x", copy,
                             base + s + (i >> 7), (i & 127) * 4, s * 128u + i, e[i]);
                    rep++;
                }
            }
        }
        ESP_LOGE(TAG, "FAT copy%u %u..%u 非ゼロ %u 個(表示 %u)", copy, base,
                 base + (uint32_t)fs->fsize - 1, nz, rep);
    }
    free(b);
}

/* 1 セクタだけ書いて読み戻す。FatFs が FAT 表へ書くのと同じ大きさ(512B)の
 * 書き込みがカードに届くかを見る。試した後は 'F' で作り直すこと。 */
static void raw_one_sector(uint32_t sect)
{
    uint8_t *w = heap_caps_aligned_alloc(64, 512, MALLOC_CAP_DMA);
    uint8_t *r = heap_caps_aligned_alloc(64, 512, MALLOC_CAP_DMA);
    if (!w || !r) {
        free(w);
        free(r);
        ESP_LOGE(TAG, "1セクタ試験: バッファ確保失敗");
        return;
    }
    for (int i = 0; i < 512; i++) {
        w[i] = (uint8_t)((i * 7 + 0x31) & 0xff);
    }
    esp_err_t e1 = sdmmc_write_sectors(g_card, w, sect, 1);
    memset(r, 0, 512);
    esp_err_t e2 = sdmmc_read_sectors(g_card, r, sect, 1);
    int d = 0;
    for (int i = 0; i < 512; i++) {
        if (w[i] != r[i]) {
            d++;
        }
    }
    char hw[64 * 3], hr[64 * 3];
    hex64(w, hw);
    hex64(r, hr);
    ESP_LOGE(TAG, "1セクタ sec=%u 書込=%s 読込=%s 不一致 %d/512", sect,
             esp_err_to_name(e1), esp_err_to_name(e2), d);
    ESP_LOGE(TAG, "  期待 %s", hw);
    ESP_LOGE(TAG, " 実際 %s", hr);
    free(w);
    free(r);
}

/* FAT 表の中身を実カードから読む検査('z') */
esp_err_t gbac_sd_fatscan(void)
{
    if (!g_card || g_detached || g_task) {
        ESP_LOGE(TAG, "SD が使えない状態(mount 済み・非録画・非 MSC のときだけ)");
        return ESP_ERR_INVALID_STATE;
    }
    fat_dump_state();
    fat_scan_nonzero(24);
    FATFS *fs = NULL;
    DWORD nc = 0;
    if (f_getfree("0:", &nc, &fs) == FR_OK && fs) {
        raw_one_sector((uint32_t)fs->fatbase + 15);   /* 壊れている FAT セクタ */
        raw_one_sector((uint32_t)fs->database + 4);   /* データ領域 */
    }
    ESP_LOGE(TAG, "この検査の後は 'F' で SD を作り直すこと");
    return ESP_OK;
}

/* 生セクタ書き込みがどこへ行くのか探す検査('v')。
 * 模様を書いてから、同じセクタ・周辺・カード先頭を読み、
 * 模様が実在する場所を探す。書き込み先がずれているならここで分かる。 */
static size_t find_pattern(const uint8_t *hay, size_t hlen, const uint8_t *pat, size_t plen)
{
    if (hlen < plen) {
        return SIZE_MAX;
    }
    for (size_t i = 0; i + plen <= hlen; i++) {
        if (hay[i] == pat[0] && memcmp(hay + i, pat, plen) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

esp_err_t gbac_sd_wrprobe(void)
{
    if (!g_card || g_detached || g_task) {
        ESP_LOGE(TAG, "SD が使えない状態(mount 済み・非録画・非 MSC のときだけ)");
        return ESP_ERR_INVALID_STATE;
    }
    sdmmc_card_print_info(stdout, g_card);
    ESP_LOGE(TAG, "get_status=%s ocr=%08x capacity=%u bus=%dbit real=%dkhz max=%u",
             esp_err_to_name(sdmmc_get_status(g_card)), (unsigned)g_card->ocr,
             (unsigned)g_card->csd.capacity, 1u << (unsigned)g_card->log_bus_width,
             g_card->real_freq_khz, (unsigned)g_card->max_freq_khz);
    FATFS *fs = NULL;
    DWORD nc = 0;
    if (f_getfree("0:", &nc, &fs) != FR_OK || !fs) {
        ESP_LOGE(TAG, "FATFS が取れない");
        return ESP_FAIL;
    }
    const uint32_t X = (uint32_t)fs->database + 2048u * 1024u;   /* データ 1GB 地点 */
    uint8_t *w = heap_caps_aligned_alloc(64, 512, MALLOC_CAP_DMA);
    uint8_t *r = heap_caps_aligned_alloc(64, 512, MALLOC_CAP_DMA);
    uint8_t *big = heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!w || !r || !big) {
        free(w);
        free(r);
        free(big);
        ESP_LOGE(TAG, "wrprobe: バッファ確保失敗");
        return ESP_ERR_NO_MEM;
    }
    static const char MAGIC[] = "GBAPROBE12345678";
    memcpy(w, MAGIC, 16);
    for (int i = 16; i < 512; i++) {
        w[i] = (uint8_t)((i * 3 + 0x11) & 0xff);
    }
    esp_err_t e1 = sdmmc_write_sectors(g_card, w, X, 1);
    memset(r, 0, 512);
    esp_err_t e2 = sdmmc_read_sectors(g_card, r, X, 1);
    int d = 0;
    for (int i = 0; i < 512; i++) {
        if (w[i] != r[i]) {
            d++;
        }
    }
    ESP_LOGE(TAG, "wrprobe sec=%u 書込=%s 読込=%s 不一致 %d/512", X,
             esp_err_to_name(e1), esp_err_to_name(e2), d);
    ESP_LOGE(TAG, "書込後の get_status=%s",
             esp_err_to_name(sdmmc_get_status(g_card)));

    /* 周辺 ±64 セクタ(128 セクタ)を探す */
    uint32_t s0 = X - 64;
    if (sdmmc_read_sectors(g_card, big, s0, 128) == ESP_OK) {
        size_t at = find_pattern(big, 128 * 512, (const uint8_t *)MAGIC, 16);
        if (at == SIZE_MAX) {
            ESP_LOGE(TAG, "周辺検索 %u..%u : 模様なし", s0, s0 + 127);
        } else {
            ESP_LOGE(TAG, "周辺検索 %u..%u : sec=%u off=%u に発見", s0, s0 + 127,
                     s0 + (uint32_t)(at / 512), (unsigned)(at % 512));
        }
    }
    /* カード先頭 1MB を探す(アドレス指定の間違いならここ) */
    size_t found_at = SIZE_MAX;
    for (uint32_t s = 0; s < 2048; s += 128) {
        if (sdmmc_read_sectors(g_card, big, s, 128) != ESP_OK) {
            break;
        }
        size_t at = find_pattern(big, 128 * 512, (const uint8_t *)MAGIC, 16);
        if (at != SIZE_MAX) {
            found_at = (size_t)s * 512 + at;
            break;
        }
    }
    ESP_LOGE(TAG, "先頭 1MB 検索: %s",
             found_at == SIZE_MAX ? "模様なし" : "発見");
    if (found_at != SIZE_MAX) {
        ESP_LOGE(TAG, "  場所: バイト %u セクタ %u off %u", (unsigned)found_at,
                 (unsigned)(found_at / 512), (unsigned)(found_at % 512));
    }
    /* 0 で戻す */
    memset(w, 0, 512);
    ESP_LOGE(TAG, "sec=%u を 0 で戻した(%s)", X,
             esp_err_to_name(sdmmc_write_sectors(g_card, w, X, 1)));
    free(w);
    free(r);
    free(big);
    ESP_LOGE(TAG, "この検査の後は 'F' で SD を作り直すこと");
    return ESP_OK;
}

/* 生セクタへ nsec 個書いて読み戻し、バッファの種類で書き込みが消えるか見る。
 * (a) 内部 RAM の 64 バイト整列 DMA バッファ
 * (b) 内部 RAM で 4 バイトずらして DMA 整列を崩したもの
 * (c) PSRAM バッファ
 * 書いた場所は最後に 0 で戻す。 */
static void raw_write_test(uint32_t sect, uint32_t nsec)
{
    size_t sz = (size_t)nsec * 512;
    uint8_t *al = heap_caps_aligned_alloc(64, sz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *rawb = heap_caps_aligned_alloc(64, sz + 4, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *ps = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *rd = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ps || !rd) {
        ESP_LOGE(TAG, "sec=%u n=%u: %uKB×2 が確保できない", sect, nsec, (unsigned)(sz / 1024));
        free(al);
        free(rawb);
        free(ps);
        free(rd);
        return;
    }
    const char *nm[3] = { "内部整列", "内部4ずら", "PSRAM" };
    uint8_t *kb[3] = { al, rawb ? rawb + 4 : NULL, ps };
    for (int k = 0; k < 3; k++) {
        if (!kb[k]) {
            ESP_LOGE(TAG, "sec=%u n=%u %s: バッファなし", sect, nsec, nm[k]);
            continue;
        }
        for (size_t i = 0; i < sz; i++) {
            kb[k][i] = (uint8_t)((i * 3 + k * 17 + 0x11) & 0xff);
        }
        esp_err_t e1 = sdmmc_write_sectors(g_card, kb[k], sect, nsec);
        memset(rd, 0, sz);
        esp_err_t e2 = sdmmc_read_sectors(g_card, rd, sect, nsec);
        size_t d = 0, first = 0;
        for (size_t i = 0; i < sz; i++) {
            if (rd[i] != kb[k][i]) {
                if (!d) {
                    first = i;
                }
                d++;
            }
        }
        ESP_LOGE(TAG, "sec=%u n=%u %s 書込=%s 読込=%s 不一致 %u/%u 最初 %zu(期待%02x 実際%02x)",
                 sect, nsec, nm[k], esp_err_to_name(e1), esp_err_to_name(e2),
                 (unsigned)d, (unsigned)sz, first, kb[k][first], rd[first]);
    }
    memset(ps, 0, sz);
    esp_err_t e3 = sdmmc_write_sectors(g_card, ps, sect, nsec);
    ESP_LOGE(TAG, "sec=%u n=%u を 0 で戻した(%s)", sect, nsec, esp_err_to_name(e3));
    free(al);
    free(rawb);
    free(ps);
    free(rd);
}

/* 1 セクタ単発の書き込みが消えるのか、バッファの種類で違うのかを見る検査('x')。
 * FAT1 の未使用領域とデータ領域の後ろでやる。後は 'F' で作り直すこと。 */
esp_err_t gbac_sd_buf_battery(void)
{
    if (!g_card || g_detached || g_task) {
        ESP_LOGE(TAG, "SD が使えない状態(mount 済み・非録画・非 MSC のときだけ)");
        return ESP_ERR_INVALID_STATE;
    }
    FATFS *fs = NULL;
    DWORD nc = 0;
    if (f_getfree("0:", &nc, &fs) != FR_OK || !fs) {
        ESP_LOGE(TAG, "FATFS が取れない");
        return ESP_FAIL;
    }
    ESP_LOGE(TAG, "FAT1 %u..%u FAT2 %u..%u データ開始 %u",
             (unsigned)fs->fatbase, (unsigned)(fs->fatbase + fs->fsize - 1),
             (unsigned)(fs->fatbase + fs->fsize),
             (unsigned)(fs->fatbase + 2 * fs->fsize - 1), (unsigned)fs->database);
    raw_write_test((uint32_t)fs->fatbase + fs->fsize - 328, 1);   /* FAT1 未使用 */
    raw_write_test((uint32_t)fs->database + 2048, 1);             /* データ 1MB 地点 */
    raw_write_test((uint32_t)fs->database + 2048u * 1024u, 1);    /* 1GB 地点 1 セクタ */
    raw_write_test((uint32_t)fs->database + 2048u * 1024u, 2);    /* 2 セクタ */
    raw_write_test((uint32_t)fs->database + 2048u * 1024u, 8);    /* 8 セクタ */
    raw_write_test((uint32_t)fs->database + 2048u * 1024u, 256);  /* 256 セクタ */
    ESP_LOGE(TAG, "この検査の後は 'F' で SD を作り直すこと");
    return ESP_OK;
}

/* fp.clust が載っている FAT セクタ(1 枚目と 2 枚目)を実カードから読み、
 * 鎖のエントリがカードに残っているか出す。f_sync 直後に確認する。 */
static void fat_check_clust(uint32_t clust)
{
    FATFS *fs = NULL;
    DWORD nc = 0;
    if (f_getfree("0:", &nc, &fs) != FR_OK || !fs) {
        ESP_LOGE(TAG, "FAT ent[%u]: FATFS が取れない", clust);
        return;
    }
    uint8_t *b = heap_caps_aligned_alloc(64, 512, MALLOC_CAP_DMA);
    if (!b) {
        ESP_LOGE(TAG, "FAT ent[%u]: 512B が確保できない", clust);
        return;
    }
    uint32_t off = (clust & 127u) * 4u;
    uint32_t s1 = (uint32_t)fs->fatbase + clust / 128u;
    uint32_t s2 = s1 + (uint32_t)fs->fsize;
    esp_err_t e1 = sdmmc_read_sectors(g_card, b, s1, 1);
    uint32_t v1 = *(const uint32_t *)(b + off);
    esp_err_t e2 = sdmmc_read_sectors(g_card, b, s2, 1);
    uint32_t v2 = *(const uint32_t *)(b + off);
    ESP_LOGE(TAG, "FAT ent[%u] 1枚目 sec=%u %s val=%08x 2枚目 sec=%u %s val=%08x",
             clust, s1, esp_err_to_name(e1), v1, s2, esp_err_to_name(e2), v2);
    free(b);
}

/* SD の実測の書き込み・読み出し速度。esp_muxer の ram_cache_size を決める材料と、
 * mux が詰まる原因(SD が遅い / 止まる)を切り分けるために使う。
 * 録画していないときだけ呼ぶこと(FAT を二重に触らない)。 */
esp_err_t gbac_sd_speedtest(int mb)
{
    if (!g_card) {
        ESP_LOGE(TAG, "SD がマウントされていない");
        return ESP_FAIL;
    }
    if (g_detached) {
        ESP_LOGE(TAG, "PC に SD を見せている最中。速度テストはできない('m' で戻す)");
        return ESP_ERR_INVALID_STATE;
    }
    if (mb < 1) {
        mb = 1;
    }
    const int CH = 128 * 1024;
    uint8_t *buf = heap_caps_malloc((size_t)CH, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "速度テストの %dKB が確保できない", CH / 1024);
        return ESP_ERR_NO_MEM;
    }
    memset(buf, 0x5a, (size_t)CH);

    /* stdio 経由だと FRESULT が errno に潰されて、ディスク層のエラーなのか
     * FatFs の内部エラーなのか区別できない。FatFs の API を直接呼ぶ。 */
    FIL fl;
    FRESULT fr = f_open(&fl, "0:/SPEED.BIN", FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        free(buf);
        ESP_LOGE(TAG, "f_open 失敗 fr=%d", (int)fr);
        return ESP_FAIL;
    }

    int64_t t0 = esp_timer_get_time();
    int wrote = 0;
    int n = mb * 8;                 /* 1 回 128KB なので mb MB には 8 倍必要 */
    UINT bw = 0;
    for (int i = 0; i < n; i++) {
        fr = f_write(&fl, buf, (UINT)CH, &bw);
        if (fr != FR_OK || bw != (UINT)CH) {
            int64_t e = esp_timer_get_time();
            ESP_LOGE(TAG, "f_write 失敗 %d回目=%.2fMB %.1fMB/s fr=%d wrote=%u pos=%llu fp.err=%d fp.clust=%u",
                     i, (double)((int64_t)i * CH) / 1048576.0,
                     (double)wrote * CH / 1e6 / ((double)(e - t0) / 1e6),
                     (int)fr, bw, (unsigned long long)f_tell(&fl), (int)fl.err,
                     (unsigned)fl.clust);
            uint32_t ws = fat_dump_state();
            if (ws) {
                /* 壊れている疑いのある FAT セクタと、その前後を実カードから読む */
                fat_sector_probe(ws);
                fat_sector_probe(ws + 1);
                fat_sector_probe(ws - 1);
            }
            ESP_LOGE(TAG, "f_close fr=%d", (int)f_close(&fl));
            free(buf);
            return ESP_FAIL;
        }
        wrote++;
        if ((wrote & 15) == 0) {
            int64_t e = esp_timer_get_time();
            ESP_LOGI(TAG, "SD 書込 %.1fMB %.1fMB/s",
                     (double)((int64_t)wrote * CH) / 1048576.0,
                     (double)wrote * CH / 1e6 / ((double)(e - t0) / 1e6));
        }
    }
    int64_t tw = esp_timer_get_time();
    fr = f_sync(&fl);
    int64_t ts = esp_timer_get_time();
    fr = f_close(&fl);
    int64_t tf = esp_timer_get_time();
    ESP_LOGI(TAG, "SD 書込 %.1fMB %.1fMB/s  sync %.0fms(fr=%d) close %.0fms(fr=%d)",
             (double)((int64_t)wrote * CH) / 1048576.0,
             (double)wrote * CH / 1e6 / ((double)(tw - t0) / 1e6),
             (double)(ts - tw) / 1000.0, (int)fr,
             (double)(tf - ts) / 1000.0, (int)fr);

    /* 読み戻し。マウントの読み側と FAT の状態も見る */
    fr = f_open(&fl, "0:/SPEED.BIN", FA_READ);
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "読み戻しの f_open 失敗 fr=%d", (int)fr);
        /* ルートディレクトリを直接数えて、目录エントリが残っているか見る。
         * 書いた直後に消えているなら、目录エントリの書き込みが飛んでいる。 */
        FATFS *fs2 = NULL;
        DWORD nclst2 = 0;
        ESP_LOGE(TAG, "f_getfree fr=%d", (int)f_getfree("0:", &nclst2, &fs2));
        FILINFO *fi = malloc(sizeof(FILINFO));
        if (fi) {
            FRESULT fr2 = f_stat("0:/SPEED.BIN", fi);
            ESP_LOGE(TAG, "f_stat SPEED.BIN fr=%d size=%lld", (int)fr2,
                     (long long)fi->fsize);
            free(fi);
        }
        /* POSIX 側からも数える(FatFs 側だけ見えないのかを見る) */
        DIR *d = opendir(SD_BASE);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                ESP_LOGE(TAG, "  root: %s", e->d_name);
            }
            closedir(d);
        } else {
            ESP_LOGE(TAG, "opendir %s 失敗", SD_BASE);
        }
        fat_dump_state();
        free(buf);
        return ESP_FAIL;
    }
    int64_t r0 = esp_timer_get_time();
    f_read(&fl, buf, (UINT)CH, &bw);
    int64_t r1 = esp_timer_get_time();
    f_close(&fl);
    ESP_LOGI(TAG, "SD 先頭 %dKB 読み %.1fMB/s", (int)bw / 1024,
             (double)bw / 1e6 / ((double)(r1 - r0) / 1e6));
    free(buf);
    return ESP_OK;
}

/* FatFs を通さず、カードの生セクタへ直接 mb MB 書いて読み戻して比べる。
 * カードそのものが壊れているのか、FatFs のせいなのかを切り分けるため。
 * 書き込む場所は既定で 1GB 地点(データ領域の後ろの方)。FAT 表は触らないが、
 * その場所にあったファイルは壊れる。試した後は 'F' で作り直すこと。 */
esp_err_t gbac_sd_rawtest(int mb, int start_mb)
{
    if (!g_card) {
        ESP_LOGE(TAG, "SD がマウントされていない");
        return ESP_FAIL;
    }
    if (g_detached) {
        ESP_LOGE(TAG, "PC に SD を見せている最中。生セクタテストはできない('m' で戻す)");
        return ESP_ERR_INVALID_STATE;
    }
    if (g_task) {
        ESP_LOGE(TAG, "録画中。先に 't' で止める");
        return ESP_ERR_INVALID_STATE;
    }
    if (mb < 1) {
        mb = 1;
    }
    if (start_mb < 1) {
        start_mb = 1;
    }
    const int CH = 128 * 1024;
    const uint32_t SEC_PER_CH = CH / 512;      /* 256 セクタ */
    uint8_t *w = heap_caps_malloc((size_t)CH, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *r = heap_caps_malloc((size_t)CH, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!w || !r) {
        free(w);
        free(r);
        ESP_LOGE(TAG, "生セクタテストの %dKB×2 が確保できない", CH / 1024);
        return ESP_ERR_NO_MEM;
    }
    uint64_t base = (uint64_t)start_mb * 2048;  /* 1MB = 512B × 2048 */
    int n = mb * 8;                             /* 128KB × n */
    if (base + (uint64_t)n * SEC_PER_CH > g_card->csd.capacity) {
        free(w);
        free(r);
        ESP_LOGE(TAG, "範囲外: %llu+%d セクタ > カード %u セクタ",
                 (unsigned long long)base, n * (int)SEC_PER_CH, (unsigned)g_card->csd.capacity);
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "生セクタ書込 %dMB @%dMB 地点(カード %u セクタ)", mb, start_mb,
             (unsigned)g_card->csd.capacity);

    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < CH; j++) {
            w[j] = (uint8_t)((i * 131 + j * 3 + 0x5a) & 0xff);
        }
        esp_err_t err = sdmmc_write_sectors(g_card, w, (size_t)(base + (uint64_t)i * SEC_PER_CH),
                                            SEC_PER_CH);
        if (err != ESP_OK) {
            int64_t e = esp_timer_get_time();
            ESP_LOGE(TAG, "生セクタ書込 失敗 %dMB目(%d回目) %s %.1fMB/s",
                     (i * CH) >> 20, i, esp_err_to_name(err),
                     (double)i * CH / 1e6 / ((double)(e - t0) / 1e6));
            free(w);
            free(r);
            return err;
        }
    }
    int64_t tw = esp_timer_get_time();
    ESP_LOGI(TAG, "生セクタ書込 %dMB %.1fMB/s", mb,
             (double)n * CH / 1e6 / ((double)(tw - t0) / 1e6));

    int bad = 0, first_i = -1, first_off = -1, first_exp = 0, first_act = 0;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < CH; j++) {
            w[j] = (uint8_t)((i * 131 + j * 3 + 0x5a) & 0xff);
        }
        esp_err_t err = sdmmc_read_sectors(g_card, r, (size_t)(base + (uint64_t)i * SEC_PER_CH),
                                           SEC_PER_CH);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "生セクタ読込 失敗 %dMB目(%d回目) %s", (i * CH) >> 20, i,
                     esp_err_to_name(err));
            free(w);
            free(r);
            return err;
        }
        if (memcmp(w, r, (size_t)CH) != 0) {
            int j = 0;
            while (j < CH && w[j] == r[j]) {
                j++;
            }
            if (first_i < 0) {
                first_i = i;
                first_off = j;
                first_exp = w[j];
                first_act = r[j];
            }
            bad++;
        }
    }
    int64_t tr = esp_timer_get_time();
    ESP_LOGI(TAG, "生セクタ読込 %dMB %.1fMB/s  不一致 %d/%d 個", mb,
             (double)n * CH / 1e6 / ((double)(tr - tw) / 1e6), bad, n);
    if (bad) {
        ESP_LOGE(TAG, "最初の不一致 %dMB目(%d回目) 内 %d バイト目 期待=%02x 実際=%02x",
                 (first_i * CH) >> 20, first_i, first_off, first_exp, first_act);
        free(w);
        free(r);
        return ESP_FAIL;
    }
    free(w);
    free(r);
    return ESP_OK;
}

/* mux_task が今どの呼び出しにいるかを常に残しておく。
 * NAL スロットが枯れたとき、エンコード側から「mux がどこで止まっているか」
 * を見るため。esp_muxer の mp4 はプリビルドなので中を読めない。 */
static const char *volatile g_mux_where = "待機";
static volatile int64_t g_mux_where_us;

static void mux_mark(const char *w)
{
    g_mux_where = w;
    g_mux_where_us = esp_timer_get_time();
}

const char *gbac_rec_mux_where(void)
{
    return g_mux_where;
}

int64_t gbac_rec_mux_age_us(void)
{
    return g_mux_where_us ? esp_timer_get_time() - g_mux_where_us : 0;
}

/* mux タスクがどこでどれだけ回っているか。mux が esp_muxer の中で止まると
 * 停止ログにたどり着けないので、定期表示の 1 行として読めるようにする。 */
void gbac_rec_mux_stats(char *out, int out_sz)
{
    snprintf(out, (size_t)out_sz,
             "addV n=%u max=%u avg=%u | addA n=%u max=%u avg=%u | aac n=%u max=%u avg=%u"
             " | file n=%u max=%u avg=%u bytes=%lluMB maxlen=%u werr=%u sdfail=%d",
             (unsigned)g_add_v_n, (unsigned)g_add_v_max,
             (unsigned)(g_add_v_n ? g_add_v_sum / g_add_v_n : 0),
             (unsigned)g_add_a_n, (unsigned)g_add_a_max,
             (unsigned)(g_add_a_n ? g_add_a_sum / g_add_a_n : 0),
             (unsigned)g_aac_n, (unsigned)g_aac_max,
             (unsigned)(g_aac_n ? g_aac_sum / g_aac_n : 0),
             (unsigned)g_fw_n, (unsigned)g_fw_max,
             (unsigned)(g_fw_n ? g_fw_sum / g_fw_n : 0),
             (unsigned long long)(g_fw_bytes / (1024 * 1024)),
             (unsigned)g_fw_maxlen, (unsigned)g_fw_err, (int)g_sd_fail);
}

/* NAL を 1 本 muxer に入れる。まだ muxer がなければ SPS/PPS を拾ってから開く。 */
static void write_video(const gbac_nal_t *n)
{
    if (g_sd_fail) {
        return;                   /* SD が壊れたあと書き足しても意味がない */
    }
    if (!g_muxer) {
        mux_mark("SPS/PPS 抽出");
        extract_sps_pps(n->buf, n->len);
        if (!g_avcc) {
            ESP_LOGE(TAG, "SPS/PPS が見つからない NAL(%d バイト)", n->len);
            return;
        }
        mux_mark("muxer open");
        if (open_muxer() != ESP_OK) {
            g_run = false;
            return;
        }
    }
    uint32_t ms = (uint32_t)(n->pts_us / 1000);
    /* muxer に渡す NAL の中身が壊れている疑いを、渡す直前で確かめる */
    if (g_nal_dbg < 10) {
        ESP_LOGI(TAG, "NAL %d/10 len=%d key=%d pts=%uus head=%02x %02x %02x %02x %02x",
                 g_nal_dbg + 1, n->len, n->key, (unsigned)ms,
                 n->buf[0], n->buf[1], n->buf[2], n->buf[3], n->buf[4]);
        g_nal_dbg++;
    }
    if (n->len <= 0 || n->len > 512 * 1024) {
        g_verr++;
        ESP_LOGE(TAG, "NAL の len が不正(%d)。捨てて続ける pts=%uus",
                 n->len, (unsigned)ms);
        return;
    }
    if (g_pts_dbg < 20) {
        ESP_LOGI(TAG, "mux %d/20 ms=%u len=%d key=%d", g_pts_dbg + 1, ms, n->len, n->key);
        g_pts_dbg++;
    }
    esp_muxer_video_packet_t vp = {
        .data = n->buf,
        .len = n->len,
        .pts = ms,
        .dts = ms,
        .key_frame = n->key,
    };
    mux_mark("add_video_packet");
    int64_t tc = esp_timer_get_time();
    esp_muxer_err_t me = esp_muxer_add_video_packet(g_muxer, g_vidx, &vp);
    uint32_t dc = (uint32_t)(esp_timer_get_time() - tc);
    g_add_v_n++;
    g_add_v_sum += dc;
    if (dc > g_add_v_max) {
        g_add_v_max = dc;
    }
    if (me == ESP_MUXER_ERR_OK) {
        if (g_vpkts == 0) {
            g_vpts_first = ms;
        }
        g_vpts_last = ms;
        g_vpkts++;
    } else {
        g_verr++;
        ESP_LOGE(TAG, "video packet 追加失敗(%d)", (int)me);
    }
}

static void mux_task(void *arg)
{
    (void)arg;
    int64_t last_space = esp_timer_get_time();
    while (true) {
        gbac_nal_t n;
        mux_mark("NAL 待ち");
        if (gbac_enc_pop(&n, pdMS_TO_TICKS(10))) {
            write_video(&n);
            mux_mark("slot 返却");
            gbac_enc_free(&n);
        }
        if (g_muxer) {
            mux_mark("drain_audio");
            drain_audio_q();
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
        if (!g_run || g_sd_fail) {
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
        int64_t tf = esp_timer_get_time();
        /* aac タスクに残りの PCM を 1 本にまとめさせてから mp4 に足す。
         * 先に muxer を閉じると足す先がなくなる。 */
        g_aac_run = false;
        for (int i = 0; i < 200 && g_aac_task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        drain_audio_q();
        g_flush_us = (uint32_t)(esp_timer_get_time() - tf);
        int64_t tg = esp_timer_get_time();
        esp_muxer_err_t me = esp_muxer_close(g_muxer);
        g_close_us = (uint32_t)(esp_timer_get_time() - tg);
        if (me != ESP_MUXER_ERR_OK) {
            ESP_LOGE(TAG, "muxer close 失敗(%d)", (int)me);
        }
        g_muxer = NULL;
    }
    ESP_LOGI(TAG, "muxer 終了 video=%u audio=%u err=%u/%u stack 残り=%uB",
             (unsigned)g_vpkts, (unsigned)g_apkts, (unsigned)g_verr, (unsigned)g_aerr,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "mux 呼び出し us: video n=%u max=%u avg=%u | audio n=%u max=%u avg=%u | aac n=%u max=%u avg=%u | flush=%u close=%u",
             (unsigned)g_add_v_n, (unsigned)g_add_v_max,
             (unsigned)(g_add_v_n ? g_add_v_sum / g_add_v_n : 0),
             (unsigned)g_add_a_n, (unsigned)g_add_a_max,
             (unsigned)(g_add_a_n ? g_add_a_sum / g_add_a_n : 0),
             (unsigned)g_aac_n, (unsigned)g_aac_max,
             (unsigned)(g_aac_n ? g_aac_sum / g_aac_n : 0),
             (unsigned)g_flush_us, (unsigned)g_close_us);
    ESP_LOGI(TAG, "音声内訳 read=%u pairs=%u aac=%u zero=%u ringovf=%u",
             (unsigned)g_a_reads, (unsigned)g_a_pairs, (unsigned)g_a_enc, (unsigned)g_a_zero,
             (unsigned)audio_pipe_overruns());
    g_task = NULL;
    vTaskDelete(NULL);
}

/* SD の中にある REC_NNNN の最大番号。再起動しても番号が前に進むように使う。
 * stat() だけでは、まだ mp4 が閉じられていない回とぶつかって
 * 同じ番号を割り当てて上書きした(REC_0006 が毎回上書きされた)。 */
static int scan_max_num(void)
{
    DIR *d = opendir(SD_BASE);
    if (d == NULL) {
        return 0;
    }
    int max = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int n = 0;
        if (sscanf(e->d_name, "REC_%d.mp4", &n) == 1 && n > max && n <= 9999) {
            max = n;
        }
    }
    closedir(d);
    return max;
}

static void next_base(void)
{
    int ondisk = scan_max_num();
    if (ondisk > g_last_num) {
        g_last_num = ondisk;
    }
    for (int i = g_last_num + 1; i <= 9999; i++) {
        char p[64];
        snprintf(p, sizeof(p), "%s/REC_%04d.mp4", SD_BASE, i);
        struct stat st;
        if (stat(p, &st) != 0) {
            g_last_num = i;
            snprintf(g_base, sizeof(g_base), "REC_%04d", i);
            ESP_LOGI(TAG, "ファイル名 %s.mp4(SD 内の最大=%d)", g_base, ondisk);
            return;
        }
    }
    g_last_num = 9999;
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
    uint32_t bi0 = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    if (esp_audio_enc_open(&cfg, &g_aac) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "AAC エンコーダを開けない");
        return ESP_FAIL;
    }
    /* コーデック本体の確保量。internal から減っていれば作業バッファが internal、
     * 減っていなければ PSRAM に落ちている(AAC が遅い原因の候補)。 */
    ESP_LOGI(TAG, "AAC open で internal %uKB -> %uKB(差分 %uKB)",
             bi0, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(bi0 - heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    if (esp_audio_enc_get_frame_size(g_aac, &g_in_bytes, &g_out_bytes) != ESP_AUDIO_ERR_OK) {
        esp_audio_enc_close(g_aac);
        g_aac = NULL;
        return ESP_FAIL;
    }
    /* PCM の部屋は AAC 1 フレーム分の 2 倍。ちょうどの大きさだと
     * (1023 ペアで止まる不具合)読み出しが永久に止まる。 */
    g_pcm_cap = g_in_bytes * 2;
    /* internal は 960x640 の H264 が 1 本 69KB の連続ブロックを要求するので空けておく。
     * ここは malloc のまま(2KB 超は PSRAM へ流れる)にして、エンコーダの確保を優先する。 */
    g_pcm = malloc((size_t)g_pcm_cap + 8);
    g_aacbuf = malloc((size_t)g_out_bytes + 64);
    if (!g_pcm || !g_aacbuf) {
        return ESP_ERR_NO_MEM;
    }
    /* aac -> mux の受け渡し。4 枠あれば mux が SD で 200ms 止まっても持てる。 */
    g_aq_free = xQueueCreate(A_QSLOTS, sizeof(int));
    g_aq_full = xQueueCreate(A_QSLOTS, sizeof(aac_item_t));
    if (g_aq_free == NULL || g_aq_full == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < A_QSLOTS; i++) {
        g_abuf[i] = malloc((size_t)g_out_bytes + 64);
        if (g_abuf[i] == NULL || xQueueSend(g_aq_free, &i, 0) != pdTRUE) {
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "AAC in=%d out=%d バイト、部屋は %s",
             g_in_bytes, g_out_bytes,
             esp_ptr_internal(g_pcm) ? "internal" : "PSRAM");
    return ESP_OK;
}

static void rec_free(void)
{
    g_aac_run = false;
    free(g_pcm);
    free(g_aacbuf);
    free(g_avcc);
    g_pcm = NULL;
    g_aacbuf = NULL;
    for (int i = 0; i < A_QSLOTS; i++) {
        free(g_abuf[i]);
        g_abuf[i] = NULL;
    }
    if (g_aq_free) {
        vQueueDelete(g_aq_free);
        g_aq_free = NULL;
    }
    if (g_aq_full) {
        vQueueDelete(g_aq_full);
        g_aq_full = NULL;
    }
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
    g_vpts_first = g_vpts_last = g_apts_first = g_apts_last = 0;
    g_a_reads = g_a_pairs = g_a_enc = g_a_zero = g_a_drop = 0;
    g_add_v_n = g_add_v_max = g_add_v_sum = 0;
    g_add_a_n = g_add_a_max = g_add_a_sum = 0;
    g_aac_n = g_aac_max = g_aac_sum = 0;
    g_flush_us = g_close_us = 0;
    g_fw_n = g_fw_max = g_fw_sum = 0;
    g_fw_bytes = 0;
    g_fw_maxlen = 0;
    g_fw_err = 0;
    g_sd_fail = false;
    g_seek_n = 0;
    g_seek_last = 0;
    g_nal_dbg = 0;
    g_pts_dbg = 0;
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
    /* AAC 符号化のタスク。core0 の低優先度で、PCM を読み続けて AAC にする。
     * mux(core1)の SD 書き込みに引っ張られないため別コアに置く。
     * 2026-10-19 r98 は mux が止まっている間 AAC が作れず音が欠けた。 */
    g_aac_run = true;
    g_aac_task = (TaskHandle_t)1; /* 生成前に NULL と区別できるようにする */
    if (xTaskCreatePinnedToCore(aac_task, "aac", 8192, NULL, 2, &g_aac_task, 0) != pdPASS) {
        g_aac_run = false;
        g_aac_task = NULL;
        rec_free();
        return ESP_ERR_NO_MEM;
    }
    /* SD 書き込みだけ。AAC は上のタスクが作るので優先度は rx より下でよい。
     * enc(core0 prio4)より下にして、符号化待ちで enc を止めないようにする。 */
    xTaskCreatePinnedToCore(mux_task, "mux", 12288, NULL, 2, &g_task, 1);
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
    /* mp4 の長さが実時間と合っているかを見る。映像フレーム数と pts の幅、
     * 音声フレーム数と pts の幅を並べて出す(1 フレーム = 16.74ms)。 */
    ESP_LOGI(TAG, "停止ログ %s.mp4 映像=%u 先頭pts=%.2fs 末尾pts=%.2fs | 音声=%u 先頭pts=%.2fs 末尾pts=%.2fs",
             g_base, (unsigned)g_vpkts,
             (double)g_vpts_first / 1000.0, (double)g_vpts_last / 1000.0,
             (unsigned)g_apkts,
             (double)g_apts_first / 1000.0, (double)g_apts_last / 1000.0);
    /* スケールが 1 枚どれくらいだったか。USB 受信と同時だとどこが膨らむかを見る */
    gbac_enc_prof_report();
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
