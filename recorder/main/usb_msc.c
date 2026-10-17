/* PC に microSD を見せる(USB MSC デバイス)。docs/recorder-wiring-esp32p4.md の
 * SS14 結線(P1 の VBUS → SS14 → VSYS)で PC と結び、Explorer に SD を出す。
 *
 * ESP32-P4 の USB-OTG(HighSpeed・UTMI・D-=GPIO49/D+=GPIO50)は
 * ホスト(nano 受信)とデバイス(MSC)の一個取り。切り替えの手順:
 *   1) gbac_usb_stop() でホストを完全に畳む(usb_host_uninstall が PHY を返す)
 *   2) gbac_sd_fat_detach() でこちらの FATFS 登録だけ外す(カードは生かす)
 *   3) tinyusb_msc_storage_init_sdmmc() に同じカードを渡す
 *      = FAT をマウントしないので PC 側が見られる状態になる
 *   4) tinyusb_driver_install() で TinyUSB を HS デバイスとして起こす
 * 戻すときはこの逆。PC が抜いた瞬間に TinyUSB 側が自動で FAT を
 * 再マウントすることがあるので、必ず tinyusb_msc_storage_unmount() で
 * 返してからこちらの /sd に付け替える(同じカードに FATFS を 2 個載せない)。
 */
#include "usb_msc.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"

#include "sd_recorder.h"
#include "usb_host_nano.h"

static const char *TAG = "msc";

/* ---- TinyUSB のディスクリプタ(HS デバイス) --------------------- */
enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL
};
enum {
    EDPT_MSC_OUT = 0x01,
    EDPT_MSC_IN  = 0x81,
};
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

/* VID/PID は Espressif 割り当て(0x303A:0x4002 = MSC デモ)。
 * nano の 1209:C001 とは別物なので、PC 側は別の機器として認識する。 */
static tusb_desc_device_t s_dev_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4002,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const tusb_desc_device_qualifier_t s_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0,
};

static uint8_t const s_cfg_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};

static uint8_t const s_cfg_hs[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 512),
};

static char const *s_str_arr[] = {
    (const char[]) { 0x09, 0x04 },   /* 0: 言語 = 英語 */
    "GBAcapture",                    /* 1: メーカー */
    "GBAcapture microSD",            /* 2: 製品名 */
    "000001",                        /* 3: シリアル */
};

/* ---- MSC ストレージのイベント(ログだけ) ------------------------- */
static void msc_mount_cb(tinyusb_msc_event_t *ev)
{
    ESP_LOGI(TAG, "MSC: 向こう側の FAT マウント=%d", (int)ev->mount_changed_data.is_mounted);
}

static int s_on;          /* PC に見せている最中 */
static int s_storage;     /* tinyusb_msc_storage_init_* した数(deinit と対) */

int gbac_msc_active(void)
{
    return s_on;
}

/* MSC を畳む。順番は uninstall → unmount → deinit。
 *
 * esp_tinyusb の tud_umount_cb() は s_storage_handle->base_path を読む。
 * TinyUSB を畳むと tud_deinit() は「PC に認識されていた」(cfg_num > 0) ときだけ
 * このコールバックを呼ぶ。だからストレージを先に畳んで handle を NULL にすると、
 * PC につないでいたときだけ NULL を読んで Load access fault で落ちる
 * (実測 2026-10-16、MEPC = tud_umount_cb、MTVAL = 0x2010)。
 * handle が生きているうちに uninstall する。
 *
 * uninstall の中で tud_umount_cb が /sd へ FAT を自動で再マウントするので、
 * そのあと unmount でそれを返す(同じカードに FATFS を 2 個載せない)。
 * unmount は未マウントなら ESP_OK を返すので、PC をつないでいなかったときは
 * 何も起きない。 */
