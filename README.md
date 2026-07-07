# GBAcapture (Nisekoro-capture)

A DIY video/audio capture system for the original Game Boy Advance (AGB-001),
built around a **nanoCH32V305** (WCH CH32V305, RISC-V) board — in the spirit of the
Japanese "偽トロキャプチャ" (Nisetro) capture mods. Named after its author's handle "koro".

**2026-07-05: first light!**

![first light](docs/captures/2026-07-05_first-light.png)

## Features

- **Video**: 240×160, 15-bit RGB, **60 fps** real-time streaming over USB High-Speed
- **Audio**: stereo, tapped at the CPU's SO1/SO2 outputs (**pre-volume** — works with the
  wheel at zero), PWM-carrier-synchronous integration + analog RC front-end for clean sound
- **Viewer** (Windows, C#/.NET 8): pixel-perfect integer scaling (1x–4x), aspect-correct
  fullscreen, screenshots (F12), volume control, NO SIGNAL overlay
- **Recording**: designed to be captured with OBS (window capture + desktop audio)
- No custom kernel drivers — WinUSB via Zadig, vendor bulk protocol

## How it works

```
GBA LCD bus (taps on labeled vias) ──> nanoCH32V305 ──USB-HS bulk──> Windows viewer
  DCK 4.19MHz burst clock              timer-triggered DMA            decode + render
  SPL line sync                        circular capture               WASAPI audio
  15-bit RGB parallel                  3-endpoint streaming
  SO1/SO2 audio (PWM) ──RC filter──>   262kHz ADC + carrier-sync integration
```

Key techniques (hard-won; see `docs/` for the full story):

- One line = **241 DCK pulses** (240 visible + 1 junk in H-blank)
- Jitter-free capture: circular DMA armed once, frame anchor derived from the
  **VBLANK pause in the data itself** — no per-line interrupts in the data path
- USB-HS bulk tops out at ~3.3 MB/s per endpoint (NAK → next-microframe retry),
  so video is striped across two endpoints; audio gets its own (ordering!)
- GBA audio PWM carrier is always a multiple of 32.768 kHz (SOUNDBIAS) — sampling at
  N×32.768 kHz and box-averaging N kills the carrier exactly

## Repository layout

| Path | Contents |
|---|---|
| `firmware/app/` | Capture firmware (C, [ch32fun](https://github.com/cnlohr/ch32fun)) |
| `firmware/iap/` | Self-update bootloader — flash new app firmware over USB, no buttons |
| `firmware/gba-pantest/` | Tiny GBA ROM to verify L/R channel mapping |
| `host/GbaCap/` | Core library (packet parsing, pixel decode) + xUnit tests |
| `host/GbaCap.Viewer/` | The viewer app (WinForms) |
| `host/GbaCap.Cli/` | Developer CLI (flash, diagnostics, still capture) |
| `docs/` | Documentation (Japanese): wiring, signal analysis, design notes |

## Getting started (development)

Prereqs: .NET 8 SDK, xPack RISC-V GCC, GNU Make, wchisp — see `docs/setup.md`.

```bash
# Host tests
cd host && dotnet test

# Build firmware and update the device over USB (no buttons needed)
cd firmware && ./build.sh app
cd ../host && dotnet run --project GbaCap.Cli -- update ../firmware/app/gbacap_app.bin

# Run the viewer
host/GbaCap.Viewer/bin/Debug/net8.0-windows/GbaCap.Viewer.exe
```

Hardware wiring is documented in `docs/wiring.md` (GBA-side taps on the
silkscreen-labeled vias of an AGB-CPU-02/03 board) and `docs/gba-40pin-pinout.md`.

## Documentation index (Japanese)

- [docs/wiring.md](docs/wiring.md) — 配線表（正）と実機診断の記録
- [docs/gba-40pin-pinout.md](docs/gba-40pin-pinout.md) — 40ピンLCDコネクタの検証済みピン配置
- [docs/gba-video-signal.md](docs/gba-video-signal.md) — 映像信号仕様（実測値）
- [docs/audio-plan.md](docs/audio-plan.md) — 音声取得（SO1/SO2、RCフィルタ、キャリア同期積分）
- [docs/hands-free-loop.md](docs/hands-free-loop.md) — ボタンフリー開発ループ（IAP方式）
- [docs/flashing.md](docs/flashing.md) — 書き込み方式の検討経緯・救済手順
- [docs/architecture.md](docs/architecture.md) — 全体設計
- [docs/hardware-adapter-plan.md](docs/hardware-adapter-plan.md) — 基板化プラン
- [docs/setup.md](docs/setup.md) — 開発環境の再現手順
- [docs/publishing-policy.md](docs/publishing-policy.md) — 公開時のコンテンツ方針

## Status / Roadmap

- [x] 60 fps video capture + viewer
- [x] Clean stereo audio (pre-volume tap)
- [x] Button-free development loop (IAP self-update)
- [ ] UVC/UAC composite device (driverless, works with any capture software)
- [ ] Custom interposer PCB (32-pin & 40-pin FFC support)
- [ ] Standalone recorder

## License

TBD (private repository for now — see `docs/publishing-policy.md` for what must be
scrubbed before any public release).
