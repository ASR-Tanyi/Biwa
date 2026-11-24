# Biwa

Biwa turns Nintendo Switch games into native **Windows x64** programs. It recompiles a game's AArch64 code ahead of time to C, compiles that C to x64, and runs it on Biwa's own implementation of the Switch's operating system, with graphics on Vulkan.

The core is game-agnostic: what one game needs beyond it lives in a small per-game profile.

Status: design. Nothing builds yet.

## What it is made of

| Part | What it does | Design |
|---|---|---|
| `biwa extract` | NSP + update + DLC + your keys → a work folder (ExeFS modules, RomFS image, metadata) | [extract](docs/design/03-extract.md) |
| `biwa lift` / `biwa build` | AArch64 modules → C → x64 module DLLs | [cpu](docs/design/04-cpu-recompiler.md) |
| `biwa.exe` (runtime) | Loader, kernel (SVCs), threads, memory, IPC services | [os](docs/design/05-runtime-os.md) |
| GPU | NVN API on Vulkan | [gpu](docs/design/06-gpu-nvn-vulkan.md) |
| `biwa shaders` | Maxwell shaders → SPIR-V (yuzu's shader recompiler, ported), pipeline caches | [shaders](docs/design/07-shaders.md) |
| Audio, input, movies, window | audren / hwopus, hid, VP9, Win32 | [media](docs/design/08-audio-input-media.md) |

Start with the [overview](docs/design/00-overview.md). The full list is in [docs/design](docs/design/README.md).

## No game data here

This repository contains no game code, assets, keys or firmware, and it never will. Generated C, shaders and caches made from a game stay on the machine of the person who owns that game. Biwa builds a program from your own dump, on your own PC.

## License

GPL-3.0 (see [LICENSE](LICENSE)). Code ported from yuzu keeps its GPL-2.0-or-later notices. Third-party code is listed in [dependencies](docs/design/02-dependencies.md).
