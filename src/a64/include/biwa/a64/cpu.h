/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Biwa contributors */

/* The guest CPU state (docs/design/04-cpu-recompiler.md, "CPU state"). Plain C17, shared by the
   runtime, the interpreter and the generated code. */

#ifndef BIWA_A64_CPU_H
#define BIWA_A64_CPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct BiwaThread;

typedef struct BiwaExclusiveMonitor {
    uint64_t addr;
    uint64_t lo;
    uint64_t hi;
    uint8_t size;
    uint8_t valid;
} BiwaExclusiveMonitor;

typedef struct BiwaCpu {
    uint64_t x[31];
    uint64_t sp;
    uint64_t pc;
    uint32_t nzcv; /* N, Z, C, V in bits 31..28 */
    uint32_t fpcr;
    uint32_t fpsr;
    uint32_t reserved;
    __attribute__((aligned(16))) uint8_t v[32][16];
    uint64_t tpidr_el0;
    uint64_t tpidrro_el0;
    BiwaExclusiveMonitor excl;
    struct BiwaThread* thread; /* runtime side */
} BiwaCpu;

#ifdef __cplusplus
}
#endif

#endif /* BIWA_A64_CPU_H */
