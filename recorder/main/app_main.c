/* GBA キャプチャ録画機(ESP32-P4-Pico)の本体。
 *   nanoCH32V305(USB-HS) → 受信 → 2x 拡大(480x320) → HW H.264 + AAC → microSD の mp4
 * ボタン 1 回で録画開始、もう 1 回で停止。docs/recorder-plan-esp32p4.md 参照。 */
#include <stdio.h>

#include "audio_pipe.h"
#include "board.h"
#include "button.h"
#include "cfg_nvs.h"
#include "driver/uart.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/lp_system_reg.h"
#include "frame_pipe.h"
#include "gbacap.h"
#include "ppa_probe.h"
#include "sdkconfig.h"
#include "sd_recorder.h"
#include "usb_host_nano.h"
#include "usb_msc.h"
#include "video_enc.h"

static const char *TAG = "main";

static gbac_cfg_t g_cfg;
static bool g_recording;
/* 試験用: 'T' で録画を始めるとこの時刻に自動で止まる(0=使わない)。
 * 20 秒録画のログは途中でシリアルを開けず 1 回で取れるようにする。 */
static int64_t g_auto_stop_at;

static uint32_t g_pkt_unk;   /* マジバがどちらでもないパケット */
static int g_pkt_dbg;        /* 中身を一度だけログに出す回数 */

static void on_packet(const uint8_t *pkt, int len, void *arg)
{
    (void)arg;
    if (len > 0 && pkt[0] == GBAC_LINE_MAGIC) {
        frame_pipe_push(pkt, len);
    } else if (len > 0 && pkt[0] == GBAC_AUDIO_MAGIC) {
        audio_pipe_push(pkt, len);
    } else {
        g_pkt_unk++;
        if (g_pkt_dbg < 4) {
            g_pkt_dbg++;
            ESP_LOGW(TAG, "未知 pkt %d/4 len=%d %02x %02x %02x %02x %02x %02x %02x %02x",
                     g_pkt_dbg, len, pkt[0], len > 1 ? pkt[1] : 0, len > 2 ? pkt[2] : 0,
                     len > 3 ? pkt[3] : 0, len > 4 ? pkt[4] : 0, len > 5 ? pkt[5] : 0,
                     len > 6 ? pkt[6] : 0, len > 7 ? pkt[7] : 0);
        }
    }
}

/* 1 秒ごとの受信速度。録画・テストの「経過秒」と「直近 1 秒の bytes/s」を
 * ログに残して、速さを推定ではなく実測で示すため(2026-10-16)。
 * サンプルはメインループの 1 秒周期で取る(追加タスクは作らない)。 */
static int64_t g_run_us;        /* 録画/テスト開始時刻。0 = 止まっている */
static uint32_t g_run_bytes;    /* 開始時の受信バイト累計 */
static int64_t g_smp_us;        /* 前回サンプル時刻 */
static uint32_t g_smp_bytes, g_smp_pkts, g_smp_ring, g_smp_fok, g_smp_fdrop;
/* 音声入力側の 1 秒計測(2026-10-16)。
 * 32,802 ペア/秒 = 683.4 パケット/秒が rx 側から届いているか、
 * seq の飛び幅(=失われたパケット数)が 1 秒にどれだけ出るかを見る。 */
static uint32_t g_smp_apkt, g_smp_apair, g_smp_aseqlost, g_smp_aadrop;
static double g_rate_mbs;       /* 直近 1 秒の MB/s */

static void run_begin(void)
{
    g_run_us = esp_timer_get_time();
    g_run_bytes = gbac_usb_bytes();
    g_smp_us = g_run_us;
    g_smp_bytes = g_run_bytes;
    g_smp_pkts = gbac_usb_packets();
    g_smp_ring = gbac_usb_ring_dropped();
    g_smp_fok = frame_pipe_completed();
    g_smp_fdrop = frame_pipe_dropped();
    g_smp_apkt = audio_pipe_pushed_pkts();
    g_smp_apair = audio_pipe_pushed_pairs();
    g_smp_aseqlost = audio_pipe_seq_lost();
    g_smp_aadrop = 0;
    gbac_usb_put_audio(NULL, &g_smp_aadrop);
    g_rate_mbs = 0;
}

