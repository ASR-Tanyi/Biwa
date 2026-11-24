# 10 — Bringing up a game

The core never changes for one game. What a game needs beyond the core goes into its profile, and missing services or NVN functions are added to the core for every game.

## Game profile

`games/<title id>.json`, for example:

```json
{
  "title_id": "0100000000000000",
  "versions": ["1.2.0"],
  "takeovers": [{ "symbol": "nvnBootstrapLoader", "with": "nvn_bootstrap_loader" }],
  "nvn_api": "from-sdk",
  "core_model": false,
  "report_system_version": "from-cnmt",
  "shader_discovery": null,
  "notes": ""
}
```

| Field | Meaning |
|---|---|
| `versions` | Game versions the profile was checked with |
| `takeovers` | Guest functions replaced by host code ([05](05-runtime-os.md)) |
| `nvn_api` | NVN API version to report, or read it from the game's SDK ([06](06-gpu-nvn-vulkan.md)) |
| `core_model` | Horizon's one-thread-per-core scheduling, for games that depend on it ([05](05-runtime-os.md)) |
| `report_system_version` | System version to report, or the content meta's required version |
| `shader_discovery` | Optional plugin that finds shader containers before the first run ([07](07-shaders.md)) |

## What to learn about a game first

Answered by `biwa extract`, `biwa info` and the first traced runs:

- Modules: which `subsdk*` exist, their sizes and build IDs; whether the game loads NROs through `nn::ro`, and how many.
- Whether it uses C++ exceptions, `setjmp` / `longjmp`, or fibers ([04](04-cpu-recompiler.md)).
- NVN API version, texture formats (ASTC or not), how many shaders it compiles.
- Movies: codec, and whether they go through the hardware decoder or a library inside the game ([08](08-audio-input-media.md)).
- Audio: renderer revision and effects; Opus through `hwopus` or the game's own decoder.
- DLC: how add-on content is listed and mounted.
- The required system version, and which services it opens.

## Milestones

The same ladder for every game. Each step ends with something visible on a Windows machine with the game.

| # | Milestone | Done when |
|---|---|---|
| G0 | Inventory | `biwa extract` on base + update + DLC succeeds; `biwa info` lists the modules, sizes, build IDs, NPDM values |
| G1 | Lifted | Every ExeFS module lifted and compiled into DLLs; functions found, coverage and build time reported |
| G2 | SDK starts | `rtld` relocates, the SDK initialises, `nnMain` runs; the trace shows the SVC sequence until the first stop |
| G3 | Services | RomFS mounted, settings, time, applet, account and HID answered; the game reaches NVN initialisation |
| G4 | First frame | NVN device, queue and window up; a cleared frame presented in Biwa's window |
| G5 | Title screen | Textures, buffers, shaders, draws; the title screen renders; sound starts |
| G6 | Menus | Navigation with a controller and the keyboard; save data created; music and sound effects |
| G7 | Gameplay | NROs, if any, load (interpreted, then lifted after a rebuild); a play session of a few minutes without a stop |
| G8 | Everything | Movies, DLC, every offline mode checked once |
| G9 | Fast | Full frame rate on mid-range hardware; all pipelines compiled before play; a 30-minute session without a stop |

## Open questions

- Whether per-game notes (what was learned at each milestone) live in the repository beside the profile, or only with whoever brings the game up.
- Save data: start fresh, or import from the console with a `biwa save import` command.
