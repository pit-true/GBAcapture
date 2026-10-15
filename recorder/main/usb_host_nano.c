#include "usb_host_nano.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include "gbacap.h"        /* GBAC_PACKET_SIZE / マジック / 行数 */

static const char *TAG = "usbnano";

#define VID_GBAC     0x1209
#define PID_GBAC     0xC001
#define EP_CMD_OUT   0x05
#define XFER_BYTES   512          /* bulk IN の MPS。484B は短_packets で完了する */
#define RX_PER_EP    32           /* EP 1 本あたりに積み続ける転送数。
                                   * 8 の実測で毎秒 2.0MB(60fps に必要は 6.9MB)だったので
                                   * インフライトを増してバスを空けないようにする */
#define MAX_RX_EP    3

static gbac_pkt_cb g_cb;
static void *g_cb_arg;
static usb_host_client_handle_t g_client;
static usb_device_handle_t g_dev;
static volatile bool g_ready;
/* MSC へ切り替えるとき、ホスト側を素直に畳むための合図。
 * usb_host_uninstall は「client が全解除」「lib のイベント残が 0」でないと
 * ESP_ERR_INVALID_STATE で弾くので、client/lib のタスクは自分のループを
 * 抜けてから消えなければならない(抜ける合図は unblock で起こす)。 */
static volatile bool g_stop_tasks;
static volatile bool g_client_dead, g_lib_dead;
static TaskHandle_t g_rx_task;
static uint32_t g_pkts;
static uint32_t g_errs;
static uint32_t g_bytes;      /* 転送で受け取ったバイト数の合計 */
static uint32_t g_ep_pkts[MAX_RX_EP];   /* EP ごとの完了転送数(どれが黙っているか見る) */
static int g_xfer_dbg;        /* 最初の 4 転送の中身をログに出す */

/* rx タスクがパケットを処理した時間(µs 累計)。1 パケットの原価を測るため。
 * esp_timer_get_time は mtime レジスタ読みで軽いので毎パケット測る。 */
static volatile int64_t g_prof_v_us, g_prof_a_us;
static volatile uint32_t g_prof_v_n, g_prof_a_n;

static usb_transfer_t *g_rx[MAX_RX_EP][RX_PER_EP];
static int g_rx_ep[MAX_RX_EP];    /* 実際の EP アドレス(0x84 など) */
static int g_rx_n;
static int g_ep_out;              /* コマンド用の OUT EP(0x05)。後始末で止める */

/* --- ping 応答(EP4 IN に返ってくる 8 バイト)を横取りする --- */
static uint8_t g_reply[16];
static int g_reply_len;
static volatile bool g_reply_wait;
static SemaphoreHandle_t g_reply_sem;

/* nano はパケットを USB の 512B 境界に揃えず、連続したバイト列として流す。
 * なので 1 転送 = 1 パケットでは扱えない(PC 版 ViewerForm.cs の RxLoop と
 * 同じ再組み立てをする)。EP ごとに独立したバッファへ追記し、
 *   [0xA5][frame][line<160][0] → 484B のライン
 *   [0xA6][seq][n 1..120][0]   → 484B の音声
 * に一致する位置から 484B ずつ切り出して上に渡す。違えば 1 バイトずらして
 * 再同期し、端数は先頭へ詰める。転送の完了順は EP 内では保たれる。 */
#define REASM_BYTES (GBAC_PACKET_SIZE * 12)
static uint8_t g_reasm[MAX_RX_EP][REASM_BYTES];
static int g_reasm_len[MAX_RX_EP];
static uint32_t g_reasm_lost;   /* バッファ溢れで捨てたバイト数 */

/* 切り出した 484B パケットを渡すリング(SPSC)。
 * rx_cb(=usbh タスク、core0)はここへコピーするだけにして、ピクセル復号や
 * フレーム書き込みといった重い仕事は core1 の rx タスクでやる。
 * そうしないと usbh が IDLE0 を飢えさせて task_wdt が発火した(実測)。
 * 内部 RAM で 128 枠 = 62KB。14,500 パケット/秒なら約 9 ミリ秒ぶん。 */