static void run_end(const char *what)
{
    if (g_run_us == 0) {
        return;
    }
    int64_t now = esp_timer_get_time();
    double dur = (double)(now - g_run_us) / 1e6;
    uint32_t bytes = gbac_usb_bytes() - g_run_bytes;   /* 32bit 差なので折返し-safe */
    ESP_LOGI(TAG, "%s 経過 %.1fs 受信 %lluKB 平均 %.2fMB/s", what, dur,
             (unsigned long long)(bytes / 1024),
             dur > 0 ? (double)bytes / dur / 1e6 : 0.0);
    g_run_us = 0;
}

static void sample_1s(void)
{
    if (g_run_us == 0) {
        return;
    }
    int64_t now = esp_timer_get_time();
    double dt = (double)(now - g_smp_us) / 1e6;
    if (dt < 0.5) {
        return;
    }
    uint32_t b = gbac_usb_bytes(), p = gbac_usb_packets();
    uint32_t ring = gbac_usb_ring_dropped();
    uint32_t fok = frame_pipe_completed(), fdrop = frame_pipe_dropped();
    g_rate_mbs = (double)(b - g_smp_bytes) / dt / 1e6;
    ESP_LOGI(TAG, "1s rx=%.2fMB/s t=%.1fs pkt+=%llu ring+=%llu frame+=%llu drop+=%llu",
             g_rate_mbs, (double)(now - g_run_us) / 1e6,
             (unsigned long long)(p - g_smp_pkts),
             (unsigned long long)(ring - g_smp_ring),
             (unsigned long long)(fok - g_smp_fok),
             (unsigned long long)(fdrop - g_smp_fdrop));
    /* 音声入力: in=audio_pipe まで届いたパケット数、pairs=入力ペア数
     * (32,802 ペア/秒が目標)、seqlost=seq の飛び幅の合計(=失われたパケット数)、
     * rxadrop=USB rx リングの溢れで捨てた音声パケット数 */
    uint32_t apk = audio_pipe_pushed_pkts(), apa = audio_pipe_pushed_pairs();
    uint32_t asl = audio_pipe_seq_lost(), aad = 0;
    gbac_usb_put_audio(NULL, &aad);
    ESP_LOGI(TAG, "1s aud in=%llu(%.0f/s) pairs=%llu(%.0f/s) seqlost=%llu rxadrop+=%llu",
             (unsigned long long)(apk - g_smp_apkt), (double)(apk - g_smp_apkt) / dt,
             (unsigned long long)(apa - g_smp_apair), (double)(apa - g_smp_apair) / dt,
             (unsigned long long)(asl - g_smp_aseqlost),
             (unsigned long long)(aad - g_smp_aadrop));
    g_smp_us = now;
    g_smp_bytes = b;
    g_smp_pkts = p;
    g_smp_ring = ring;
    g_smp_fok = fok;
    g_smp_fdrop = fdrop;
    g_smp_apkt = apk;
    g_smp_apair = apa;
    g_smp_aseqlost = asl;
    g_smp_aadrop = aad;
}

/* 起動時にチップとヒープを出す。rev1_3 プロファイルが実機(v1.3)と
 * 合っているか、PSRAM が生きているかをここで確認する。 */
