#include "usb_host_nano.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include "soc/usb_dwc_struct.h"

static const char *TAG = "usbnano";

#define VID_GBAC     0x1209
#define PID_GBAC     0xC001
#define EP_CMD_OUT   0x05
#define XFER_BYTES   512          /* bulk IN の MPS。484B は短_packets で完了する */
#define RX_PER_EP    8            /* EP 1 本あたりに積み続ける転送数 */
#define MAX_RX_EP    3

static gbac_pkt_cb g_cb;
static void *g_cb_arg;
static usb_host_client_handle_t g_client;
static usb_device_handle_t g_dev;
static volatile bool g_ready;
static uint32_t g_pkts;
static uint32_t g_errs;

static usb_transfer_t *g_rx[MAX_RX_EP][RX_PER_EP];
static int g_rx_ep[MAX_RX_EP];    /* 実際の EP アドレス(0x84 など) */
static int g_rx_n;

/* --- ping 応答(EP4 IN に返ってくる 8 バイト)を横取りする --- */
static uint8_t g_reply[16];
static int g_reply_len;
static volatile bool g_reply_wait;
static SemaphoreHandle_t g_reply_sem;

static void rx_cb(usb_transfer_t *t)
{
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        int len = t->actual_num_bytes;
        if (g_reply_wait && len >= 8 && t->data_buffer[0] == 'G') {
            g_reply_len = len <= (int)sizeof(g_reply) ? len : (int)sizeof(g_reply);
            memcpy(g_reply, t->data_buffer, g_reply_len);
            g_reply_wait = false;
            xSemaphoreGive(g_reply_sem);
        } else if (g_cb && len > 0) {
            g_cb(t->data_buffer, len, g_cb_arg);
        }
        g_pkts++;
    } else if (t->status == USB_TRANSFER_STATUS_NO_DEVICE ||
               t->status == USB_TRANSFER_STATUS_CANCELED) {
        g_errs++;
        return;     /* 抜かれた。再接続まで積まない */
    } else {
        g_errs++;
    }

    if (g_dev != NULL && usb_host_transfer_submit(t) != ESP_OK) {
        g_errs++;
    }
}

static void attach(uint8_t addr)
{
    const usb_device_desc_t *dd;
    const usb_config_desc_t *cd;

    if (g_dev != NULL) {
        ESP_LOGW(TAG, "2 つ目のデバイスは無視");
        return;
    }
    if (usb_host_device_open(g_client, addr, &g_dev) != ESP_OK) {
        ESP_LOGE(TAG, "device_open 失敗");
        g_dev = NULL;
        return;
    }
    if (usb_host_get_device_descriptor(g_dev, &dd) != ESP_OK) {
        ESP_LOGE(TAG, "device_desc 失敗");
        return;
    }
    ESP_LOGI(TAG, "接続 %04x:%04x", dd->idVendor, dd->idProduct);
    if (dd->idVendor != VID_GBAC || dd->idProduct != PID_GBAC) {
        ESP_LOGE(TAG, "nanoCH32V305(1209:C001)ではない。受けない");
        usb_host_device_close(g_client, g_dev);
        g_dev = NULL;
        return;
    }
    if (usb_host_interface_claim(g_client, g_dev, 0, 0) != ESP_OK) {
        ESP_LOGE(TAG, "interface_claim 失敗");
        return;
    }
    if (usb_host_get_active_config_descriptor(g_dev, &cd) != ESP_OK) {
        ESP_LOGE(TAG, "config_desc 失敗");
        return;
    }

    /* インタフェース 0 の bulk EP を集める。
     * config descriptor のヘッダの後ろを bLength ずつ歩く。 */
    g_rx_n = 0;
    int ep_out = 0;
    const uint8_t *p = (const uint8_t *)cd + cd->bLength;
    int left = (int)cd->wTotalLength - (int)cd->bLength;

    while (left >= 2) {
        const usb_standard_desc_t *d = (const usb_standard_desc_t *)p;
        if (d->bLength < 2) {
            break;
        }
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)d;
            int type = ep->bmAttributes & 0x03;
            if (type == 0x02) {           /* bulk */
                if (ep->bEndpointAddress & 0x80) {
                    if (g_rx_n < MAX_RX_EP) {
                        g_rx_ep[g_rx_n++] = ep->bEndpointAddress;
                    }
                } else {
                    ep_out = ep->bEndpointAddress;
                }
            }
        }
        left -= d->bLength;
        p += d->bLength;
    }
    ESP_LOGI(TAG, "bulk IN %d 本(%02x %02x %02x) OUT %02x",
             g_rx_n, g_rx_ep[0], g_rx_ep[1], g_rx_ep[2], ep_out);
    if (g_rx_n == 0) {
        return;
    }

    /* 転送の確保(初回だけ) */
    for (int i = 0; i < g_rx_n; i++) {
        for (int j = 0; j < RX_PER_EP; j++) {
            if (g_rx[i][j] == NULL) {
                if (usb_host_transfer_alloc(XFER_BYTES, 0, &g_rx[i][j]) != ESP_OK) {
                    ESP_LOGE(TAG, "transfer_alloc 失敗");
                    return;
                }
            }
            usb_transfer_t *t = g_rx[i][j];
            t->device_handle = g_dev;
            t->bEndpointAddress = g_rx_ep[i];
            t->callback = rx_cb;
            t->context = NULL;
            t->num_bytes = XFER_BYTES;
            t->timeout_ms = 0;
            if (usb_host_transfer_submit(t) != ESP_OK) {
                g_errs++;
            }
        }
    }
    g_ready = true;
    ESP_LOGI(TAG, "受注開始");
}