#define RXRING_SLOTS 128
#define RXRING_MASK  (RXRING_SLOTS - 1)
static uint8_t g_ring[RXRING_SLOTS][GBAC_PACKET_SIZE] __attribute__((aligned(32)));
static volatile uint32_t g_wr;
static volatile uint32_t g_rd;
static uint32_t g_ring_drop;   /* リングが満杯で捨てたパケット */
/* リング投入の成否を EP 別・種別(映像/音声)別に数える。
 * r9 で ring の溢れが 6 万パケットを超え、音声の 6 秒分がここで消えている疑い
 * (監督 2026-10-16 02:57 指示)。音声は 683 パケット/秒だけなので、
 * 溢れに巻き込まれる割合を測る。 */
static uint32_t g_put_ok[MAX_RX_EP];
static uint32_t g_put_drop[MAX_RX_EP];
static uint32_t g_put_audio_ok;
static uint32_t g_put_audio_drop;
static SemaphoreHandle_t g_ring_sem;

static int ring_put(const uint8_t *p)
{
    uint32_t wr = g_wr;   /* 書き手は usbh タスクだけ */
    if (wr - __atomic_load_n(&g_rd, __ATOMIC_ACQUIRE) >= RXRING_SLOTS) {
        g_ring_drop++;
        return 0;
    }
    memcpy(g_ring[wr & RXRING_MASK], p, GBAC_PACKET_SIZE);
    __atomic_store_n(&g_wr, wr + 1, __ATOMIC_RELEASE);
    xSemaphoreGive(g_ring_sem);
    return 1;
}

static void reasm_feed(int ep, const uint8_t *src, int len)
{
    if (ep < 0 || ep >= MAX_RX_EP || len <= 0) {
        return;
    }
    uint8_t *b = g_reasm[ep];
    int n = g_reasm_len[ep];

    if (len > REASM_BYTES - n) {
        /* 埋まるほど溜まるのは同期が壊れている証拠。捨ててやり直す */
        g_reasm_lost += (uint32_t)n;
        n = 0;
        if (len > REASM_BYTES) {
            src += len - REASM_BYTES;
            len = REASM_BYTES;
        }
    }
    memcpy(b + n, src, (size_t)len);
    n += len;

    int pos = 0;
    while (n - pos >= GBAC_PACKET_SIZE) {
        const uint8_t *p = b + pos;
        if ((p[0] == GBAC_LINE_MAGIC && p[3] == 0 && p[2] < GBAC_LINES_PER_FRAME) ||
            (p[0] == GBAC_AUDIO_MAGIC && p[3] == 0 && p[2] > 0 && p[2] <= 120)) {
            int aud = (p[0] == GBAC_AUDIO_MAGIC);
            if (ring_put(p)) {
                g_put_ok[ep]++;
                if (aud) g_put_audio_ok++;
            } else {
                g_put_drop[ep]++;
                if (aud) g_put_audio_drop++;
            }
            pos += GBAC_PACKET_SIZE;
        } else {
            pos++;    /* マジック再同期 */
        }
    }
    if (pos > 0) {
        memmove(b, b + pos, (size_t)(n - pos));
    }
    g_reasm_len[ep] = n - pos;
}

