/* nanoCH32V305 を USB-HS ホストとして扱う。
 * 物理的には MX1.25 4pin ヘッダ P1( VCC_5V / USBD_N / USBD_P / GND )
 * → ESP32-P4 の HighSpeed 専用 PHY(GPIO49/50。内部 PHY なので GPIO 指定は不要)。
 *
 * デバイス(1209:C001)はベンダークラス 1 インタフェース、
 * EP4 IN / EP1 IN / EP3 IN(bulk 512B)+ EP5 OUT(bulk 512B)。
 * 各 IN EP に 512B の転送を 8 本積み続けて 484B パケットを受け取る。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

/* 484B パケット 1 個を受け取るたびに呼ばれる。
 * EP4/EP1/EP3 のどれから来たかは分からない(先頭の magic で判別する)。
 * USB 側のタスクではなく、core1 で動く専用の rx タスクの文脈で呼ばれる
 * (rx_cb はリングへコピーするだけにしている)。 */
typedef void (*gbac_pkt_cb)(const uint8_t *pkt, int len, void *arg);

esp_err_t gbac_usb_start(gbac_pkt_cb cb, void *arg);

/* nano が接続されて受信用の準備ができているか(1/0) */
int gbac_usb_ready(void);

/* ストリーミング開始('S')/停止('X') */
esp_err_t gbac_usb_stream_start(void);
esp_err_t gbac_usb_stream_stop(void);

/* ping('P')。fw_ver が NULL でなければバージョンを入れる。
 * 応答は EP4 IN に 8 バイトで返る(先頭が 'G','B','A','C')。 */
esp_err_t gbac_usb_ping(int *fw_ver);

uint32_t gbac_usb_packets(void);   /* 完了した転送の数 */
uint32_t gbac_usb_bytes(void);     /* 転送で受け取ったバイト数の合計 */
/* EP ごとの内訳。受信 EP の並び順で idx を渡す(addr は 0x84 のような値) */
int gbac_usb_ep_count(void);
uint32_t gbac_usb_ep_packets(int idx);
int gbac_usb_ep_addr(int idx);
uint32_t gbac_usb_errors(void);    /* 転送エラー数 */
uint32_t gbac_usb_ring_dropped(void);  /* リング満杯で捨てたパケット */
uint32_t gbac_usb_reasm_lost(void);    /* 再組み立てが壊れて捨てたバイト */
/* rx リングへ入れた/捨てたパケット数。ep は受信 EP の並び順の添字。
 * 音声(0xA6)は種別で別カウント(r9 で音声 6 秒分の欠落がここで起きている疑い) */
void gbac_usb_put_stats(int ep, uint32_t *ok, uint32_t *drop);
void gbac_usb_put_audio(uint32_t *ok, uint32_t *drop);
uint32_t gbac_usb_audio_dropped(void);  /* rx リング溢れで捨てた音声パケット数 */
/* rx タスクがパケット処理に使った µs 累計と個数(NULL で省略可)。
 * 割ることで 1 パケットの原価が出る */
void gbac_usb_rx_prof(int64_t *video_us, int64_t *audio_us,
                      uint32_t *video_n, uint32_t *audio_n);