static void client_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        attach(msg->new_dev.address);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        ESP_LOGW(TAG, "nano が抜かれた");
        g_ready = false;
        usb_host_interface_release(g_client, msg->dev_gone.dev_hdl, 0);
        usb_host_device_close(g_client, msg->dev_gone.dev_hdl);
        g_dev = NULL;
    }
}

static void client_task(void *arg)
{
    (void)arg;
    for (;;) {
        usb_host_client_handle_events(g_client, portMAX_DELAY);
    }
}

esp_err_t gbac_usb_start(gbac_pkt_cb cb, void *arg)
{
    g_cb = cb;
    g_cb_arg = arg;
    g_reply_sem = xSemaphoreCreateBinary();

    /* ESP32-P4 では peripheral_map=0 が HighSpeed 側(UTMI)。
     * FullSpeed(GPIO26/27)は peripheral 1 なので、ここでは触らない。 */
    usb_host_config_t hcfg = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t e = usb_host_install(&hcfg);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "usb_host_install %s", esp_err_to_name(e));
        return e;
    }

    usb_host_client_config_t cl = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = client_cb,
            .callback_arg = NULL,
        },
    };
    e = usb_host_client_register(&cl, &g_client);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "client_register %s", esp_err_to_name(e));
        return e;
    }
    xTaskCreatePinnedToCore(client_task, "usbh", 4096, NULL, 4, NULL, 0);
    return ESP_OK;
}

int gbac_usb_ready(void)
{
    return g_ready ? 1 : 0;
}

/* DWC2(HS, 0x50000000)のレジスタを直接読む。ホストライブラリの内部なので
 * ポート状態を見る公開 API がない。読み取りだけなので起動中も安全。 */
void gbac_usb_dump_port(void)
{
    usb_dwc_hprt_reg_t h = USB_DWC_HS.hprt_reg;
    usb_dwc_gusbcfg_reg_t g = USB_DWC_HS.gusbcfg_reg;
    usb_dwc_gotgctl_reg_t o = USB_DWC_HS.gotgctl_reg;

    ESP_LOGI(TAG, "HPRT0=%08x CS=%d ENA=%d PWR=%d LNSTS=%d SPD=%d OVC=%d SUSP=%d RST=%d",
             h.val, h.prtconnsts, h.prtena, h.prtpwr, h.prtlnsts, h.prtspd,
             h.prtovrcurract, h.prtsusp, h.prtrst);
    ESP_LOGI(TAG, "GUSBCFG=%08x phyif=%d ulpiutmisel=%d physel=%d forcehst=%d forcedev=%d",
             g.val, g.phyif, g.ulpiutmisel, g.physel, g.forcehstmode, g.forcedevmode);
    ESP_LOGI(TAG, "GOTGCTL=%08x asesvld=%d bsesvld=%d curmod=%d",
             o.val, o.asesvld, o.bsesvld, o.curmod);
    ESP_LOGI(TAG, "GSNPSID=%08x HFNUM=%08x GINTSTS=%08x GINTMSK=%08x",
             USB_DWC_HS.gsnpsid_reg.val, USB_DWC_HS.hfnum_reg.val,
             USB_DWC_HS.gintsts_reg.val, USB_DWC_HS.gintmsk_reg.val);
}

/* EP5 OUT に "GBAC"+コマンドを送る */
static esp_err_t send_cmd(const uint8_t *buf, int len)
{
    if (!g_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    static usb_transfer_t *xfer;
    if (xfer == NULL) {
        if (usb_host_transfer_alloc(64, 0, &xfer) != ESP_OK) {
            return ESP_ERR_NO_MEM;
        }
    }
    memcpy(xfer->data_buffer, buf, len);
    xfer->device_handle = g_dev;
    xfer->bEndpointAddress = EP_CMD_OUT;
    xfer->num_bytes = len;
    xfer->timeout_ms = 500;
    xfer->callback = NULL;
    xfer->context = NULL;
    return usb_host_transfer_submit(xfer);
}

static esp_err_t simple_cmd(char c)
{
    uint8_t b[5] = { 'G', 'B', 'A', 'C', (uint8_t)c };
    return send_cmd(b, 5);
}

esp_err_t gbac_usb_stream_start(void)
{
    return simple_cmd('S');
}

esp_err_t gbac_usb_stream_stop(void)
{
    return simple_cmd('X');
}

esp_err_t gbac_usb_ping(int *fw_ver)
{
    if (!g_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    g_reply_wait = true;
    g_reply_len = 0;
    esp_err_t e = simple_cmd('P');
    if (e != ESP_OK) {
        g_reply_wait = false;
        return e;
    }
    if (xSemaphoreTake(g_reply_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
        g_reply_wait = false;
        return ESP_ERR_TIMEOUT;
    }
    if (fw_ver && g_reply_len >= 7) {
        *fw_ver = g_reply[6];
    }
    return ESP_OK;
}

uint32_t gbac_usb_packets(void)
{
    return g_pkts;
}

uint32_t gbac_usb_errors(void)
{
    return g_errs;
}