static void rx_cb(usb_transfer_t *t)
{
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        int len = t->actual_num_bytes;
        g_bytes += (uint32_t)(len > 0 ? len : 0);
        for (int i = 0; i < g_rx_n; i++) {
            if (g_rx_ep[i] == t->bEndpointAddress) {
                g_ep_pkts[i]++;
                break;
            }
        }
        if (g_xfer_dbg < 4) {
            g_xfer_dbg++;
            ESP_LOGW(TAG, "xfer %d/4 ep=%02x st=%d len=%d %02x %02x %02x %02x %02x %02x %02x %02x",
                     g_xfer_dbg, t->bEndpointAddress, (int)t->status, len,
                     len > 0 ? t->data_buffer[0] : 0, len > 1 ? t->data_buffer[1] : 0,
                     len > 2 ? t->data_buffer[2] : 0, len > 3 ? t->data_buffer[3] : 0,
                     len > 4 ? t->data_buffer[4] : 0, len > 5 ? t->data_buffer[5] : 0,
                     len > 6 ? t->data_buffer[6] : 0, len > 7 ? t->data_buffer[7] : 0);
        }
        if (g_reply_wait && len >= 8 && t->data_buffer[0] == 'G') {
            g_reply_len = len <= (int)sizeof(g_reply) ? len : (int)sizeof(g_reply);
            memcpy(g_reply, t->data_buffer, g_reply_len);
            g_reply_wait = false;
            xSemaphoreGive(g_reply_sem);
        } else if (g_cb && len > 0) {
            reasm_feed((int)(intptr_t)t->context, t->data_buffer, len);
        }
        g_pkts++;
    } else if (t->status == USB_TRANSFER_STATUS_NO_DEVICE ||
               t->status == USB_TRANSFER_STATUS_CANCELED) {
        g_errs++;
        return;     /* 抜かれた。再接続まで積まない */
    } else {
        g_errs++;
    }

    if (!g_stop_tasks && g_dev != NULL && usb_host_transfer_submit(t) != ESP_OK) {
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
                    ep_out = g_ep_out = ep->bEndpointAddress;
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
    /* 再接続時は前回の取りこぼしを継がない */
    for (int i = 0; i < g_rx_n; i++) {
        g_reasm_len[i] = 0;
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
            t->context = (void *)(intptr_t)i;   /* reasm_feed の EP 番号 */
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

/* EP に積んだままの転送を降ろす。
 * usb_host_interface_release は EP に in-flight URB が 1 本でもあると
 * ESP_ERR_INVALID_STATE(usb_host.c:1397)、usb_host_client_deregister は
 * client の idle_ep_tailq が空だと同じく弾く(同 872)。
 * つまり IN 転送を 3 本×3 EP に積んだままでは後始末に進めない。
 * HALT → FLUSH でパイプを空にすると、積んでいた URB は
 * status=CANCELED でコールバックへ返る(hcd_dwc.c:1820)。 */
static void flush_ep(usb_device_handle_t dev, uint8_t ep)
{
    esp_err_t e1 = usb_host_endpoint_halt(dev, ep);
    esp_err_t e2 = usb_host_endpoint_flush(dev, ep);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        ESP_LOGW(TAG, "EP %02x halt=%s flush=%s", ep, esp_err_to_name(e1), esp_err_to_name(e2));
    }
}

static void client_task(void *arg)
{
    (void)arg;
    for (;;) {
        usb_host_client_handle_events(g_client, portMAX_DELAY);
        if (!g_stop_tasks) {
            continue;
        }
        /* 抜ける合図が来た。転送の完了コールバックは handle_events の中で
         * しか呼ばれないので、流した URB はここで拾い切る。 */
        if (g_dev != NULL) {
            for (int i = 0; i < g_rx_n; i++) {
                flush_ep(g_dev, (uint8_t)g_rx_ep[i]);
            }
            if (g_ep_out) {
                flush_ep(g_dev, (uint8_t)g_ep_out);
            }
        }
        for (int i = 0; i < 10; i++) {
            usb_host_client_handle_events(g_client, pdMS_TO_TICKS(20));
        }
        if (g_dev != NULL) {
            esp_err_t e = usb_host_interface_release(g_client, g_dev, 0);
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "interface_release %s", esp_err_to_name(e));
            }
            e = usb_host_device_close(g_client, g_dev);
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "device_close %s", esp_err_to_name(e));
            }
            g_dev = NULL;
        }
        g_ready = false;
        /* client の登録解除も自分のタスクから行うのが IDF の流儀
         * (handle_events の途中でないと handling_events フラグが下りない) */
        esp_err_t e = usb_host_client_deregister(g_client);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "client_deregister %s", esp_err_to_name(e));
        }
        g_client = NULL;
        g_client_dead = true;
        vTaskDelete(NULL);
    }
}

