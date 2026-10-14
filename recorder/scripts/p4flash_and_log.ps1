# ESP32-P4-Pico に書いて起動ログまで取る。docs/esp32p4-flashing.md 参照。
# 使い: BOOT+RST でダウンロードモードに入った基板に対して
#   powershell -File recorder\scripts\p4flash_and_log.ps1
param(
    [string]$Port = 'COM6',
    [string]$BuildDir = 'build',
    [int]$WaitSecs = 120,
    [int]$LogSecs = 60,
    [string]$LogFile = 'D:\esp\scripts\applog.log'
)
$ErrorActionPreference = 'Continue'
$py = 'D:\esp\tools\python_env\idf5.5_py3.12_env\Scripts\python.exe'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location (Join-Path $root 'recorder')

Write-Host "[1/3] ROM が応答するまで待ちます。まだなら BOOT+RST を押してね。"
$deadline = (Get-Date).AddSeconds($WaitSecs)
$ready = $false
while ((Get-Date) -lt $deadline) {
    & $py -m esptool --port $Port --chip esp32p4 --before no_reset --after no_reset `
        --connect-attempts 1 read_mac *> $null
    if ($LASTEXITCODE -eq 0) { $ready = $true; break }
    & $py -m esptool --port $Port --chip esp32p4 --baud 460800 --before no_reset_no_sync `
        --after no_reset --connect-attempts 1 read_mac *> $null
    if ($LASTEXITCODE -eq 0) { $ready = $true; break }
    Start-Sleep -Seconds 2
    Write-Host "  まだ応答なし..."
}
if (-not $ready) { Write-Host "ROM が見つからずタイムアウト。"; exit 1 }

. D:\esp\scripts\idf_env.ps1 *> $null
Write-Host "[2/3] 書き込み"
idf.py -B $BuildDir -p $Port flash
if ($LASTEXITCODE -ne 0) { Write-Host "書き込み失敗 exit=$LASTEXITCODE"; exit 1 }

Write-Host "[3/3] 起動させて $LogSecs 秒ログ収集"
& $py (Join-Path $PSScriptRoot 'p4boot.py') $LogSecs $LogFile
Write-Host "ログ: $LogFile"