static esp_err_t msc_teardown(void)
{
    esp_err_t e = ESP_OK;
    esp_err_t e2 = tinyusb_driver_uninstall();
    if (e2 != ESP_OK) {
        /* esp_tinyusb 1.7.x は tusb_teardown()(= tusb_deinit(0))を呼ぶが、
         * P4 の HS は rhport1 なので必ず失敗する。
         * recorder/scripts/patch_esp_tinyusb.ps1 を当てていないとここが壊れる。 */
        ESP_LOGE(TAG, "tinyusb_driver_uninstall %s (patch_esp_tinyusb.ps1 は当たっている?)",
                 esp_err_to_name(e2));
        e = e2;
    }
    if (s_storage) {
        esp_err_t e1 = tinyusb_msc_storage_unmount();
        if (e1 != ESP_OK) {
            ESP_LOGW(TAG, "MSC storage unmount %s", esp_err_to_name(e1));
        }
        tinyusb_msc_storage_deinit();
        s_storage = 0;
    }
    return e;
}

esp_err_t gbac_msc_start(void)
{
    if (s_on) {
        return ESP_OK;
    }
    sdmmc_card_t *card = gbac_sd_card();
    if (card == NULL) {
        ESP_LOGE(TAG, "SD が初期化されていないので MSC にできない");
        return ESP_ERR_INVALID_STATE;
    }

    /* 1) ホストを畳んで OTG PHY を空ける */
    esp_err_t e = gbac_usb_stop();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "ホストが畳めない(%s)ので MSC へ切り替えない", esp_err_to_name(e));
        return e;
    }

    /* 2) こちらの FATFS 登録だけ外す(カードは初期化済みのまま) */
    e = gbac_sd_fat_detach();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "FAT が切り離せない(%s)", esp_err_to_name(e));
        goto back_host;
    }

    /* 3) MSC ストレージへカードを渡す。FAT をマウントしない = PC に見える */
    tinyusb_msc_sdmmc_config_t scfg = {
        .card = card,
        .callback_mount_changed = msc_mount_cb,
        .callback_premount_changed = msc_mount_cb,
        .mount_config = {
            .format_if_mount_failed = false,
            .max_files = 4,
            .allocation_unit_size = 64 * 1024,
        },
    };
    e = tinyusb_msc_storage_init_sdmmc(&scfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "MSC ストレージ初期化失敗(%s)", esp_err_to_name(e));
        goto back_fat;
    }
    s_storage = 1;

    /* 4) TinyUSB を HS デバイスとして起こす */
    tinyusb_config_t tcfg = {
        .device_descriptor = &s_dev_desc,
        .string_descriptor = s_str_arr,
        .string_descriptor_count = (int)(sizeof(s_str_arr) / sizeof(s_str_arr[0])),
        .external_phy = false,
#if TUD_OPT_HIGH_SPEED
        .fs_configuration_descriptor = s_cfg_fs,
        .hs_configuration_descriptor = s_cfg_hs,
        .qualifier_descriptor = &s_qualifier,
#else
        .configuration_descriptor = s_cfg_fs,
#endif
        .self_powered = false,
        .vbus_monitor_io = 0,
    };
    e = tinyusb_driver_install(&tcfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install %s", esp_err_to_name(e));
        tinyusb_msc_storage_deinit();
        s_storage = 0;
        goto back_fat;
    }

    s_on = 1;
    ESP_LOGI(TAG, "PC に microSD を見せた(303A:4002)。Explorer に出るまで数秒");
    ESP_LOGI(TAG, "外して録画に戻るときは 'm'");
    return ESP_OK;

back_fat:
    gbac_sd_fat_attach();
back_host:
    {
        esp_err_t e2 = gbac_usb_start(NULL, NULL);
        ESP_LOGW(TAG, "ホストへ戻す(%s)", esp_err_to_name(e2));
    }
    return e;
}

esp_err_t gbac_msc_stop(void)
{
    if (!s_on) {
        return ESP_OK;
    }
    esp_err_t e = msc_teardown();
    s_on = 0;

    /* こちらの /sd へ FATFS を付け替える */
    esp_err_t e2 = gbac_sd_fat_attach();
    if (e2 != ESP_OK) {
        ESP_LOGE(TAG, "SD を /sd へ戻せない(%s)", esp_err_to_name(e2));
        e = (e == ESP_OK) ? e2 : e;
        return e;
    }
    ESP_LOGI(TAG, "SD を /sd に戻した。ホスト復旧は呼び出し側で gbac_usb_start");
    return e;
}
