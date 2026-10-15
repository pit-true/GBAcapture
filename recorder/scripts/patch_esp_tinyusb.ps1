# esp_tinyusb 1.7.x の uninstall を ESP32-P4 の HighSpeed(rhport1)で動くように直す。
#
# 元コード:
#   ESP_RETURN_ON_FALSE(tusb_teardown(), ESP_ERR_NOT_FINISHED, ...)
# tusb_teardown() はマクロで tusb_deinit(0)。P4 で CONFIG_TINYUSB_RHPORT_HS を選ぶと
# デバイスは rhport1 で動くので rhport0 は未初期化 = 常に false を返す。
# その結果 usb_del_phy() が飛ばず OTG PHY が残り、USB ホストをもう一度
# usb_host_install() すると "selected PHY is in use" で失敗する。
#
# 使い方(managed_components を取り直したあとに必ず実行):
#   pwsh -File recorder\scripts\patch_esp_tinyusb.ps1
# 二重に当てても大丈夫(マーカーを見て判断する)。

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot          # recorder/
$file = Join-Path $root "managed_components\espressif__esp_tinyusb\tinyusb.c"

if (-not (Test-Path $file)) {
    Write-Error "$file が見つからない。先に idf.py reconfigure で依存を取ってきてください"
}

$txt = Get-Content $file -Raw -Encoding utf8

if ($txt.Contains("GBAcapture local patch")) {
    Write-Host "すでに当たっている: $file"
    exit 0
}

$old = "    ESP_RETURN_ON_FALSE(tusb_teardown(), ESP_ERR_NOT_FINISHED, TAG, ""Unable to teardown TinyUSB"");"
if (-not $txt.Contains($old)) {
    Write-Error "想定した行が見つからない。esp_tinyusb のバージョンが変わった可能性: $file"
}

$new = @"
    /* GBAcapture local patch: tusb_teardown() is tusb_deinit(0), but on ESP32-P4
     * with CONFIG_TINYUSB_RHPORT_HS the device lives on rhport 1, so the call
     * always returned false (ESP_ERR_NOT_FINISHED) and the PHY was never deleted.
     * Deinit the port the stack was actually initialized on. */
    ESP_RETURN_ON_FALSE(tusb_deinit(TUD_OPT_RHPORT), ESP_ERR_NOT_FINISHED, TAG, "Unable to teardown TinyUSB");
"@

$txt = $txt.Replace($old, $new.TrimEnd())
Set-Content -Path $file -Value $txt -Encoding utf8 -NoNewline
Write-Host "patch 適用: $file"
