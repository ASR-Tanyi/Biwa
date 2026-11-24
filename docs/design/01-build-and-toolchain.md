# 01 — Build and toolchain

## Toolchain

| What | Choice |
|---|---|
| Compiler | Clang + lld from **llvm-mingw**, one pinned release (currently 20261117), target `x86_64-w64-mingw32` with UCRT |
| Build | CMake + Ninja |
| Languages | C++23 for Biwa itself; C17 with Clang extensions for generated code |
| CPU baseline | `-march=x86-64-v3` (AVX2, FMA, BMI2); `biwa.exe` checks the CPU at start-up and says so if it falls short |
| Debug info | CodeView: `-gcodeview` and `-Wl,--pdb=`, so Visual Studio, WinDbg, PIX, Superluminal and RenderDoc can read the symbols |
| Runtime libraries | libc++ and libunwind linked statically into `biwa.exe`; module DLLs are plain C against UCRT |

Why Clang on the MinGW target (measured 2026-11-24, both under Wine on the same benchmark):

- `[[clang::musttail]]` guarantees the tail branches between recompiled functions. MSVC has no equivalent, and GCC only has it from version 15.
- `thread_local` is native: a helper's access to the per-thread CPU context took 1.4 ns with Clang and 14–15 ns with MinGW GCC 13, whose TLS is emulated (`__emutls_get_address`).
- Clang has `__int128`, vector extensions, and inline assembly on x64, and it handles very large generated functions well.
- Plain recompiled-style arithmetic ran at the same speed with Clang and GCC (5.7 vs 5.8 ns per iteration), so the choice costs nothing there.
- The same pinned toolchain runs on Windows and on Linux, so a build in either place uses the same compiler.

MSYS2 can be the shell on Windows (git, cmake, ninja from pacman), but the compiler is always the pinned llvm-mingw release: MSYS2's own packages update continuously and cannot be pinned.

## Presets

| Preset | Host | Builds | Runs tests with |
|---|---|---|---|
| `win-debug`, `win-release` | Windows | Everything, Windows | Native |
| `cross-debug`, `cross-release` | Linux | Everything, Windows (llvm-mingw on Linux) | Wine |
| `linux-debug` | Linux | Everything except the Win32 platform layer | Native |
| `linux-asan` | Linux | Same, AddressSanitizer + UBSan | Native |
| `linux-tsan` | Linux | Same, ThreadSanitizer | Native |
| `lint` | Both | clang-format check, clang-tidy with warnings as errors | — |

- The Linux builds exist for sanitizers and fast native tests. Code that is Windows-only (window, input, audio output, the Windows memory and thread primitives) sits behind the small `platform` and `host` interfaces, which have a Linux implementation only for tests.
- Formatting: `.clang-format` (Google style, IndentWidth 4, ColumnLimit 120) and `.clang-tidy` (bug-finding checks). Both tools pinned at one LLVM version.

## Repository layout

```
CMakeLists.txt  CMakePresets.json
cmake/
  BiwaOptions.cmake           warnings, sanitizers, clang-tidy hook
  BiwaModule.cmake            biwa_add_module(): one static library per module, layer-checked
  BiwaFormat.cmake            biwa_format, biwa_format_check
  Dependencies.cmake          every third-party project, pinned by SHA-256
  toolchains/llvm-mingw.cmake Windows builds, native and cross
.github/workflows/ci.yml      CI
docs/design/                  this design
games/<title id>.json         per-game profiles
src/
  core/        logging, errors, hashing, files, JSON, the host interface (memory, threads, futex-like waits)
  nx/          Switch formats: PFS0/HFS0, NCA, tickets, keys, RomFS, BKTR, NSO, NRO, NPDM, NACP, CNMT
  a64/         instruction decoder, semantics (shared), interpreter
  lift/        module analysis, function discovery, C emitter, coverage files
  rt/          runtime: CPU support, dispatch, address space, loader, kernel, IPC, services
  gpu/         NVN API objects, Vulkan backend, textures (formats, swizzle, ASTC)
  shader/      glue between the GPU and the ported shader recompiler
  audio/       audio renderer, Opus decoding, output
  media/       movie decoding
  platform/    win32/ (window, XInput, Raw Input, WASAPI) and null/ (tests)
  tools/biwa/  the biwa command (extract, info, lift, build, shaders, run)
third_party/
  yuzu/        shader_recompiler + the common/ headers it needs, GPL-2.0-or-later, see UPSTREAM.md
tests/          unit tests, homebrew test programs (AArch64 assembly), GPU image tests
```

Each module keeps its public headers in `src/<module>/include/biwa/<module>/` (included as `"biwa/<module>/…"`) and its sources in `src/<module>/src/`. Every source file starts with an SPDX licence line.

## Module graph

Every module is a static library `biwa::<module>` made with `biwa_add_module(<name> LAYER <n> …)`. A module may depend only on modules of a **lower** layer; CMake refuses anything else at configure time, so the graph cannot grow cycles or shortcuts.

| Layer | Modules | May use |
|---|---|---|
| 0 | `core` | Third-party libraries only |
| 1 | `nx`, `a64`, `platform` | `core` |
| 2 | `lift`, `shader`, `audio`, `media` | Layers 0–1 (`lift`: `a64`, `nx`; `shader`: the ported recompiler) |
| 3 | `gpu` | Layers 0–2 (`shader`, `platform`) |
| 4 | `rt` | Layers 0–3 |
| 5 | `cli` (the `biwa` command) | Everything |

Generated module DLLs use none of these: they include only the C header that defines the runtime–module interface (`biwa_module_init`, `BiwaRuntimeApi`), which lives in `rt` and is plain C17.

## How a game is built

`biwa build <work folder>` drives everything after extraction:

1. `biwa lift` writes C for each module into `<work>/gen/<module>/`, split into chunks of a bounded size (for example about 2,000 functions or 4 MB of C each) so they compile in parallel.
2. It writes `<work>/build/build.ninja` and runs Ninja with the pinned Clang (`-O2 -march=x86-64-v3 -fno-strict-aliasing`, plus the debug-info flags).
3. Each chunk's object is keyed by a hash of its C and the compiler flags, so a re-lift that changes ten functions recompiles only their chunks.
4. Each module links into `<work>/build/<module>-<build id>.dll` with its PDB. The DLL exports a single entry, `biwa_module_init`, which hands the runtime its table of guest addresses and host functions.

`biwa.exe` is built from this repository (and later shipped as a prebuilt release), so a player needs the toolchain only to build the game's module DLLs.

## CI

- Commits stay small, one step each.
- GitHub Actions (`.github/workflows/ci.yml`): `linux-debug`, `linux-asan`, `linux-tsan`, `cross-debug` and `cross-release` with tests under Wine, and `lint`. A native Windows job (llvm-mingw on a Windows runner) can be added when the runtime first runs there.
- Each preset has a workflow, so CI and a local check run the same command: `cmake --workflow --preset <name>`.

## Open questions

- Whether x86-64-v3 as the baseline leaves out CPUs people still play on; if so, a second build at v2 (SSE4.2).
- Whether to ship `biwa.exe` as a release binary from CI once a first game is playable.