static void show_board(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    /* revision は MXX の 16bit(メジャー*100+マイナー)。v1.3 なら 103。
     * %02d にすると v1.03 と出るので %d にする(ROM ログは v1.3 と出す) */
    ESP_LOGI(TAG, "chip rev v%d.%d cores=%d features=0x%x", (int)(ci.revision / 100),
             (int)(ci.revision % 100), (int)ci.cores, (unsigned)ci.features);
    ESP_LOGI(TAG, "heap free internal=%uKB psram=%uKB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

/* EP ごとの受信数。nano は映像を EP4/EP1、音声を EP3 に分けて送るので、
 * どれか 1 本が黙っているとフレームが組立たない。切り分け用に常時出す。 */
static const char *usb_ep_stats(void)
{
    static char buf[128];
    int n = gbac_usb_ep_count();
    int off = 0;
    for (int i = 0; i < n && off < (int)sizeof(buf) - 20; i++) {
        uint32_t ok = 0, drop = 0;
        gbac_usb_put_stats(i, &ok, &drop);
        off += snprintf(buf + off, sizeof(buf) - (size_t)off, "%02x=%llu/%llu/%llu ",
                        gbac_usb_ep_addr(i),
                        (unsigned long long)gbac_usb_ep_packets(i),
                        (unsigned long long)ok, (unsigned long long)drop);
    }
    if (off >= (int)sizeof(buf)) {
        off = (int)sizeof(buf) - 1;
    }
    buf[off] = '\0';
    return buf;
}

static void show_status(void)
{
    int64_t vus = 0, aus = 0;
    uint32_t vn = 0, an = 0;
    gbac_usb_rx_prof(&vus, &aus, &vn, &an);
    double t_run = g_run_us ? (double)(esp_timer_get_time() - g_run_us) / 1e6 : 0.0;
    ESP_LOGI(TAG, "sd=%d rec=%d free=%lluMB | usb pkts=%llu bytes=%lluKB err=%llu unk=%llu ring=%llu lost=%llu ep[rx/put/drop %s] | frame ok=%llu drop=%llu ldrop=%llu | aud in=%llu pairs=%llu seqlost=%llu rxadrop=%llu gap=%llu ovf=%llu | enc f=%llu e=%llu scale<=%uus enc<=%uus | mux=%s %.1fs",
             gbac_sd_ready(), g_recording ? 1 : 0,
             (unsigned long long)gbac_rec_sd_free_mb(),
             (unsigned long long)gbac_usb_packets(),
             (unsigned long long)(gbac_usb_bytes() / 1024),
             (unsigned long long)gbac_usb_errors(),
             (unsigned long long)g_pkt_unk,
             (unsigned long long)gbac_usb_ring_dropped(),
             (unsigned long long)gbac_usb_reasm_lost(),
             usb_ep_stats(),
             (unsigned long long)frame_pipe_completed(), (unsigned long long)frame_pipe_dropped(),
             (unsigned long long)frame_pipe_dropped_lines(),
             (unsigned long long)audio_pipe_pushed_pkts(),
             (unsigned long long)audio_pipe_pushed_pairs(),
             (unsigned long long)audio_pipe_seq_lost(),
             (unsigned long long)gbac_usb_audio_dropped(),
             (unsigned long long)audio_pipe_gaps(), (unsigned long long)audio_pipe_overruns(),
             (unsigned long long)gbac_enc_frames(), (unsigned long long)gbac_enc_errors(),
             (unsigned)gbac_enc_scale_us_max(), (unsigned)gbac_enc_enc_us_max(),
             gbac_rec_mux_where(), (double)gbac_rec_mux_age_us() / 1e6);
    if (g_recording) {
        /* mux が esp_muxer の中で止まっても読めるように、毎回の状態として出す */
        char ms[256];
        gbac_rec_mux_stats(ms, (int)sizeof(ms));
        ESP_LOGI(TAG, "mux %s", ms);
    }
    /* 経過秒と直近 1 秒の実測速度、rx タスクの 1 パケット原価 */
    ESP_LOGI(TAG, "t=%.1fs rx1s=%.2fMB/s | rx v=%lldus/%llu(=%lldus/pkt) a=%lldus/%llu(=%lldus/pkt)",
             t_run, g_rate_mbs,
             (long long)vus, (unsigned long long)vn,
             vn ? (long long)(vus / (int64_t)vn) : 0,
             (long long)aus, (unsigned long long)an,
             an ? (long long)(aus / (int64_t)an) : 0);
}

/* どのタスクが CPU を食っているか。core0 が飽和しているかを見るために、
 * 録画・パイプラインテストの終わる直前に 'p' で出す。
 * RunTime は esp_timer のマイクロ秒、Percentage は全タスク合計に対する割合。 */
static void show_task_stats(void)
{
    char *buf = malloc(2048);
    if (buf == NULL) {
        ESP_LOGE(TAG, "統計用のバッファが確保できない");
        return;
    }
    /* 2 コア別々には出ないので、合計の CPU 時間と、コアの空きは i のログで見る */
    vTaskGetRunTimeStats(buf);
    ESP_LOGI(TAG, "タスク統計(起動からの累計)");
    ESP_LOGI(TAG, "%s", buf);
    free(buf);
    ESP_LOGI(TAG, "heap internal=%uKB psram=%uKB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

static void start_recording(void)
{
    if (!gbac_sd_ready() && gbac_sd_mount() != ESP_OK) {
        ESP_LOGE(TAG, "SD がないので録画できない");
        gbac_led_blink(4, 100);
        return;
    }
    frame_pipe_stats_reset();
    audio_pipe_reset();
    if (gbac_rec_start(&g_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "録画を開始できない");
        gbac_led_blink(4, 100);
        return;
    }
    gbac_usb_stream_start();
    run_begin();
    gbac_led_rec(true);
    g_recording = true;
    ESP_LOGI(TAG, "録画開始 scale=%dx %dkbps split=%dmin vol=%d%%",
             (int)g_cfg.scale, (int)g_cfg.bitrate_kbps, (int)g_cfg.split_min, (int)g_cfg.volume_pct);
    /* 受信が始まった直後の internal RAM。AAC・muxer・enc の確保が終わった後なので、
     * DMA のステージ(23,040B x2)が足りているかここで見ると切り分けやすい。 */
    ESP_LOGI(TAG, "録画開始直後 internal free=%uKB 最大=%uKB DMA 可=%uKB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024));
}

static void stop_recording(void)
{
    gbac_usb_stream_stop();
    gbac_rec_stop();
    gbac_led_rec(false);
    g_recording = false;
    run_end("録画");
    show_status();
}

/* SD がなくても映像と音声のパイプラインを確かめるための試験。
 * 録画と同じく USB 受信 → フレーム管 → 4 倍拡大 → HW エンコードまで通すが、
 * mux(ファイル書き込み)は走らせない。SD の調子が悪いときに切り分ける。*/
static bool g_pipe_test = false;

static void start_pipe_test(void)
{
    if (g_recording || g_pipe_test) {
        ESP_LOGW(TAG, "すでに動いている");
        return;
    }
    frame_pipe_stats_reset();
    audio_pipe_reset();
    if (gbac_enc_start(&g_cfg, 0) != ESP_OK) {
        ESP_LOGE(TAG, "エンコーダを開始できない");
        return;
    }
    gbac_usb_stream_start();
    run_begin();
    gbac_led_rec(true);
    g_pipe_test = true;
    ESP_LOGI(TAG, "パイプラインテスト開始(mp4 には書かない)");
}

static void stop_pipe_test(void)
{
    gbac_usb_stream_stop();
    gbac_enc_stop();
    gbac_led_rec(false);
    g_pipe_test = false;
    run_end("パイプラインテスト");
    show_status();
}

/* コンソールの RX(GPIO38)に届いた 1 文字で操作できるようにする。
 * esptool が 460800 で ROM と握手できたので、PC→基板の RX 配線は実証済み。
 * これで RST ボタンやタクト SW に頼まず、PC 側から再起動・ベンチ・録画開始ができる。
 *   r = 再起動 / i = 状態表示 / b = エンコーダベンチ / t = 録画の ON・OFF
 *   s = SD 結線の総当たり / u = USB ポートの状態レジスタ / d = 書き込みモードで再起動 */
static void uart_cmd_init(void)
{
    esp_err_t e = uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "UART0 の受信設定に失敗(%s)", esp_err_to_name(e));
    }
}

/* 書き込みモード(ダウンロード起動)で再起動する。
 * P4 は LP(常時給電)ドメインの LP_SYSTEM SYS_CTRL bit2 = FORCE_DOWNLOAD_BOOT を
 * 立ててリセットすると、ROM がダウンロード起動を選ぶ。ボタン不要。
 * IDF に API はないが、USB-Serial/JTAG コンソールが他チップで同じことをしている
 * (components/esp_system/port/usb_console.c:259 で RTC_CNTL_FORCE_DOWNLOAD_BOOT)。
 * この基板は CH343 に DTR/RTS が繋がっていないので esptool の自動リセットが効かず、
 * これが入るまで BOOT+RST を毎回押す必要があった。 */
static void enter_download_mode(void)
{
    uint32_t v = REG_READ(LP_SYSTEM_REG_SYS_CTRL_REG) | LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT;
    REG_WRITE(LP_SYSTEM_REG_SYS_CTRL_REG, v);
    /* ログが UART に流れきる前にリセットすると読めないので待つ */
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

static void uart_cmd_handle(char c)
{
    if (c == 'r') {
        ESP_LOGI(TAG, "UART の指示で再起動する");
        esp_restart();
    } else if (c == 'i') {
        show_status();
        ESP_LOGI(TAG, "heap internal=%uKB psram=%uKB largest_int=%uKB",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    } else if (c == 'b') {
        if (g_recording) {
            ESP_LOGW(TAG, "録画中なのでベンチは使えない");
        } else {
            int n = CONFIG_GBAC_BENCH_FRAMES > 0 ? CONFIG_GBAC_BENCH_FRAMES : 600;
            gbac_enc_bench(&g_cfg, n);
        }
    } else if (c == 't') {
        if (g_pipe_test) {
            ESP_LOGW(TAG, "パイプラインテスト中なので録画は使えない");
        } else if (g_recording) {
            stop_recording();
        } else {
            start_recording();
        }
    } else if (c == 'T') {
        if (g_pipe_test || g_recording) {
            ESP_LOGW(TAG, "すでに動いている");
        } else {
            start_recording();
            if (g_recording) {
                g_auto_stop_at = esp_timer_get_time() + 20 * 1000 * 1000;
                ESP_LOGI(TAG, "20 秒たったら自動停止する(試験)");
            }
        }
    } else if (c == 'e') {
        if (g_pipe_test) {
            stop_pipe_test();
        } else {
            start_pipe_test();
        }
    } else if (c == 'p') {
        show_task_stats();
    } else if (c == 's') {
        if (gbac_sd_ready()) {
            ESP_LOGI(TAG, "SD はすでにマウント済み");
        } else {
            gbac_sd_probe();
        }
    } else if (c == 'w') {
        if (g_recording) {
            ESP_LOGW(TAG, "録画中なので速度テストは使えない");
        } else {
            gbac_sd_speedtest(8);
        }
    } else if (c == 'y') {
        gbac_sd_rawtest(4, 1024);
    } else if (c == 'z') {
        gbac_sd_fatscan();
    } else if (c == 'x') {
        gbac_sd_buf_battery();
    } else if (c == 'v') {
        gbac_sd_wrprobe();
    } else if (c == 'k') {
        if (g_recording || g_pipe_test) {
            ESP_LOGW(TAG, "録画中なので PPA プローブは使えない");
        } else {
            gbac_ppa_probe();
        }
    } else if (c == 'F') {
        gbac_sd_format();
    } else if (c == 'd') {
        ESP_LOGI(TAG, "書き込みモードで再起動する(esptool がつながる)");
        enter_download_mode();
    } else if (c == 'm') {
        /* PC に SD を見せる(MSC)⇄ nano を受ける(ホスト)の切り替え */
        if (gbac_msc_active()) {
            esp_err_t e = gbac_msc_stop();
            ESP_LOGI(TAG, "MSC 解除(%s)", esp_err_to_name(e));
            if (e != ESP_OK) {
                /* OTG PHY が残ったままだとホストをもう一度立てられない。
                 * ここで無理にやると usb_lib タスクだけ動いてログが洪水になる。 */
                ESP_LOGE(TAG, "TinyUSB が畳みきれない。'd' で書き込みモードか電源入れ直し");
            } else {
                esp_err_t e2 = gbac_usb_start(on_packet, NULL);
                ESP_LOGI(TAG, "USB ホスト復旧(%s)", esp_err_to_name(e2));
            }
        } else {
            if (g_recording) {
                stop_recording();
            }
            if (g_pipe_test) {
                stop_pipe_test();
            }
            esp_err_t e = gbac_msc_start();
            if (e == ESP_OK) {
                ESP_LOGI(TAG, "MSC モード。PC の Explorer に SD が出るはず");
            } else {
                ESP_LOGE(TAG, "MSC に切り替えない(%s)", esp_err_to_name(e));
            }
        }
    }
}

static void uart_cmd_poll(void)
{
    uint8_t c;
    while (uart_read_bytes(UART_NUM_0, &c, 1, 0) == 1) {
        if (c == '\r' || c == '\n' || c == ' ') {
            continue;
        }
        ESP_LOGI(TAG, "UART コマンド '%c' を受けた", (char)c);
        uart_cmd_handle((char)c);
    }
}

/* internal RAM をどこがどれだけ食ったかの差分表(2026-10-18)。
 * 起動後の free 309KB が録画開始時に 45KB まで減っていた。
 * 大口を PSRAM へ移す判断材料にする。 */
static uint32_t g_heap_mark;
static void heap_mark(const char *what)
{
    uint32_t now = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t diff = (g_heap_mark >= now) ? (g_heap_mark - now) : 0u;
    ESP_LOGI(TAG, "internal heap %-18s free=%uKB 差分=-%uKB 最大=%uKB", what,
             (unsigned)(now / 1024), (unsigned)(diff / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    g_heap_mark = now;
}

void app_main(void)
{
    gbac_board_init();
    show_board();
    /* シリアルが出ない状況でも「起動した」と分かる合図。3回点滅。 */
    gbac_led_blink(3, 150);
    uart_cmd_init();
    ESP_LOGI(TAG, "コンソールで r=再起動 i=状態 p=タスク統計 b=ベンチ t=録画 ON/OFF e=受信とエンコードのテスト s=SD 結線 w=SD 速度 m=PC に SD を見せる/戻す d=書き込みモード");
    gbac_cfg_load(&g_cfg);
    ESP_LOGI(TAG, "設定 scale=%d bitrate=%dkbps split=%dmin vol=%d%%",
             (int)g_cfg.scale, (int)g_cfg.bitrate_kbps, (int)g_cfg.split_min, (int)g_cfg.volume_pct);

#if CONFIG_GBAC_BRINGUP
    /* 基板が起動しているかだけを見る。重い周辺は初期化しないので、
     * PSRAM を切った設定(sdkconfig.bringup.defaults)でも動く。 */
    ESP_LOGI(TAG, "BRINGUP モード。周辺機器は初期化しない");
    while (1) {
        gbac_led_blink(1, 200);
        ESP_LOGI(TAG, "alive internal=%uKB psram=%uKB largest_int=%uKB",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    }
#endif

    heap_mark("起動時");
    /* H.264 HW エンコーダと GDMA の internal RAM はここでたしかめておく。
     * 録画のたびに開くと heap が断片化して reference frame(約 69KB の連続)が
     * 取れなくなる(2026-10-18 r99/r100 で -3)。 */
    if (gbac_enc_early_init(&g_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "エンコーダが起動時に開けない。録画のときにリトライする");
    }
    heap_mark("enc+GDMA 後");
    frame_pipe_init();
    heap_mark("frame_pipe 後");
    audio_pipe_init(g_cfg.volume_pct);
    heap_mark("audio 後");
    gbac_button_init();
    gbac_usb_start(on_packet, NULL);
    heap_mark("usb 後");
    if (gbac_sd_mount() != ESP_OK) {
        /* 結線は確定済み(slot0/LDO VO4/40MHz)なので総当たりはしない。
         * ここで見えないときはカードの接触か未挿入。`s` で再確認できる。 */
        ESP_LOGE(TAG, "SD が見えない。カードを挿し直すか、's' で結線を確かめてね");
    }
    heap_mark("sd_mount 後");

#if CONFIG_GBAC_BENCH_FRAMES > 0
    /* 手順 2: HW エンコーダが 960x640@60 に耐えるかの実測。
     * SD がなくてもエンコーダ単体なら測れるので、両方用意しておく。 */
    if (gbac_sd_ready()) {
        gbac_rec_bench(&g_cfg, CONFIG_GBAC_BENCH_FRAMES);
    } else {
        ESP_LOGW(TAG, "SD がないのでエンコーダ単体のベンチにする");
        gbac_enc_bench(&g_cfg, CONFIG_GBAC_BENCH_FRAMES);
    }
    show_status();
#endif

    int64_t last_log = esp_timer_get_time();
    while (1) {
        if (gbac_button_wait(pdMS_TO_TICKS(1000))) {
            if (g_recording) {
                stop_recording();
            } else {
                start_recording();
            }
        }
        if (g_auto_stop_at != 0 && esp_timer_get_time() >= g_auto_stop_at) {
            g_auto_stop_at = 0;
            ESP_LOGI(TAG, "試験: 指定秒に達したので自動停止する");
            if (g_recording) {
                stop_recording();
            }
        }
        if (g_recording && !gbac_usb_ready()) {
            ESP_LOGE(TAG, "nano が外れた。録画を止める");
            stop_recording();
        }
        if (g_recording && gbac_rec_low_space()) {
            ESP_LOGE(TAG, "SD の空きがない。録画を止める");
            stop_recording();
            gbac_led_blink(4, 100);
        }
        uart_cmd_poll();
        sample_1s();
        if (esp_timer_get_time() - last_log > 10 * 1000 * 1000) {
            last_log = esp_timer_get_time();
            show_status();
        }
    }
}
