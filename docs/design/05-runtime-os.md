# 05 — Runtime OS

`biwa.exe` plays the part of Horizon for one game process: it maps the modules, owns the guest address space and threads, answers SVCs, and serves IPC.

## Level: SVCs and IPC, with NVN at its API

| Option | For Biwa |
|---|---|
| Re-implement SDK functions (`nn::fs::…`, `nn::os::…`) | Rejected as the main level: tens of thousands of SDK functions, and their symbols and versions differ per game |
| **Kernel SVCs + IPC services** | **Chosen.** About 50 SVCs and a documented, stable IPC surface shared by every game. The game's own SDK code runs recompiled, IPC marshalling included |
| Emulate the GPU (nvdrv + Maxwell command streams) | Rejected: a full GPU emulator. NVN is replaced at its API instead ([06](06-gpu-nvn-vulkan.md)) |

Takeovers (a guest function replaced by host code) are the exception: `nvnBootstrapLoader`, then only what a profile proves worth it (for example `memcpy`), each listed in the game profile.

## Guest address space

Guest addresses are host addresses, so generated code accesses memory with no translation ([04](04-cpu-recompiler.md)).

- At start-up, before any other DLL is loaded, `biwa.exe` reserves one window of **64 GiB at `0x10_0000_0000`**. It lies inside a 39-bit address space, as a Switch application's does, and away from where Windows places images and heaps. If the reservation fails, Biwa stops and says so.
- Inside it, pages are committed, decommitted and protected at 4 KiB granularity (`VirtualAlloc` / `VirtualProtect` within the one reservation), matching Horizon's page size.
- `svcGetInfo` reports the regions below, so the SDK lays itself out inside the window.

| Region | Offset in the window | Size | Holds |
|---|---|---|---|
| Code | 0 | 4 GiB | `rtld`, `main`, `subsdk*`, `sdk`, in Horizon's load order, each 2 MiB aligned |
| NRO | 4 GiB | 4 GiB | NROs, each at a fixed slot from its build ID (`nro/slots.json`) |
| Heap | 8 GiB | 8 GiB | `svcSetHeapSize` |
| Alias | 16 GiB | 16 GiB | `svcMapMemory` destinations, `svcMapPhysicalMemory` |
| Stack | 32 GiB | 2 GiB | Thread stacks mapped by the SDK |
| Kernel-owned | 34 GiB | rest | TLS pages, shared memory (HID), transfer memory views, the runtime's own guest-visible data |

Mapping without page aliasing:

- `svcMapMemory` makes the source inaccessible while the destination is mapped, so Biwa **moves** the bytes (commit destination, copy, decommit source) and moves them back on unmap. No shared views are needed, which also avoids Windows' 64 KiB view granularity.
- Transfer and shared memory are read and written by host services directly at their guest addresses.
- Optional, for the GPU: the window can be reserved with `MEM_WRITE_WATCH`, so `GetWriteWatch` reports pages the guest wrote since the last check ([06](06-gpu-nvn-vulkan.md)).

## Loader and boot

1. Read `manifest.json`, `npdm.json` and the game profile.
2. Map each module from `modules/<name>.bin` at its address: `.text` and `.rodata` read-only, `.data` and `.bss` read-write. Guest code is never executed from these pages; they exist for data reads, the interpreter and the unwinder.
3. Load each module's DLL and register its functions with dispatch. A module without a DLL runs on the interpreter, with a warning.
4. Create the main thread with the NPDM's priority, stack size and ideal core; `x0` = 0, `x1` = the main thread's handle; entry = `rtld`'s entry. `rtld` (recompiled) performs the relocations and starts the SDK and `main`.

## Threads

- Each guest thread is a host thread with a large reserved host stack (256 MiB reserved, committed on use), because recompiled calls nest on the host stack as well as the guest one.
- Horizon priorities 0–63 map onto Windows thread priorities in bands. Core masks and ideal cores are stored and reported; Windows schedules freely.
- Optional **core model** for games that depend on Horizon's same-core behaviour: one lock per guest core, so only one guest thread per core runs at a time, handed over by priority. Off by default; a game profile turns it on.
- `svcSleepThread`: 0 / −1 / −2 yield (`SwitchToThread`); otherwise a high-resolution waitable timer.

## Synchronisation

Implemented to Horizon's exact rules (Atmosphère's kernel is the reference for behaviour):

- Mutexes in guest memory: owner handle plus the waiters bit `0x40000000`; `svcArbitrateLock` / `svcArbitrateUnlock`.
- Condition variables: `svcWaitProcessWideKeyAtomic` / `svcSignalProcessWideKey`.
- Address arbiter: `svcWaitForAddress` (wait if less than, decrement and wait if less than, wait if equal) and `svcSignalToAddress` (signal, signal and increment if equal, signal and modify by waiting count if equal).
- One host wait table keyed by guest address, with priority-ordered wake-ups as Horizon does them.
- `svcWaitSynchronization` over events, threads and sessions, with timeouts and `svcCancelSynchronization`.

