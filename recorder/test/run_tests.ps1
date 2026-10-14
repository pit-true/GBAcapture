# recorder/test/run_tests.ps1
# ESP-IDF 非依存の純ロジック(components/gbacap)をホスト側 gcc でビルドしてテストする。
# 使い方: powershell -NoProfile -File recorder\test\run_tests.ps1
$ErrorActionPreference = "Stop"

$gcc = "D:\tools\strawberry\c\bin\gcc.exe"
if (-not (Test-Path $gcc)) {
    Write-Error "gcc が見つからない: $gcc"
    exit 2
}

$here = $PSScriptRoot
$comp = Join-Path $here "..\components\gbacap"
$build = Join-Path $here "build"
New-Item -ItemType Directory -Force -Path $build | Out-Null

$srcs = @(Get-ChildItem $comp -Filter *.c | ForEach-Object { $_.FullName })
if ($srcs.Count -eq 0) { Write-Warning "components/gbacap に .c が無い(Red のはず)" }

$exe = Join-Path $build "test_gbacap.exe"
$gcc_args = @(
    "-std=c11", "-Wall", "-Wextra", "-Werror", "-fno-strict-aliasing",
    "-I", (Join-Path $comp "include"),
    (Join-Path $here "test_gbacap.c")
) + $srcs + @("-o", $exe)

& $gcc @gcc_args
if ($LASTEXITCODE -ne 0) {
    Write-Host "ビルド失敗(exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

& $exe
exit $LASTEXITCODE
