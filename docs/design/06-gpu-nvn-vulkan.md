# 06 — GPU: NVN on Vulkan

Games draw through NVN, the Switch's native graphics API. Biwa replaces NVN at its API: every NVN function the game calls is a host function that records the work and replays it on Vulkan 1.3. No GPU command stream is emulated.

## Entry: `nvnBootstrapLoader`

- The game gets every NVN function from `nvnBootstrapLoader("nvnDeviceGetProcAddress")` and then `nvnDeviceGetProcAddress(device, name)`. Biwa takes over `nvnBootstrapLoader` (found by symbol in the game's modules; listed in the game profile).
- The game calls the pointers it receives from recompiled code, so each NVN function gets a **guest thunk address**: a 4-byte slot in a runtime-owned guest page that dispatch maps to a host adapter.
- An adapter reads the arguments from the CPU state (`x0`–`x7`, `v0`–`v7`, stack) according to the function's C signature, calls the typed host implementation, and writes the result back. Adapters are generated at compile time from one table of signatures, so a wrong signature fails to build.
- A name with no implementation still gets a slot; calling it stops with `[nvn] <name> not implemented`, the caller and a backtrace.
- `nvnDeviceGetInteger` reports the API version the game's SDK expects (read from the game's `sdk` module version; listed in the profile).

NVN's types and object sizes must match the game's, because the game allocates NVN objects itself. Where the declarations come from is an open question: a published reconstruction of `nvn.h`, or Biwa's own declarations written from what games call.

## Objects

| NVN | Biwa | Vulkan |
|---|---|---|
| Device | Device, capabilities, handles | `VkDevice` |
| Queue | Submission, fences | `VkQueue`, timeline semaphore |
| MemoryPool | Range of guest memory + flags | Imported `VkDeviceMemory`, or a mirrored allocation |
| Buffer | Pool range | `VkBuffer` over the pool's memory |
| Texture, TextureView | Description + pool range (block-linear) | `VkImage` + `VkImageView`, kept by the texture cache |
| Sampler | Parameters | `VkSampler` (custom border colours) |
| TexturePool, SamplerPool | Descriptor tables in guest memory | Read when binding |
| Program | Shader stages (code address + control data) | Translated per pipeline ([07](07-shaders.md)) |
| State objects (blend, depth-stencil, polygon, multisample, channel mask, colour, vertex attribute / stream) | Plain structs the game owns, written by Biwa's setters | Dynamic state, or part of the pipeline key |
| CommandBuffer | Host command list | Translated into `VkCommandBuffer` at submit |
| Sync | Fence | Timeline semaphore value |
| Window | Game's textures + presentation | Swapchain, blit to present |

## Memory pools and GPU addresses

- A pool's storage is guest memory the game allocated. With `VK_EXT_external_memory_host`, Biwa imports that memory directly, so the CPU and GPU see the same bytes. Otherwise it keeps a GPU copy and uploads pages the guest wrote (tracked with Windows write-watch, [05](05-runtime-os.md)) before each submit.
- A pool's GPU address is its guest address. GPU addresses are then unique and an interval map turns any GPU address into (pool, offset), which buffers, vertex streams, constant buffers and global memory accesses in shaders need.

## Textures

- NVN textures use the Tegra block-linear layout; Vulkan images are opaque. A texture cache keeps a `VkImage` per (pool range, format, size, layout) and:
  - uploads, de-swizzling block-linear to linear, when the guest wrote the range (`nvnTextureWriteTexels`, CPU writes, `CopyBufferToTexture`);
  - downloads only when the guest reads data the GPU wrote.
- Formats: a table from NVN formats to `VkFormat`. BC1–7 are native on desktop GPUs. Most desktop GPUs do not support ASTC, so ASTC textures are decoded on the GPU by a compute shader when the device lacks it.
- De-swizzling (yuzu's `textures/decoders.cpp`) and the ASTC compute decoder (`host_shaders/astc_decoder.comp`) are ported from yuzu at the same source commit as the shader recompiler; both are GPL-2.0-or-later. Biwa's own GLSL (blits, ASTC) is compiled to SPIR-V at build time with glslang.

## Commands

- `nvnCommandBuffer*` calls append to a host command list. Biwa does not write the game's command memory, but it counts consumption the way NVN does and calls the game's out-of-memory callback at the same moments, so the game's bookkeeping stays right.
- `nvnCommandBufferEndRecording` returns a handle naming the recorded list; the game may submit it many times.
- `nvnQueueSubmitCommands` translates the lists into Vulkan command buffers:
  - Draws (arrays, elements, instanced, indirect), compute dispatches, copies, clears, barriers, queries and timestamps, transform feedback, conditional rendering.
  - Render targets: dynamic rendering, so no render pass objects.
  - State: as much as possible as dynamic state (`VK_EXT_extended_dynamic_state` 1–3, `VK_EXT_vertex_input_dynamic_state`), so the pipeline key holds only shaders and what must be static.
  - Textures and samplers for each draw: the handles the shaders read from constant buffers are looked up in the texture and sampler pools, and written with push descriptors.
- NVN conventions: window origin lower-left (a flipped viewport), depth range −1..1 (`VK_EXT_depth_clip_control`).
- Bound textures: NVN's driver places bound texture handles in a driver-owned constant buffer that compiled shaders read. Biwa keeps that buffer per stage with the same layout. Its exact slot and layout are confirmed from the first dumped shaders.

## Synchronisation and presentation

- `nvnQueueFenceSync` signals a timeline semaphore value; `nvnSyncWait` waits on it on the CPU; `nvnQueueWaitSync` makes the next submit wait on it.
- `nvnQueuePresentTexture` blits the game's texture into a swapchain image (FIFO, 60 Hz), letterboxed to 16:9. Borderless full screen on F11.
- The discrete GPU is chosen explicitly, and `biwa.exe` exports `NvOptimusEnablement = 1`.

## Vulkan requirements

Vulkan 1.3 plus: `VK_KHR_push_descriptor`, `VK_EXT_extended_dynamic_state2` / `3` (as far as available), `VK_EXT_vertex_input_dynamic_state`, `VK_EXT_depth_clip_control`, `VK_EXT_custom_border_color`, `VK_EXT_transform_feedback`, `VK_EXT_conditional_rendering`, `VK_EXT_provoking_vertex`, `VK_EXT_index_type_uint8`, and `VK_EXT_external_memory_host` (optional). `biwa.exe` checks them at start-up and names any that are missing.

## Threads

The first version translates on the thread that submits. A dedicated GPU thread comes when profiles show submission costs frame time.

## Tests

Headless image tests on Mesa lavapipe: test code calls Biwa's NVN implementation directly (no guest), draws small synthetic scenes, and compares the result with reference images kept in the repository. Every new NVN function gets one.

## Open questions

- Source of NVN declarations (a published reconstruction, or Biwa's own).
- The bound-texture constant buffer's slot and layout.
- ASTC decoding is only needed by games that use ASTC; it can wait for the first one.
- Resolution scaling: a non-goal for the first playable version.
