# Biwa design

Read them in order. Each document ends with its open questions; a decision taken moves into the text.

| # | Document | Covers |
|---|---|---|
| 00 | [Overview](00-overview.md) | Goals, non-goals, the pipeline, the process at run time, principles, data policy, glossary |
| 01 | [Build and toolchain](01-build-and-toolchain.md) | CMake + Ninja, Clang (llvm-mingw), presets, the repo layout, how a game is built |
| 02 | [Dependencies](02-dependencies.md) | Third-party projects and licences, ported code, references |
| 03 | [Extract](03-extract.md) | NSP / NCA / tickets / BKTR / DLC, the work folder, `biwa extract` |
| 04 | [CPU recompiler](04-cpu-recompiler.md) | A64 decoder and semantics, function discovery, C generation, dispatch, interpreter fallback, exceptions |
| 05 | [Runtime OS](05-runtime-os.md) | Address space, loader, threads, synchronisation, SVCs, IPC, services |
| 06 | [GPU: NVN on Vulkan](06-gpu-nvn-vulkan.md) | NVN objects, command recording and replay, memory pools, textures, presentation |
| 07 | [Shaders](07-shaders.md) | Finding shaders, yuzu's recompiler ported, environments, pipeline cache |
| 08 | [Audio, input, media](08-audio-input-media.md) | audren, hwopus, hid, movies, the Win32 platform layer |
| 09 | [Testing and debugging](09-testing-and-debugging.md) | Test layers, homebrew test programs, Wine, lavapipe, tracing, crash reports |
| 10 | [Bringing up a game](10-game-bring-up.md) | Game profiles, what to learn about a game first, milestones |
| 11 | [Roadmap](11-roadmap.md) | Phases, each split into small verifiable steps |
