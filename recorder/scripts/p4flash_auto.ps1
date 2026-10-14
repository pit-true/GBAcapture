# ESP32-P4-Pico へボタンなしで書いて起動ログまで取る。
#   コンソールへ 'd'(ダウンロードモードで再起動) -> ROM 応答 -> esptool -> 起動ログ
# ボタン(BOOT+RST)が必要なのはアプリが起動しなくなったときだけ。
# docs/esp32p4-flashing.md 参照。
param(
    [string]$Port = 'COM6',
    [string]$BuildDir = 'build',
    [int]$LogSecs = 60,
    [string]$LogFile = 'D:\esp\scripts\applog.log',
    [string]$ProgressFile = 'D:\esp\scripts\p4flash_progress.log',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Continue'
$py = 'D:\esp\tools\python_env\idf5.5_py3.12_env\Scripts\python.exe'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$rec = Join-Path $root 'recorder'
$build = Join-Path $rec $BuildDir

function Log($m) {
    $line = "{0} {1}" -f (Get-Date -Format 'HH:mm:ss'), $m
    Write-Host $line
    Add-Content -Path $ProgressFile -Value $line -Encoding utf8
}

function PipeToLog($exe, $arguments) {
    # 進捗をファイルへ積みながら画面にも出す(esptool の % 表示は積みません)
    & $exe @arguments 2>&1 | ForEach-Object {
        $s = "$_"
        if ($s -notmatch '\.\.\. \(\d+ %\)|Writing at') {
            Add-Content -Path $ProgressFile -Value $s -Encoding utf8
        }
    }
    return $LASTEXITCODE
}

. D:\esp\scripts\idf_env.ps1 *> $null

if (-not $SkipBuild) {
    Log "[1/4] ビルド(ninja)"
    Push-Location $build
    $b = PipeToLog 'ninja' @()
    Pop-Location
    if ($b -ne 0) { Log "ビルド失敗 exit=$b"; exit 1 }
} else {
    Log "[1/4] ビルドはスキップ"
}

Log "[2/4] 'd' を送って書き込みモードへ"
& $py (Join-Path $PSScriptRoot 'p4send.py') $Port d 1.5 *> $null

# ROM が応答するまで待つ。応答がなければ 'd' を送り直す。
# 最後の一ラウンドだけ BOOT+RST を待つ(アプリが死んでいるときの保険)。
$ready = $false
for ($round = 1; $round -le 3; $round++) {
    $wait = if ($round -lt 3) { 8 } else { 600 }
    $deadline = (Get-Date).AddSeconds($wait)
    while ((Get-Date) -lt $deadline) {
        & $py -m esptool --port $Port --chip esp32p4 --before no_reset `
            --after no_reset --connect-attempts 1 read_mac *> $null
        if ($LASTEXITCODE -eq 0) { $ready = $true; break }
        Start-Sleep -Seconds 2
    }
    if ($ready) { break }
    if ($round -lt 3) {
        Log "ROM が応答しない。'd' を送り直す"
        & $py (Join-Path $PSScriptRoot 'p4send.py') $Port d 0.5 *> $null
    } else {
        Log "それでも ROM が見つからない。BOOT+RST を押してね(アプリが死んだときだけ)"
    }
}
if (-not $ready) { Log "ROM が見つからずタイムアウト。"; exit 1 }
Log "ROM が応答した。書き込みに入る"

Log "[3/4] 書き込み"
Push-Location $build
$fa = (Get-Content (Join-Path $build 'flash_args') -Raw) -split '\s+' | Where-Object { $_ -ne '' }
$flash_args = @('-m', 'esptool', '--port', $Port, '--chip', 'esp32p4',
                '--before', 'no_reset', '--after', 'hard_reset', '-b', '460800',
                'write_flash') + $fa
$e = PipeToLog $py $flash_args
Pop-Location
if ($e -ne 0) { Log "書き込み失敗 exit=$e"; exit 1 }
Log "FLASH_DONE"

Log "[4/4] 起動させて $LogSecs 秒ログ収集"
& $py (Join-Path $PSScriptRoot 'p4watch.py') $LogSecs $LogFile 2>&1 |
    ForEach-Object { Add-Content -Path $ProgressFile -Value $_ -Encoding utf8 }
Log "ログ: $LogFile"
