/* GBA キャプチャ録画機(ESP32-P4-Pico)の本体。
 *   nanoCH32V305(USB-HS) → 受信 → 4x 拡大 → HW H.264 + AAC → microSD の mp4
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
#include "sdkconfig.h"
#include "sd_recorder.h"
#include "usb_host_nano.h"
#include "video_enc.h"

static const char *TAG = "main";

static gbac_cfg_t g_cfg;
static bool g_recording;

static void on_packet(const uint8_t *pkt, int len, void *arg)
{
    (void)arg;
    if (len > 0 && pkt[0] == GBAC_LINE_MAGIC) {
        frame_pipe_push(pkt, len);
    } else if (len > 0 && pkt[0] == GBAC_AUDIO_MAGIC) {
        audio_pipe_push(pkt, len);
    }
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

static void show_status(void)
{
    ESP_LOGI(TAG, "sd=%d rec=%d free=%lluMB | usb pkts=%llu err=%llu | frame ok=%llu drop=%llu | aud gap=%llu ovf=%llu | enc f=%llu e=%llu scale<=%uus enc<=%uus",
             gbac_sd_ready(), g_recording ? 1 : 0,
             (unsigned long long)gbac_rec_sd_free_mb(),
             (unsigned long long)gbac_usb_packets(), (unsigned long long)gbac_usb_errors(),
             (unsigned long long)frame_pipe_completed(), (unsigned long long)frame_pipe_dropped(),
             (unsigned long long)audio_pipe_gaps(), (unsigned long long)audio_pipe_overruns(),
             (unsigned long long)gbac_enc_frames(), (unsigned long long)gbac_enc_errors(),
             (unsigned)gbac_enc_scale_us_max(), (unsigned)gbac_enc_enc_us_max());
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
    gbac_led_rec(true);
    g_recording = true;
    ESP_LOGI(TAG, "録画開始 scale=%dx %dkbps split=%dmin vol=%d%%",
             (int)g_cfg.scale, (int)g_cfg.bitrate_kbps, (int)g_cfg.split_min, (int)g_cfg.volume_pct);
}

static void stop_recording(void)
{
    gbac_usb_stream_stop();
    gbac_rec_stop();
    gbac_led_rec(false);
    g_recording = false;
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
    } else if (c == 'e') {
        if (g_pipe_test) {
            stop_pipe_test();
        } else {
            start_pipe_test();
        }
    } else if (c == 's') {
        if (gbac_sd_ready()) {
            ESP_LOGI(TAG, "SD はすでにマウント済み");
        } else {
            gbac_sd_probe();
        }
    } else if (c == 'u') {
        gbac_usb_dump_port();
    } else if (c == 'd') {
        ESP_LOGI(TAG, "書き込みモードで再起動する(esptool がつながる)");
        enter_download_mode();
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

void app_main(void)
{
    gbac_board_init();
    show_board();
    /* シリアルが出ない状況でも「起動した」と分かる合図。3回点滅。 */
    gbac_led_blink(3, 150);
    uart_cmd_init();
    ESP_LOGI(TAG, "コンソールで r=再起動 i=状態 b=ベンチ t=録画 ON/OFF e=受信とエンコードのテスト s=SD 総当たり u=USB ポート d=書き込みモード");
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

    frame_pipe_init();
    audio_pipe_init(g_cfg.volume_pct);
    gbac_button_init();
    gbac_usb_start(on_packet, NULL);
    if (gbac_sd_mount() != ESP_OK) {
        /* 結線(スロット番号 / IO 電源の LDO / 電源ゲート)が回路図で読み切れない。
         * 総当たりで生やすので、カードを挿していればここで復旧する。 */
        ESP_LOGW(TAG, "SD が見えない。結線を総当たりする");
        if (gbac_sd_probe() != ESP_OK) {
            ESP_LOGE(TAG, "SD 未挿入。挿してからボタンを押してね");
        }
    }

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
        if (esp_timer_get_time() - last_log > 10 * 1000 * 1000) {
            last_log = esp_timer_get_time();
            show_status();
        }
    }
}
