# 09 — Testing and debugging

## Where things run

| Where | What |
|---|---|
| Linux x64, native | Every unit and integration test, with AddressSanitizer + UBSan and ThreadSanitizer |
| Linux x64, Windows build under Wine | The same tests cross-built, plus the Win32-specific code that Wine can run |
| Linux x64, Mesa lavapipe | GPU image tests and shader tests (no physical GPU) |
| Windows with a GPU | Real games, real GPU, performance, controllers, audio |

Tests never read game files. Real games run only on their owner's machine.

## Test layers

### Formats ([03](03-extract.md))

Builders in the tests create PFS0, NCA, tickets, IVFC trees, BKTR patches, compressed sections, NSO and NPDM from made-up keys and data. Round trips, damage (bit flips → exit code 3), missing keys (exit code 4).

### CPU ([04](04-cpu-recompiler.md))

- **Decoder:** all 2^32 encodings against Capstone; an explicit list of what Biwa does not support.
- **Instruction semantics:** for each instruction form, random register, flag, FP-control and memory states run on Unicorn and on Biwa's semantics; every result must match: registers, flags, memory, FPSR. Unicorn runs at EL0 with the settings Horizon gives applications (`CTR_EL0` and `DC ZVA` allowed, counters readable at 19.2 MHz), so privileged instructions fault in both. Loads, stores and branches use a block of random memory that Unicorn maps at the block's host address, with the base register and pc pointing into it; the whole block must match afterwards. Exclusives also run as load-exclusive / store-exclusive sequences (with `CLREX` or another address in between, or neither), and four threads incrementing one counter through the interpreter check that store-exclusives are atomic. FP states carry edge values (NaNs with payloads, infinities, denormals, the ends of the normal range), and a table of hand-picked FP operands (tininess on either side of the smallest normal, invalid operations, signed zeros) runs under eight FPCR settings. Edge cases (NaNs, denormals, overflow, shifts by register width, unaligned pairs) are listed by hand on top of the random ones.
- **Lifted C vs interpreter:** random straight-line and branching sequences assembled in the test, lifted, compiled, run, and compared with the interpreter.

### Homebrew test programs

Small AArch64 programs in `tests/guest/`, assembled and linked with the same LLVM (`--target=aarch64-none-elf`), then packed into Biwa's module format by a test tool. They run through the real pipeline (lift → compile → load → run) on the real runtime with the `null` platform. Each one checks one behaviour and exits with a code:

- Calls, tail calls, jump tables, indirect calls into unknown code (interpreter fallback and coverage file).
- C++ exceptions across three frames, `setjmp` / `longjmp`.
- Threads, priorities, mutexes, condition variables, address arbiter, events, timeouts.
- Memory SVCs: heap, `MapMemory` move semantics, permissions, `QueryMemory`.
- IPC: a session to a test service, every descriptor type, domains, handles.

They are written from scratch for Biwa and contain no Nintendo code.

### Runtime and services ([05](05-runtime-os.md))

Unit tests for the HIPC / CMIF parser and builder, each service's commands with hand-built messages, the RomFS reader on built images, save data commits.

### GPU and shaders ([06](06-gpu-nvn-vulkan.md), [07](07-shaders.md))

NVN calls made directly by test code render synthetic scenes on lavapipe, compared with reference images in `tests/gpu/ref/`. Synthetic Maxwell programs go through the ported recompiler, `spirv-val`, and lavapipe.

## Debugging tools

| Tool | What it gives |
|---|---|
| Logs | Per subsystem (`cpu`, `svc`, `ipc`, `fs`, `nvn`, `vk`, `audio`, `hid`), levels, one file per run under `<work>/logs/` |
| Stops | Unimplemented SVC, service command or NVN function: one line naming it, the guest caller, a guest backtrace (frame pointer chain, symbolised from module symbols and the lifter's function table) |
| Crash reports | Host exceptions go through the vectored exception handler: host PC → guest function (lifter's table), guest registers, backtrace, the last SVCs and IPC calls |
| `--trace=svc,ipc,nvn,thread` | Every call of the chosen kinds, with arguments and results |
| `biwa lift --explain <addr>` | How an address was found, its function, blocks and disassembly |
| `--interp=<module or range>` | Run chosen code on the interpreter instead of its compiled version, to bisect lifter bugs |
| `--compare=<range>` | Run a function both ways from the same state and report the first difference |
| RenderDoc | GPU captures (F12 starts a capture through RenderDoc's in-app API when RenderDoc is attached) |
| PDBs | WinDbg, Visual Studio, PIX and Superluminal |

## CI

GitHub Actions on every push to `main`: `linux-debug` + tests, `linux-asan`, `linux-tsan`, `cross-release` + tests under Wine, `lint`. A red build is fixed before the next step.
