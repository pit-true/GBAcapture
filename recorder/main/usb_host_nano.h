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
 * usb_host_nano.c のクライアントタスクの文脈で呼ばれる。長く止めること。 */
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

uint32_t gbac_usb_packets(void);   /* 受信したパケット総数 */
uint32_t gbac_usb_errors(void);    /* 転送エラー数 */

/* HS コントローラのポート状態レジスタを出す。
 * CS(port connect status)=0 なら D+ のプルアップを物理的に見ていない
 * = ケーブル / PHY / 給電側。CS=1 なのに列挙しなければプロトコル側。 */
void gbac_usb_dump_port(void);
