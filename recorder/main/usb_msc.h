/* PC に microSD を見せる(USB MSC デバイス)。
 *
 * ESP32-P4 の USB-OTG(HighSpeed 側・UTMI)はホスト(nano 受信)と
 * デバイス(PC から SD リーダとして見る)で一個取り。切り替えるときは
 * ホストを完全に畳んで PHY を返してから TinyUSB をデバイスとして起こす。
 *
 * SD も一個取り。PC に見せている間はこちらの FATFS の登録を外す
 * (カードは初期化済みのまま esp_tinyusb の MSC ストレージへ渡す)。
 * 二重アクセスになるので録画はできない。
 *
 * 使い方(app_main のコンソール 'm'):
 *   録画中なら止める → gbac_msc_start() → PC の Explorer に SD が出る
 *   → 外すときは gbac_msc_stop() → FATFS を /sd へ戻す
 *   → 呼び出し側が gbac_usb_start() でホストを復旧する
 */
#pragma once

#include "esp_err.h"

/* 1 = PC に見せている最中(MSC モード) */
int gbac_msc_active(void);

/* ホストを畳み、SD の FATFS 登録を外し、TinyUSB を MSC デバイスとして起動する。
 * 失敗したときはホスト復旧を試みてからエラーを返す。 */
esp_err_t gbac_msc_start(void);

/* MSC を畳んで SD の FATFS を /sd へ戻す。
 * ホストの復旧(gbac_usb_start)は呼び出し側の仕事。 */
esp_err_t gbac_msc_stop(void);
