# ESP32-P4-Pico に書いて起動ログまで取る。docs/esp32p4-flashing.md 参照。
# ダウンロードモード(BOOT+RST)を待つ -> 書く -> RUN で起こす -> ログ収集。
# 進捗は Add-Content で逐次 flush するので、ログを後から tail できる。
param(
    [string]$Port = 'COM6',
    [string]$BuildDir = 'build',
    [int]$WaitSecs = 120,
    [int]$LogSecs = 60,
    [string]$LogFile = 'D:\esp\scripts\applog.log',
    [string]$ProgressFile = 'D:\esp\scripts\p4flash_progress.log'
)
$ErrorActionPreference = 'Continue'
$py = 'D:\esp\tools\python_env\idf5.5_py3.12_env\Scripts\python.exe'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location (Join-Path $root 'recorder')

function Log($m) {
    $line = "{0} {1}" -f (Get-Date -Format 'HH:mm:ss'), $m
    Write-Host $line
    Add-Content -Path $ProgressFile -Value $line -Encoding utf8
}

Log "[1/3] ROM が応答するまで待ちます。まだなら BOOT+RST を押してね。"
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
}
if (-not $ready) { Log "ROM が見つからずタイムアウト。"; exit 1 }
Log "ROM が応答した。書き込みに入る"

. D:\esp\scripts\idf_env.ps1 *> $null
Log "[2/3] 書き込み"
idf.py -B $BuildDir -p $Port flash 2>&1 | ForEach-Object { Add-Content -Path $ProgressFile -Value $_ -Encoding utf8 }
if ($LASTEXITCODE -ne 0) { Log "書き込み失敗 exit=$LASTEXITCODE"; exit 1 }
Log "FLASH_DONE"

Log "[3/3] 起動させて $LogSecs 秒ログ収集"
& $py (Join-Path $PSScriptRoot 'p4boot.py') $LogSecs $LogFile 2>&1 | ForEach-Object { Add-Content -Path $ProgressFile -Value $_ -Encoding utf8 }
Log "ログ: $LogFile"