## Handles and objects

Per-process handle table with generation bits; pseudo-handles `0xFFFF8000` (current thread) and `0xFFFF8001` (current process). Objects: thread, process, readable / writable event, client session, shared memory, transfer memory, and the few device objects nvdrv needs.

## SVCs

| Group | SVCs (id) |
|---|---|
| Memory | SetHeapSize (0x01), SetMemoryPermission (0x02), SetMemoryAttribute (0x03), MapMemory (0x04), UnmapMemory (0x05), QueryMemory (0x06), MapSharedMemory (0x13), UnmapSharedMemory (0x14), CreateTransferMemory (0x15), MapPhysicalMemory (0x2C), UnmapPhysicalMemory (0x2D) |
| Threads | CreateThread (0x08), StartThread (0x09), ExitThread (0x0A), SleepThread (0x0B), Get/SetThreadPriority (0x0C / 0x0D), Get/SetThreadCoreMask (0x0E / 0x0F), GetCurrentProcessorNumber (0x10), GetThreadId (0x25), SetThreadActivity (0x32), GetThreadContext3 (0x33) |
| Sync | SignalEvent (0x11), ClearEvent (0x12), ResetSignal (0x17), WaitSynchronization (0x18), CancelSynchronization (0x19), ArbitrateLock / Unlock (0x1A / 0x1B), WaitProcessWideKeyAtomic (0x1C), SignalProcessWideKey (0x1D), WaitForAddress (0x34), SignalToAddress (0x35), CreateEvent (0x45) |
| IPC | ConnectToNamedPort (0x1F), SendSyncRequest (0x21), SendSyncRequestWithUserBuffer (0x22), CloseHandle (0x16) |
| Info, debug | ExitProcess (0x07), GetSystemTick (0x1E), GetProcessId (0x24), Break (0x26), OutputDebugString (0x27), GetInfo (0x29) |

Any other SVC, or an unknown `GetInfo` id, stops with a report. The NPDM's SVC mask is checked too: a game calling an SVC it was not granted is a Biwa bug.

## IPC

- The message lives in the thread's TLS (the first 0x100 bytes at `TPIDRRO_EL0`).
- HIPC: header, copy / move handles, PID, descriptors (pointer X, send A, receive B, exchange W, receive-list C), raw data.
- CMIF (`SFCI` / `SFCO`) with domains (object IDs, close); TIPC for `sm:`, which the SDK uses on newer system versions.
- A request is served synchronously on the calling guest thread. A service that must block (for example waiting for audio) blocks that thread, as the real call would.
- Services are C++ classes with a table of commands. Each handler gets typed inputs and outputs, and unknown commands stop with the service name, command ID and the guest caller.

## Services

The first set: what games commonly use at boot and in play. "Real" means behaves as on the console; "offline" means answers the way a console without network or that hardware would.

| Service | Level | Notes |
|---|---|---|
| `sm:` | Real | CMIF and TIPC |
| `fsp-srv` | Real | RomFS from `romfs.bin`; DLC by data ID from `aoc/`; save data as a host folder (`save/`), with commit semantics; no SD card |
| `hid` | Real | Shared memory layout, Pro Controller and handheld; [08](08-audio-input-media.md) |
| `appletOE` and its interfaces | Real | Operation mode (docked by default), focus, message queue, launch parameters, desired language, `EnsureSaveData`; software keyboard and error applet later |
| `acc:u0` | Real | One user, named in the config |
| `set`, `set:sys` | Real | Language, region, reported system version |
| `time:u` | Real | System clock from the host's UTC, steady clock, time zone rules |
| `pl:u` | Real | Shared fonts: from the player's own system dump, or open replacement fonts whose licences are checked first |
| `audren:u`, `audout:u`, `hwopus` | Real | [08](08-audio-input-media.md) |
| `vi:u` / `vi:m` | Minimal | Displays, layers, the vsync event; presentation itself belongs to NVN's window |
| `nvdrv` | Minimal | Only what the SDK's graphics initialisation calls before NVN takes over |
| `aoc:u` | Real | DLC count, list, prepare, change event |
| `nifm`, `bsd:u`, `sfdnsres`, `ssl`, `friends`, `bcat` | Offline | No network |
| `nfp:user`, `irs`, `ldn` | Offline | No amiibo, IR camera or local wireless |
| `pctl`, `prepo`, `caps:su`, `erpt`, `mm:u`, `apm`, `psm`, `lm` | Accept | Answer success; `lm` forwards the game's logs to Biwa's log |
| `fatal:u` | Real | Stop with the game's error code and a guest backtrace |

## Reported system

The system version Biwa reports is configurable and starts at the content meta's required system version or newer. It decides, among other things, which `sm:` protocol and HID shared memory layout the SDK uses, so services follow it.

## Open questions

- Which games need the core model; decided per game from traces.
- The shared font source: a system dump step in `biwa extract`, or replacement fonts (licence check first).
- Exact `nvdrv` surface the SDK touches before `nvnBootstrapLoader`.
