# 02 — Dependencies

## Policy

- Biwa uses well-established projects only, and keeps their number small.
- Every dependency is pinned (release tag or commit plus SHA-256) in `cmake/Dependencies.cmake`, or in `third_party/<name>/UPSTREAM.md` for ported code.
- Licences must be compatible with Biwa's GPL-3.0.
- Reading a project to learn how something behaves is not use. Code is copied only from the projects listed under "Ported code".

## In shipped binaries

| Project | Used for | Licence |
|---|---|---|
| Mbed TLS 3.6 LTS | AES-XTS / CTR / ECB, SHA-256 for NCA decryption and hash checks | Apache-2.0 or GPL-2.0-or-later |
| LZ4 | NSO segments, compressed NCA sections | BSD-2-Clause |
| nlohmann/json | Work-folder manifests, game profiles, caches' indexes | MIT |
| Vulkan-Headers | Vulkan API | Apache-2.0 / MIT |
| volk | Loading Vulkan entry points from the driver | MIT |
| Vulkan Memory Allocator | Vulkan memory management | MIT |
| SPIRV-Headers | SPIR-V definitions (needed by sirit) | MIT-style (Khronos) |
| sirit | SPIR-V emitter used by yuzu's shader recompiler | BSD-3-Clause |
| fmt | Formatting inside the ported shader recompiler, and logging | MIT |
| Boost (Container, Intrusive) | Used by the ported shader recompiler; may be replaced with std / own containers during the port | BSL-1.0 |
| libopus | `hwopus` (Opus decoding service) | BSD-3-Clause |
| libvpx | VP9 movies, for games that decode VP9 through a path Biwa implements ([08](08-audio-input-media.md)) | BSD-3-Clause |

Windows itself provides the window, XInput, Raw Input, WASAPI and the Vulkan loader (installed with the GPU driver). No SDL: Biwa targets Windows only, so the platform layer is a thin Win32 layer with no third-party code.

## Ported code

| Source | What | Licence | Pin |
|---|---|---|---|
| yuzu (`yuzu-emu-mirror/yuzu-mainline`, the last mainline state) | `src/shader_recompiler/` (241 files, 2.3 MB) and the `src/common/` headers it includes (`common_types`, `bit_field`, `bit_cast`, `div_ceil`, `alignment`, `common_funcs`, `polyfill_ranges`, `assert`; `settings.h` is replaced by a small Biwa config) | GPL-2.0-or-later (240 files) and WTFPL (one generator script) | `7ffac53c9e67fa18d067114a8eb3e77c74bba68f`, 2024-03-03 |
| yuzu, same commit | `src/video_core/textures/decoders.cpp` (block-linear swizzle) and `src/video_core/host_shaders/astc_decoder.comp` (ASTC on the GPU) | GPL-2.0-or-later | same |
| dynarmic (`yuzu-mirror/dynarmic`) | The A64 instruction pattern table (`a64.inc`, 1,029 lines) as the starting point for Biwa's decoder | 0BSD | `9d4582339990d4eae53f1dc7160686920fc2075c` |

Ported files keep their copyright and SPDX lines. `third_party/yuzu/UPSTREAM.md` lists the source commit, every file taken, and every change Biwa makes. yuzu's code is used under GPL-2.0-or-later, which combines with Biwa's GPL-3.0.

## Tests and development only (never in shipped binaries)

| Project | Used for | Licence |
|---|---|---|
| GoogleTest | Unit tests | BSD-3-Clause |
| Unicorn | Reference CPU for lifter and interpreter tests | GPL-2.0 |
| Capstone | Disassembly in test failures, logs and `biwa lift --explain` | BSD-3-Clause |
| SPIRV-Tools | `spirv-val` on every SPIR-V the recompiler emits in tests | Apache-2.0 |
| glslang | Build tool: compiles Biwa's own GLSL (blits, ASTC decoder) to SPIR-V; also writes test shaders | BSD-3-Clause and others (Khronos) |
| Wine | Running the cross-built Windows tests on Linux | LGPL-2.1-or-later |
| Mesa lavapipe | Headless Vulkan for GPU image tests | MIT |
| RenderDoc | GPU captures on Windows | MIT |

Unicorn's GPL-2.0 does not reach Biwa's binaries because it is linked only into test programs, which are not distributed.

## References (read, not used)

- switchbrew wiki: formats, SVCs, IPC, services.
- Atmosphère: Horizon kernel behaviour (memory states, synchronisation, scheduler).
- libnx: service command IDs and structures.
- hactool: NCA and BKTR details.
- yuzu and Ryujinx: service behaviour, HID shared memory, audio renderer, NVN and Maxwell behaviour.
- Arm Architecture Reference Manual for A-profile: instruction semantics.
- envytools: Maxwell instruction encodings.
