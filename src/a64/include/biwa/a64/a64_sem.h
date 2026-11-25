/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Biwa contributors */

/* A64 instruction semantics: one source of truth for the interpreter and the generated C code
   (docs/design/04-cpu-recompiler.md, "Semantics"). Plain C17 with static inline functions; it also
   compiles as C++. Operands arrive decoded and already checked: reserved encodings never get here.
   Register number 31 is XZR unless a function says SP. Results of 32-bit forms (sf = false) are
   zero-extended into the 64-bit register. */

#ifndef BIWA_A64_SEM_H
#define BIWA_A64_SEM_H

#include <stdbool.h>
#include <stdint.h>

#include "biwa/a64/cpu.h"

#ifdef __cplusplus
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
extern "C" {
#endif

#define SEM_FLAG_N 0x80000000u
#define SEM_FLAG_Z 0x40000000u
#define SEM_FLAG_C 0x20000000u
#define SEM_FLAG_V 0x10000000u

/* ---- Registers ---- */

static inline uint64_t sem_x(const BiwaCpu* cpu, unsigned r) { return r == 31 ? 0 : cpu->x[r]; }
static inline uint64_t sem_xsp(const BiwaCpu* cpu, unsigned r) { return r == 31 ? cpu->sp : cpu->x[r]; }
static inline void sem_set_x(BiwaCpu* cpu, unsigned r, uint64_t value) {
    if (r != 31) {
        cpu->x[r] = value;
    }
}
static inline void sem_set_xsp(BiwaCpu* cpu, unsigned r, uint64_t value) {
    if (r == 31) {
        cpu->sp = value;
    } else {
        cpu->x[r] = value;
    }
}
static inline uint64_t sem_width(bool sf, uint64_t value) { return sf ? value : (uint64_t)(uint32_t)value; }

/* ---- Helpers ---- */

/* AddWithCarry: a + b + carry_in at 64 or 32 bits; the flags it would set go to *nzcv. */
static inline uint64_t sem_add_with_carry(uint64_t a, uint64_t b, unsigned carry_in, bool sf, uint32_t* nzcv) {
    if (sf) {
        uint64_t sum = 0;
        const bool c1 = __builtin_add_overflow(a, b, &sum);
        const bool c2 = __builtin_add_overflow(sum, (uint64_t)carry_in, &sum);
        const uint32_t v = (uint32_t)((((a ^ sum) & (b ^ sum)) >> 63) & 1);
        *nzcv = (uint32_t)(sum >> 63) << 31 | (uint32_t)(sum == 0) << 30 | (uint32_t)(c1 || c2) << 29 | v << 28;
        return sum;
    }
    const uint32_t a32 = (uint32_t)a;
    const uint32_t b32 = (uint32_t)b;
    uint32_t sum32 = 0;
    const bool c1 = __builtin_add_overflow(a32, b32, &sum32);
    const bool c2 = __builtin_add_overflow(sum32, (uint32_t)carry_in, &sum32);
    const uint32_t v = (((a32 ^ sum32) & (b32 ^ sum32)) >> 31) & 1;
    *nzcv = (sum32 >> 31) << 31 | (uint32_t)(sum32 == 0) << 30 | (uint32_t)(c1 || c2) << 29 | v << 28;
    return sum32;
}

/* N and Z from a result, C and V clear (logical instructions). */
static inline uint32_t sem_nz(bool sf, uint64_t result) {
    const uint64_t sign = sf ? result >> 63 : (result >> 31) & 1;
    return (uint32_t)sign << 31 | (uint32_t)(sem_width(sf, result) == 0) << 30;
}

static inline uint64_t sem_ror(uint64_t value, unsigned amount, unsigned width) {
    const uint64_t mask = width == 64 ? ~(uint64_t)0 : ((uint64_t)1 << width) - 1;
    value &= mask;
    amount %= width;
    if (amount == 0) {
        return value;
    }
    return ((value >> amount) | (value << (width - amount))) & mask;
}

static inline uint64_t sem_replicate(uint64_t element, unsigned esize) {
    uint64_t out = 0;
    for (unsigned i = 0; i < 64; i += esize) {
        out |= element << i;
    }
    return out;
}

/* DecodeBitMasks from the A64 manual, for logical immediates (`immediate`) and bitfield moves.
   Returns false for a reserved encoding. */
static inline bool sem_decode_bit_masks(unsigned n, unsigned imms, unsigned immr, bool immediate, unsigned datasize,
                                        uint64_t* wmask, uint64_t* tmask) {
    const unsigned combined = (n << 6) | (~imms & 0x3F);
    if (combined == 0) {
        return false;
    }
    const unsigned len = 31u - (unsigned)__builtin_clz(combined);
    if (len < 1 || (1u << len) > datasize) {
        return false;
    }
    const unsigned levels = (1u << len) - 1;
    if (immediate && (imms & levels) == levels) {
        return false;
    }
    const unsigned s = imms & levels;
    const unsigned r = immr & levels;
    const unsigned diff = (s - r) & levels;
    const unsigned esize = 1u << len;
    const uint64_t emask = esize == 64 ? ~(uint64_t)0 : ((uint64_t)1 << esize) - 1;
    const uint64_t welem = s + 1 == 64 ? ~(uint64_t)0 : ((uint64_t)1 << (s + 1)) - 1;
    const uint64_t telem = diff + 1 == 64 ? ~(uint64_t)0 : ((uint64_t)1 << (diff + 1)) - 1;
    const uint64_t dmask = datasize == 64 ? ~(uint64_t)0 : ((uint64_t)1 << datasize) - 1;
    *wmask = sem_replicate(sem_ror(welem & emask, r, esize), esize) & dmask;
    *tmask = sem_replicate(telem & emask, esize) & dmask;
    return true;
}

/* ---- Data processing: immediate ---- */

/* ADR: Xd = pc + imm. ADRP: Xd = (pc with the low 12 bits clear) + imm (imm already shifted). */
static inline void sem_adr(BiwaCpu* cpu, unsigned d, int64_t imm) { sem_set_x(cpu, d, cpu->pc + (uint64_t)imm); }
static inline void sem_adrp(BiwaCpu* cpu, unsigned d, int64_t imm) {
    sem_set_x(cpu, d, (cpu->pc & ~(uint64_t)0xFFF) + (uint64_t)imm);
}

/* ADD, ADDS, SUB, SUBS (immediate). Rn is SP for 31; Rd is SP for 31 unless flags are set. */
static inline void sem_addsub_imm(BiwaCpu* cpu, bool sf, bool sub, bool setflags, unsigned d, unsigned n,
                                  uint64_t imm) {
    uint32_t nzcv = 0;
    const uint64_t result = sem_add_with_carry(sem_xsp(cpu, n), sub ? ~imm : imm, sub ? 1u : 0u, sf, &nzcv);
    if (setflags) {
        cpu->nzcv = nzcv;
        sem_set_x(cpu, d, result);
    } else {
        sem_set_xsp(cpu, d, result);
    }
}

/* AND, ORR, EOR, ANDS (immediate): opc 0..3. `imm` comes from sem_decode_bit_masks. Rd is SP for
   31 except for ANDS. */
static inline void sem_logical_imm(BiwaCpu* cpu, bool sf, unsigned opc, unsigned d, unsigned n, uint64_t imm) {
    const uint64_t a = sem_x(cpu, n);
    uint64_t result = 0;
    switch (opc) {
        case 0:
        case 3:
            result = a & imm;
            break;
        case 1:
            result = a | imm;
            break;
        default:
            result = a ^ imm;
            break;
    }
    result = sem_width(sf, result);
    if (opc == 3) {
        cpu->nzcv = sem_nz(sf, result);
        sem_set_x(cpu, d, result);
    } else {
        sem_set_xsp(cpu, d, result);
    }
}

/* MOVN (opc 0), MOVZ (opc 2), MOVK (opc 3); `shift` is hw * 16. */
static inline void sem_move_wide(BiwaCpu* cpu, bool sf, unsigned opc, unsigned d, unsigned shift, uint64_t imm16) {
    uint64_t result = 0;
    if (opc == 3) {
        result = (sem_x(cpu, d) & ~((uint64_t)0xFFFF << shift)) | (imm16 << shift);
    } else {
        result = imm16 << shift;
        if (opc == 0) {
            result = ~result;
        }
    }
    sem_set_x(cpu, d, sem_width(sf, result));
}

/* SBFM (opc 0), BFM (opc 1), UBFM (opc 2) with the masks from sem_decode_bit_masks. */
static inline void sem_bitfield(BiwaCpu* cpu, bool sf, unsigned opc, unsigned d, unsigned n, unsigned immr,
                                unsigned imms, uint64_t wmask, uint64_t tmask) {
    const unsigned datasize = sf ? 64 : 32;
    const uint64_t dst = opc == 1 ? sem_x(cpu, d) : 0;
    const uint64_t src = sem_width(sf, sem_x(cpu, n));
    const uint64_t bot = (dst & ~wmask) | (sem_ror(src, immr, datasize) & wmask);
    uint64_t top = dst;
    if (opc == 0) {
        top = ((src >> imms) & 1) != 0 ? ~(uint64_t)0 : 0;
    }
    sem_set_x(cpu, d, sem_width(sf, (top & ~tmask) | (bot & tmask)));
}

/* EXTR: the `datasize` bits starting at `lsb` of Rn:Rm. */
static inline void sem_extr(BiwaCpu* cpu, bool sf, unsigned d, unsigned n, unsigned m, unsigned lsb) {
    const uint64_t hi = sem_width(sf, sem_x(cpu, n));
    const uint64_t lo = sem_width(sf, sem_x(cpu, m));
    uint64_t result = lo;
    if (lsb != 0) {
        result = sf ? (lo >> lsb) | (hi << (64 - lsb)) : (lo >> lsb) | (hi << (32 - lsb));
    }
    sem_set_x(cpu, d, sem_width(sf, result));
}

#ifdef __cplusplus
}
#pragma clang diagnostic pop
#endif

#endif /* BIWA_A64_SEM_H */