/* リングからパケットを取り出して映像・音声のパイプへ流すタスク。
 * ピクセル復号(1 ライン 240 語)やフレームバッファへの書き込みは重いので、
 * USB 側のタスクではなく core1 でやる(enc と同じ側)。 */
static void rx_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(g_ring_sem, portMAX_DELAY);
        while (g_rd != __atomic_load_n(&g_wr, __ATOMIC_ACQUIRE)) {
            const uint8_t *p = g_ring[g_rd & RXRING_MASK];
            if (g_cb) {
                int64_t t0 = esp_timer_get_time();
                g_cb(p, GBAC_PACKET_SIZE, g_cb_arg);
                int64_t d = esp_timer_get_time() - t0;
                if (p[0] == GBAC_LINE_MAGIC) {
                    g_prof_v_us += d;
                    g_prof_v_n++;
                } else {
                    g_prof_a_us += d;
                    g_prof_a_n++;
                }
            }
            __atomic_store_n(&g_rd, g_rd + 1, __ATOMIC_RELEASE);
        }
    }
}

/* ホストライブラリ本体のイベントループ。これが無いと HCD のポート状態変化
 * (接続/切断)が処理されず、列挙も client へのイベントも一切起きない。
 * IDF の例でいう usb_lib_task。client より高い優先度で回す。 */
static void usb_lib_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags = 0;
        esp_err_t e = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (e != ESP_OK && e != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "lib_handle_events %s", esp_err_to_name(e));
        }
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            /* client が全滅したらデバイスを閉じる(IDF の例と同じ) */
            usb_host_device_free_all();
        }
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "全デバイス解放済み");
        }
        if (g_stop_tasks) {
            /* uninstall は process_pending_flags / lib_event_flags / flags が
             * 全部 0 でないと弾かれる。残イベントをここで消化してから抜ける。
             * さらに usb_host_uninstall の中の usbh_uninstall() はデバイスが
             * 1 つでも残っていると ESP_ERROR_CHECK で止まる。
             * なので NO_CLIENTS → device_free_all → ALL_FREE までを
             * 見届けてから消える(待っている間も自分がイベントを処理する)。 */
            bool all_free = false;
            for (int i = 0; i < 20; i++) {
                uint32_t f2 = 0;
                esp_err_t e2 = usb_host_lib_handle_events(pdMS_TO_TICKS(30), &f2);
                if (f2 & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
                    usb_host_device_free_all();
                }
                if (f2 & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
                    all_free = true;
                }
                if (e2 == ESP_ERR_TIMEOUT && all_free) {
                    break;      /* 取りこぼしなし */
                }
            }
            if (!all_free) {
                ESP_LOGW(TAG, "デバイス解放の完了が見えなかった");
            }
            g_lib_dead = true;
            vTaskDelete(NULL);
        }
    }
}

