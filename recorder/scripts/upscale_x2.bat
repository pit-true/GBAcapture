@echo off
rem 録画機の 480x320 の mp4 を、ドットをそのまま 2 倍にして 960x640 にする。
rem 使い方: mp4 をこのバッチにドラッグアンドドロップする。複数まとめてもよい。
rem 出力は元と同じフォルダの 元の名前_960x640.mp4
setlocal

where ffmpeg >nul 2>nul
if errorlevel 1 (
  echo ffmpeg が見つからない。winget install Gyan.FFmpeg などで入れてから使う。
  pause
  exit /b 1
)

if "%~1"=="" (
  echo mp4 をこのバッチにドラッグ^&ドロップしてください。
  pause
  exit /b 1
)

for %%F in (%*) do (
  echo 変換中: %%~nxF
  ffmpeg -hide_banner -loglevel error -stats -y -i "%%~fF" -vf "scale=iw*2:ih*2:flags=neighbor" -c:v libx264 -preset slow -crf 10 -pix_fmt yuv420p -c:a copy -movflags +faststart "%%~dpnF_960x640.mp4"
  if errorlevel 1 (
    echo 失敗: %%~nxF
  ) else (
    echo 完了: %%~dpnF_960x640.mp4
  )
)

pause
