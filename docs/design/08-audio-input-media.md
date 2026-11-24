# 08 — Audio, input, media and the platform layer

## Audio

### Audio renderer (`audren:u`)

The game's SDK (`nn::audio`) builds the whole mix description in memory and sends it with `RequestUpdate`; the renderer mixes and reports back. Biwa implements the renderer:

| Part | What Biwa does |
|---|---|
| Revision | Accepts the renderer revision the game's SDK announces and enables the matching behaviour |
| Voices | Wave buffers in PCM16, PCM8, PCM float and DSP-ADPCM; pitch (resampling), volume, biquad filters, priority, drop on overload |
| Mixes | Final mix, sub-mixes, splitters, mix volumes |
| Effects | Delay, reverb, I3DL2 reverb, biquad, aux (the game processes the buffer itself), limiter, compressor, capture |
| Sinks | Device sink (to WASAPI), circular-buffer sink |
| Output to the game | Voice status, played sample counts, consumed wave buffers, performance metrics |

- Mixing runs on its own thread every 5 ms (240 samples at 48 kHz), the console's audio frame.
- Biwa's own code, with yuzu's `audio_core` (GPL-2.0-or-later) and Ryujinx's renderer (MIT) as references. Where an effect must match the console sample for sample (reverb, I3DL2), porting yuzu's code for it is allowed by [02](02-dependencies.md).

### Other audio services

- `audout:u`: plain PCM output streams, for games that use them.
- `hwopus`: hardware Opus decoder (single and multi-stream) on libopus.

### Output

WASAPI shared mode, event-driven, 48 kHz; resampled when the device runs at another rate, down-mixed from 5.1 when the device is stereo.

## Input (`hid`)

- The game reads input from HID's shared memory, which the service writes. Its layout depends on the system version Biwa reports ([05](05-runtime-os.md)); Biwa writes the layout that version uses.
- Npads: Pro Controller (full key) for each connected controller, and handheld. Each npad's ring buffer gets new entries at the console's sampling rate, with sampling numbers and timestamps as the SDK expects (rate checked against the references before implementing).
- Sources:
  - XInput controllers (up to four) → Pro Controllers.
  - Keyboard (Raw Input) → player 1, with a mapping in the config.
  - GameCube controller adapters later.
- Vibration (`SendVibrationValues`) → XInput rumble, approximated. Six-axis reports at rest. Touch from the mouse in handheld mode, later.
- Applets: the controller support applet and software keyboard come when a game needs them.

## Movies

Games often play VP9 video. How a game decodes it decides where Biwa steps in, and a trace answers that for each game:

| If the game uses | Biwa does |
|---|---|
| The hardware decoder (nvdec through `nvdrv`) | Implements the nvdec channel: rebuilds each VP9 frame from the SDK's parsed frame data and decodes it with libvpx (yuzu does the same with FFmpeg) |
| A movie library inside the game | Takes over that library's entry points in the game profile, with a libvpx-based host implementation |

The movies' audio format is found by the same trace.

## Platform layer (Win32)

| Piece | Choice |
|---|---|
| Window | Win32, per-monitor DPI aware, resizable, borderless full screen (F11), message loop on the main host thread |
| Graphics surface | `VK_KHR_win32_surface` |
| Timers | High-resolution waitable timers |
| Input | XInput, Raw Input |
| Audio | WASAPI |
| Crash handling | Vectored exception handler: guest data aborts and host crashes become reports ([09](09-testing-and-debugging.md)) |
| Config | `biwa.json` beside the executable: controllers, key mapping, window, audio device, language, user name |

The `null` platform (no window, no audio device, scripted input) runs the same runtime in tests.

## Open questions

- Movies: the hardware decoder or an in-game library, per game (trace first).
- Which renderer revisions and effects to support first (from the first games' traces).
- GameCube adapter support: through XInput-compatible adapters only, or the adapter's own USB protocol.