esp_err_t gbac_usb_start(gbac_pkt_cb cb, void *arg)
{
    /* cb が NULL のときは前のコールバックをそのまま使う
     * (MSC からのロールバックでホストだけ戻すときに渡すため) */
    if (cb) {
        g_cb = cb;
        g_cb_arg = arg;
    }
    g_stop_tasks = false;
    g_client_dead = false;
    g_lib_dead = false;
    if (g_reply_sem == NULL) {
        g_reply_sem = xSemaphoreCreateBinary();
    }
    if (g_ring_sem == NULL) {
        g_ring_sem = xSemaphoreCreateCounting(RXRING_SLOTS, 0);
    }
    if (g_ring_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* MSC から戻ってきたときはリングに残った古いパケットを捨てて始める
     * (rx タスクは生きたまま sem を待っているだけなので、読み位置を進めるだけ) */
    g_rd = __atomic_load_n(&g_wr, __ATOMIC_ACQUIRE);
    while (xSemaphoreTake(g_ring_sem, 0) == pdTRUE) {
    }
    /* rx タスクは 1 本だけ。停止しても消さないので、使い回す。
     * 重い仕事(復号・フレーム書き込み)は core1 の rx でやる。
     * 優先度は enc(5) より下にする: rx(6) のときは rx が core1 を占有して
     * enc が動かず IDLE1 まで飢えて task_wdt が出た(2026-10-16 実測)。
     * リングが 128 枠(約 9ms 分)あるので抜かれた分は次のチャンスにまとめて処理する。 */
    if (g_rx_task == NULL) {
        if (xTaskCreatePinnedToCore(rx_task, "rx", 8192, NULL, 4, &g_rx_task, 1) != pdPASS) {
            ESP_LOGE(TAG, "rx タスク作成失敗");
            return ESP_ERR_NO_MEM;
        }
    }

    /* ESP32-P4 では peripheral_map=0 が HighSpeed 側(UTMI)。
     * FullSpeed(GPIO26/27)は peripheral 1 なので、ここでは触らない。 */
    usb_host_config_t hcfg = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    /* DWC2 の FIFO バイアスは sdkconfig で BALANCED にしてある。
     * HS(hsphy_type!=0)だと otg_dfifo_depth=1024 で nptx=256 行(1024B)、
     * rx = fifo_size-384 行になり、nano の EP(全部 512B)を扱える。
     * 既定の BIAS_IN だと nptx=64 行=256B で "EP MPS (512) exceeds supported
     * limit (256)" となり interface_claim が失敗した(実機で確認)。
     * fifo_settings_custom は使わない(総行数のレジスタは install 前に読めない)。 */
    esp_err_t e = usb_host_install(&hcfg);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "usb_host_install %s", esp_err_to_name(e));
        return e;
    }

    /* インストールできたら、まずライブラリ自身のイベントループを動かす。
     * これが無いとポートの接続イベントが誰も処理しない。 */
    /* ホストライブラリのイベントループは最優先級(core0)。
     * mux(SD 書き込み)は core1 へ逃がしてあるので、ここでバスを空けない。
     * esp_timer タスクが 22 なので、usb_lib は同じ 22(実質張り付き)、
     * client(転送完了コールバック)は 21。 */
    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL, 22, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "usb_lib タスク作成失敗");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "USB ホスト開始(HS/UTMI)");

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
    xTaskCreatePinnedToCore(client_task, "usbh", 4096, NULL, 21, NULL, 0);
    return ESP_OK;
}

/* ホスト側を完全に畳む。MSC へ USB コントローラ(OTG PHY)を渡すために必要。
 * 手順は IDF の後始末の流儀どおり:
 *   1) nano へ 'X' を送って送信を止める(抜けなくても構わない)
 *   2) client タスクへ合図して unblock で起こす。client 自身が
 *      interface_release → device_close → 取消転送の回収 → deregister する
 *   3) lib タスクへ合図して unblock。残イベントを消化させてから抜けさせる
 *   4) usb_host_uninstall() で hub/enum/usbh/HCD/PHY を解放する
 * rx タスクは消さない(次に gbac_usb_start したとき使い回す)。 */
