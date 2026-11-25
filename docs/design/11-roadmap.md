# 11 — Roadmap

Every step below is one small commit with its own test, small enough to check quickly. Steps marked **(hardware)** need a Windows machine with a GPU and a game; everything else runs on Linux. Game milestones (G0–G9) are in [10](10-game-bring-up.md).

## P0 — Skeleton

1. `CMakeLists.txt`, `CMakePresets.json`, `cmake/Dependencies.cmake`; a `biwa` command that prints its version; GoogleTest wired.
2. `.clang-format`, `.clang-tidy`, the `lint` preset.
3. `cross-*` presets with the pinned llvm-mingw; tests under Wine.
4. GitHub Actions: `linux-debug`, `linux-asan`, `linux-tsan`, `cross-release`, `lint`.
5. `src/core`: logging, errors, files, hashing (SHA-256 via Mbed TLS), JSON.
6. **(hardware)** `win-debug` builds on Windows; start-up checks for CPU features ([01](01-build-and-toolchain.md)) and Vulkan extensions ([06](06-gpu-nvn-vulkan.md)).

## P1 — Extract ([03](03-extract.md))

1. `Storage`: file, sub-range, memory; parallel copy.
2. PFS0 reader.
3. `prod.keys` / `title.keys` parser (values never printed).
4. NCA header (AES-XTS), section headers.
5. AES-CTR sections; ExeFS through HierarchicalSha256.
6. IVFC; RomFS reader; `biwa romfs ls/cat`.
7. Tickets and title keys.
8. BKTR: indirect storage and AES-CTR-Ex.
9. Compressed sections (LZ4).
10. NSO (decompression, MOD0), NPDM, CNMT, NACP.
11. Work folder writer and `manifest.json`.
12. `biwa info`, `biwa extract`, exit codes.
13. **(hardware)** G0 on a first game.

## P2 — A64 core ([04](04-cpu-recompiler.md))

1. Decoder table from `a64.inc`; the 2^32 encoding test against Capstone.
2. Unicorn test harness (random states, comparison).
3. Semantics, one group per step: integer arithmetic and logic (with CRC32) → loads and stores → branches and system → exclusives and barriers → FP scalar → AdvSIMD (several steps) → crypto.
4. Interpreter over the semantics.

## P3 — Lifter ([04](04-cpu-recompiler.md))

1. Module loader for analysis (image, symbols, relocations, `.eh_frame`).
2. Entry discovery and recursive descent; `--explain`.
3. Jump tables.
4. C emitter, phase 1.
5. `biwa build`: chunks, `build.ninja`, module DLL / shared object, `biwa_module_init`.
6. Homebrew test program toolchain (`tests/guest/`), first program runs end to end.
7. Dispatch with per-thread cache; interpreter fallback; coverage file.
8. Non-local resume: C++ exceptions and `longjmp` test programs.
9. Same tests cross-built and run under Wine.
10. **(hardware)** G1.

## P4 — Kernel ([05](05-runtime-os.md))

1. Address space window and region layout; `QueryMemory`, `GetInfo`.
2. Loader for runtime: map modules, load DLLs, main thread, boot through `rtld`.
3. Threads: create, start, exit, sleep, priorities, core masks.
4. Handles and events; `WaitSynchronization`, cancel.
5. Mutexes and condition variables (guest-memory words).
6. Address arbiter.
7. Memory SVCs: heap, `MapMemory` move, permissions, attributes, physical memory.
8. Stops and crash reports with guest backtraces.
9. **(hardware)** G2.

## P5 — IPC and services ([05](05-runtime-os.md), [08](08-audio-input-media.md))

1. HIPC / CMIF / TIPC parser and builder; domains; the service framework.
2. `sm:`.
3. `fsp-srv`: RomFS; then save data; then DLC storages.
4. `set`, `set:sys`, `time:u`, `acc:u0`, `apm`, `psm`, `lm`.
5. `appletOE` and its interfaces.
6. `hid` shared memory; XInput and keyboard.
7. `vi`, `nvdrv` minimum, `aoc:u`, offline and accept-only services.
8. **(hardware)** G3.

## P6 — GPU and shaders ([06](06-gpu-nvn-vulkan.md), [07](07-shaders.md))

1. Win32 window, Vulkan instance and device (discrete GPU), swapchain; lavapipe on Linux.
2. NVN guest thunks and generated adapters; `nvnBootstrapLoader` takeover.
3. Device, queue, memory pools (imported memory, with the fallback), buffers, sync.
4. Window and present → **(hardware)** G4.
5. Import yuzu's shader recompiler unchanged into `third_party/yuzu/`; make it build; `UPSTREAM.md`.
6. Environment, Profile, RuntimeInfo; shader records; synthetic shader tests.
7. Textures (swizzle, formats, pools, samplers), state, draws, pipeline cache.
8. **(hardware)** G5.

## P7 — Sound, movies, the rest

1. Audio renderer core (voices, mixes, device sink), WASAPI → sound in G5.
2. Effects; `audout:u`; `hwopus`.
3. **(hardware)** G6, G7.
4. Movie path chosen by trace, then implemented; DLC → **(hardware)** G8.

## P8 — Performance

1. Profile on real hardware (Superluminal / PIX).
2. Lifter phase 2: registers in locals, flag liveness, direct PLT calls.
3. Asynchronous pipeline compilation; GPU thread if needed.
4. **(hardware)** G9.

## P9 — More games

For each: write its profile, then implement the services and NVN functions it stops on ([10](10-game-bring-up.md)).