esp_err_t gbac_usb_stop(void)
{
    if (g_client == NULL) {
        ESP_LOGW(TAG, "USB ホストはすでに畳んでいる");
        return ESP_ERR_INVALID_STATE;
    }
    gbac_usb_stream_stop();
    vTaskDelay(pdMS_TO_TICKS(20));      /* 'X' が飛びきるのを待つ */

    g_stop_tasks = true;
    usb_host_client_unblock(g_client);
    for (int i = 0; i < 100 && !g_client_dead; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!g_client_dead) {
        ESP_LOGE(TAG, "client タスクが抜けなかった");
        return ESP_ERR_TIMEOUT;
    }
    /* 転送オブジェクトは host ライブラリのプールから生えている。
     * uninstall 後に使い回すと壊れるので、ここで全部返して NULL にする
     * (attach でまた確保し直す)。全部完了済みでないと free は弾かれる。 */
    for (int i = 0; i < MAX_RX_EP; i++) {
        for (int j = 0; j < RX_PER_EP; j++) {
            if (g_rx[i][j] != NULL) {
                usb_host_transfer_free(g_rx[i][j]);
                g_rx[i][j] = NULL;
            }
        }
    }
    usb_host_lib_unblock();
    for (int i = 0; i < 100 && !g_lib_dead; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!g_lib_dead) {
        ESP_LOGE(TAG, "usb_lib タスクが抜けなかった");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t e = ESP_ERR_INVALID_STATE;
    for (int i = 0; i < 20 && e != ESP_OK; i++) {
        e = usb_host_uninstall();
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "usb_host_uninstall %s(リトライ)", esp_err_to_name(e));
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    g_ready = false;
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "USB ホストを畳んだ(OTG PHY を解放)");
    } else {
        ESP_LOGE(TAG, "USB ホストが畳めない(%s)", esp_err_to_name(e));
    }
    return e;
}

int gbac_usb_ready(void)
{
    return g_ready ? 1 : 0;
}

static void cmd_done_cb(usb_transfer_t *t)
{
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "コマンド送信失敗 status=%d", (int)t->status);
    }
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
    xfer->callback = cmd_done_cb;
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

uint32_t gbac_usb_bytes(void)
{
    return g_bytes;
}

uint32_t gbac_usb_ep_packets(int idx)
{
    return (idx >= 0 && idx < MAX_RX_EP) ? g_ep_pkts[idx] : 0;
}

int gbac_usb_ep_addr(int idx)
{
    return (idx >= 0 && idx < g_rx_n) ? g_rx_ep[idx] : 0;
}

int gbac_usb_ep_count(void)
{
    return g_rx_n;
}

uint32_t gbac_usb_packets(void)
{
    return g_pkts;
}

uint32_t gbac_usb_errors(void)
{
    return g_errs;
}

uint32_t gbac_usb_ring_dropped(void)
{
    return g_ring_drop;
}

uint32_t gbac_usb_reasm_lost(void)
{
    return g_reasm_lost;
}

/* rx リングへ入れた/捨てたパケット数。ep は g_rx_ep の添字(0..gbac_usb_ep_count-1)。
 * 音声(0xA6)は種別で別カウントして、溢れに音声がどれだけ巻き込まれているか見る。 */
void gbac_usb_put_stats(int ep, uint32_t *ok, uint32_t *drop)
{
    if (ep < 0 || ep >= MAX_RX_EP) {
        if (ok) *ok = 0;
        if (drop) *drop = 0;
        return;
    }
    if (ok) *ok = g_put_ok[ep];
    if (drop) *drop = g_put_drop[ep];
}

void gbac_usb_put_audio(uint32_t *ok, uint32_t *drop)
{
    if (ok) *ok = g_put_audio_ok;
    if (drop) *drop = g_put_audio_drop;
}

uint32_t gbac_usb_audio_dropped(void)
{
    return g_put_audio_drop;
}

/* rx タスクがパケット処理に使った時間(µs 累計)と処理個数。
 * 1 パケットの原価(=時間/個数)を出して、6.9MB/s に必要な 14.5k pkt/s を
 * 1 コアでさばけるか判断するため */
void gbac_usb_rx_prof(int64_t *video_us, int64_t *audio_us,
                      uint32_t *video_n, uint32_t *audio_n)
{
    if (video_us) *video_us = g_prof_v_us;
    if (audio_us) *audio_us = g_prof_a_us;
    if (video_n) *video_n = g_prof_v_n;
    if (audio_n) *audio_n = g_prof_a_n;
}
