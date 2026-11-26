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

#if defined(__x86_64__)
#include <xmmintrin.h>
#else
#include <fenv.h>
#endif

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

/* ---- Data processing: register ---- */

/* The condition codes of B.cond, CSEL, CCMP, ...: EQ NE CS CC MI PL VS VC HI LS GE LT GT LE AL NV. */
static inline bool sem_condition_holds(uint32_t nzcv, unsigned cond) {
    const bool n = (nzcv & SEM_FLAG_N) != 0;
    const bool z = (nzcv & SEM_FLAG_Z) != 0;
    const bool c = (nzcv & SEM_FLAG_C) != 0;
    const bool v = (nzcv & SEM_FLAG_V) != 0;
    bool result = false;
    switch (cond >> 1) {
        case 0:
            result = z;
            break;
        case 1:
            result = c;
            break;
        case 2:
            result = n;
            break;
        case 3:
            result = v;
            break;
        case 4:
            result = c && !z;
            break;
        case 5:
            result = n == v;
            break;
        case 6:
            result = n == v && !z;
            break;
        default:
            return true; /* AL and NV */
    }
    return (cond & 1) != 0 ? !result : result;
}

/* LSL (0), LSR (1), ASR (2), ROR (3) by `amount` (below the width). */
static inline uint64_t sem_shift(bool sf, uint64_t value, unsigned type, unsigned amount) {
    const unsigned width = sf ? 64 : 32;
    value = sem_width(sf, value);
    if (amount == 0) {
        return value;
    }
    switch (type) {
        case 0:
            return sem_width(sf, value << amount);
        case 1:
            return value >> amount;
        case 2:
            if (sf) {
                return (uint64_t)((int64_t)value >> amount);
            }
            return (uint64_t)(uint32_t)((int32_t)(uint32_t)value >> amount);
        default:
            return sem_ror(value, amount, width);
    }
}

/* AND, ORR, EOR, ANDS (opc 0..3) with a shifted register; `invert` gives BIC, ORN, EON, BICS. */
static inline void sem_logical_shift(BiwaCpu* cpu, bool sf, unsigned opc, bool invert, unsigned d, unsigned n,
                                     unsigned m, unsigned type, unsigned amount) {
    uint64_t operand = sem_shift(sf, sem_x(cpu, m), type, amount);
    if (invert) {
        operand = ~operand;
    }
    const uint64_t a = sem_x(cpu, n);
    uint64_t result = 0;
    switch (opc) {
        case 0:
        case 3:
            result = a & operand;
            break;
        case 1:
            result = a | operand;
            break;
        default:
            result = a ^ operand;
            break;
    }
    result = sem_width(sf, result);
    if (opc == 3) {
        cpu->nzcv = sem_nz(sf, result);
    }
    sem_set_x(cpu, d, result);
}

/* ADD, ADDS, SUB, SUBS (shifted register: LSL, LSR or ASR). */
static inline void sem_addsub_shift(BiwaCpu* cpu, bool sf, bool sub, bool setflags, unsigned d, unsigned n, unsigned m,
                                    unsigned type, unsigned amount) {
    const uint64_t operand = sem_shift(sf, sem_x(cpu, m), type, amount);
    uint32_t nzcv = 0;
    const uint64_t result = sem_add_with_carry(sem_x(cpu, n), sub ? ~operand : operand, sub ? 1u : 0u, sf, &nzcv);
    if (setflags) {
        cpu->nzcv = nzcv;
    }
    sem_set_x(cpu, d, result);
}

/* Rm extended by `option` (UXTB, UXTH, UXTW, UXTX, SXTB, SXTH, SXTW, SXTX) and shifted left by
   `shift` (0..4). */
static inline uint64_t sem_extend(uint64_t value, unsigned option, unsigned shift) {
    switch (option) {
        case 0:
            value = (uint8_t)value;
            break;
        case 1:
            value = (uint16_t)value;
            break;
        case 2:
            value = (uint32_t)value;
            break;
        case 4:
            value = (uint64_t)(int64_t)(int8_t)(uint8_t)value;
            break;
        case 5:
            value = (uint64_t)(int64_t)(int16_t)(uint16_t)value;
            break;
        case 6:
            value = (uint64_t)(int64_t)(int32_t)(uint32_t)value;
            break;
        default:
            break;
    }
    return value << shift;
}

/* ADD, ADDS, SUB, SUBS (extended register). Rn is SP for 31; Rd too unless flags are set. */
static inline void sem_addsub_ext(BiwaCpu* cpu, bool sf, bool sub, bool setflags, unsigned d, unsigned n, unsigned m,
                                  unsigned option, unsigned shift) {
    const uint64_t operand = sem_extend(sem_x(cpu, m), option, shift);
    uint32_t nzcv = 0;
    const uint64_t result = sem_add_with_carry(sem_xsp(cpu, n), sub ? ~operand : operand, sub ? 1u : 0u, sf, &nzcv);
    if (setflags) {
        cpu->nzcv = nzcv;
        sem_set_x(cpu, d, result);
    } else {
        sem_set_xsp(cpu, d, result);
    }
}

/* ADC, ADCS, SBC, SBCS. */
static inline void sem_adc(BiwaCpu* cpu, bool sf, bool sub, bool setflags, unsigned d, unsigned n, unsigned m) {
    const uint64_t operand = sem_x(cpu, m);
    uint32_t nzcv = 0;
    const unsigned carry = (cpu->nzcv & SEM_FLAG_C) != 0 ? 1u : 0u;
    const uint64_t result = sem_add_with_carry(sem_x(cpu, n), sub ? ~operand : operand, carry, sf, &nzcv);
    if (setflags) {
        cpu->nzcv = nzcv;
    }
    sem_set_x(cpu, d, result);
}

/* CCMN (sub false) and CCMP (sub true) with `operand` (a register's value or a 5-bit immediate):
   the flags of Rn + / - operand if `cond` holds, otherwise `flags` (nzcv in bits 3..0). */
static inline void sem_ccmp(BiwaCpu* cpu, bool sf, bool sub, unsigned n, uint64_t operand, unsigned cond,
                            unsigned flags) {
    if (sem_condition_holds(cpu->nzcv, cond)) {
        uint32_t nzcv = 0;
        (void)sem_add_with_carry(sem_x(cpu, n), sub ? ~operand : operand, sub ? 1u : 0u, sf, &nzcv);
        cpu->nzcv = nzcv;
    } else {
        cpu->nzcv = (uint32_t)flags << 28;
    }
}

/* CSEL (op 0), CSINC (1), CSINV (2), CSNEG (3). */
static inline void sem_csel(BiwaCpu* cpu, bool sf, unsigned op, unsigned d, unsigned n, unsigned m, unsigned cond) {
    uint64_t result = sem_x(cpu, n);
    if (!sem_condition_holds(cpu->nzcv, cond)) {
        result = sem_x(cpu, m);
        switch (op) {
            case 1:
                result += 1;
                break;
            case 2:
                result = ~result;
                break;
            case 3:
                result = 0 - result;
                break;
            default:
                break;
        }
    }
    sem_set_x(cpu, d, sem_width(sf, result));
}

/* CRC32B/H/W/X (castagnoli false) and CRC32CB/H/W/X: Wn updated with the low `bytes` of Rm,
   bit-reflected, without the initial and final inversions. */
static inline void sem_crc32(BiwaCpu* cpu, bool castagnoli, unsigned bytes, unsigned d, unsigned n, unsigned m) {
    const uint32_t poly = castagnoli ? 0x82F63B78u : 0xEDB88320u;
    uint32_t crc = (uint32_t)sem_x(cpu, n);
    const uint64_t value = sem_x(cpu, m);
    for (unsigned i = 0; i < 8 * bytes; ++i) {
        const uint32_t bit = (crc ^ (uint32_t)(value >> i)) & 1;
        crc = (crc >> 1) ^ (bit != 0 ? poly : 0);
    }
    sem_set_x(cpu, d, crc);
}

/* RBIT. */
static inline void sem_rbit(BiwaCpu* cpu, bool sf, unsigned d, unsigned n) {
    uint64_t value = sem_x(cpu, n);
    uint64_t result = 0;
    const unsigned width = sf ? 64 : 32;
    for (unsigned i = 0; i < width; ++i) {
        result = (result << 1) | ((value >> i) & 1);
    }
    sem_set_x(cpu, d, result);
}

/* REV16 (container 2 bytes), REV32 (4) and REV (the whole register): bytes reversed in each
   container. */
static inline void sem_rev(BiwaCpu* cpu, bool sf, unsigned container, unsigned d, unsigned n) {
    const uint64_t value = sem_x(cpu, n);
    const unsigned bytes = sf ? 8 : 4;
    uint64_t result = 0;
    for (unsigned base = 0; base < bytes; base += container) {
        for (unsigned i = 0; i < container; ++i) {
            const uint64_t byte = (value >> (8 * (base + i))) & 0xFF;
            result |= byte << (8 * (base + container - 1 - i));
        }
    }
    sem_set_x(cpu, d, result);
}

/* CLZ and CLS (count leading sign bits). */
static inline void sem_clz(BiwaCpu* cpu, bool sf, bool cls, unsigned d, unsigned n) {
    uint64_t value = sem_width(sf, sem_x(cpu, n));
    const unsigned width = sf ? 64 : 32;
    if (cls) {
        /* The bits after the sign bit that equal it: CLZ of value ^ (value >> 1), less one. */
        value = sem_width(sf, value ^ (value >> 1)) & (sf ? ~(uint64_t)0 >> 1 : 0x7FFFFFFFu);
    }
    unsigned count = width;
    if (value != 0) {
        count = (unsigned)__builtin_clzll(value) - (64 - width);
    }
    sem_set_x(cpu, d, cls ? count - 1 : count);
}

/* UDIV and SDIV: division by zero gives 0; the signed overflow case wraps. */
static inline void sem_div(BiwaCpu* cpu, bool sf, bool is_signed, unsigned d, unsigned n, unsigned m) {
    uint64_t result = 0;
    if (sf) {
        const uint64_t a = sem_x(cpu, n);
        const uint64_t b = sem_x(cpu, m);
        if (b == 0) {
            result = 0;
        } else if (!is_signed) {
            result = a / b;
        } else if (a == (uint64_t)1 << 63 && b == ~(uint64_t)0) {
            result = a;
        } else {
            result = (uint64_t)((int64_t)a / (int64_t)b);
        }
    } else {
        const uint32_t a = (uint32_t)sem_x(cpu, n);
        const uint32_t b = (uint32_t)sem_x(cpu, m);
        if (b == 0) {
            result = 0;
        } else if (!is_signed) {
            result = a / b;
        } else if (a == 0x80000000u && b == 0xFFFFFFFFu) {
            result = a;
        } else {
            result = (uint32_t)((int32_t)a / (int32_t)b);
        }
    }
    sem_set_x(cpu, d, result);
}

/* LSLV, LSRV, ASRV, RORV (type 0..3): shift by Rm modulo the width. */
static inline void sem_shiftv(BiwaCpu* cpu, bool sf, unsigned type, unsigned d, unsigned n, unsigned m) {
    const unsigned amount = (unsigned)(sem_x(cpu, m) & (sf ? 63u : 31u));
    sem_set_x(cpu, d, sem_shift(sf, sem_x(cpu, n), type, amount));
}

/* MADD and MSUB: Ra + / - Rn * Rm. */
static inline void sem_madd(BiwaCpu* cpu, bool sf, bool sub, unsigned d, unsigned n, unsigned m, unsigned a) {
    const uint64_t product = sem_x(cpu, n) * sem_x(cpu, m);
    sem_set_x(cpu, d, sem_width(sf, sub ? sem_x(cpu, a) - product : sem_x(cpu, a) + product));
}

/* SMADDL, SMSUBL, UMADDL, UMSUBL: Xa + / - Wn * Wm, the product 64-bit. */
static inline void sem_maddl(BiwaCpu* cpu, bool is_signed, bool sub, unsigned d, unsigned n, unsigned m, unsigned a) {
    uint64_t product = 0;
    if (is_signed) {
        product = (uint64_t)((int64_t)(int32_t)(uint32_t)sem_x(cpu, n) * (int64_t)(int32_t)(uint32_t)sem_x(cpu, m));
    } else {
        product = (uint64_t)(uint32_t)sem_x(cpu, n) * (uint64_t)(uint32_t)sem_x(cpu, m);
    }
    sem_set_x(cpu, d, sub ? sem_x(cpu, a) - product : sem_x(cpu, a) + product);
}

/* SMULH and UMULH: the high 64 bits of the 128-bit product. */
static inline void sem_mulh(BiwaCpu* cpu, bool is_signed, unsigned d, unsigned n, unsigned m) {
    uint64_t result = 0;
    if (is_signed) {
        const __int128 product = (__int128)(int64_t)sem_x(cpu, n) * (__int128)(int64_t)sem_x(cpu, m);
        result = (uint64_t)(product >> 64);
    } else {
        const unsigned __int128 product = (unsigned __int128)sem_x(cpu, n) * (unsigned __int128)sem_x(cpu, m);
        result = (uint64_t)(product >> 64);
    }
    sem_set_x(cpu, d, result);
}

/* ---- Loads and stores ---- */

/* Guest addresses are host addresses (docs/design/05-runtime-os.md, "Guest address space"), so an
   access is a plain host access. Unaligned accesses are allowed, as for normal memory on the
   console. Addresses come from the caller: base register, offset and writeback are the
   addressing mode's business. */

/* The only places that turn a guest address into a host pointer. Any address may arrive, 0
   included: an unmapped one faults on the host, and the runtime reports it as a guest data abort
   (docs/design/04-cpu-recompiler.md, "Memory accesses"). */
/* NOLINTBEGIN(performance-no-int-to-ptr) */
/* NOLINTBEGIN(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */
/* NOLINTBEGIN(clang-analyzer-unix.cstring.NullArg) */
static inline void* sem_host(uint64_t address) { return (void*)(uintptr_t)address; }

static inline void sem_copy_in(void* host, uint64_t address, unsigned bytes) {
    __builtin_memcpy(host, sem_host(address), bytes);
}

static inline void sem_copy_out(uint64_t address, const void* host, unsigned bytes) {
    __builtin_memcpy(sem_host(address), host, bytes);
}

/* Bit patterns and host floating-point values. */
static inline float sem_f32(uint32_t bits) {
    float value = 0;
    __builtin_memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline double sem_f64(uint64_t bits) {
    double value = 0;
    __builtin_memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline uint32_t sem_f32_bits(float value) {
    uint32_t bits = 0;
    __builtin_memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static inline uint64_t sem_f64_bits(double value) {
    uint64_t bits = 0;
    __builtin_memcpy(&bits, &value, sizeof(bits));
    return bits;
}
/* NOLINTEND(clang-analyzer-unix.cstring.NullArg) */
/* NOLINTEND(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */
/* NOLINTEND(performance-no-int-to-ptr) */

static inline uint64_t sem_read(uint64_t address, unsigned bytes) {
    uint64_t value = 0;
    sem_copy_in(&value, address, bytes);
    return value;
}

static inline void sem_write(uint64_t address, unsigned bytes, uint64_t value) { sem_copy_out(address, &value, bytes); }

static inline uint64_t sem_sign_extend(uint64_t value, unsigned bits) {
    const uint64_t sign = (uint64_t)1 << (bits - 1);
    return ((value & ((sign << 1) - 1)) ^ sign) - sign;
}

/* STR, LDR and their byte, halfword and signed forms on Xt / Wt (any addressing mode). `size` is
   log2 of the access in bytes; `opc` as encoded: 0 store, 1 load, 2 load sign-extended to 64 bits,
   3 load sign-extended to 32 bits. */
static inline void sem_ldst(BiwaCpu* cpu, unsigned size, unsigned opc, unsigned t, uint64_t address) {
    const unsigned bytes = 1u << size;
    if (opc == 0) {
        sem_write(address, bytes, sem_x(cpu, t));
        return;
    }
    uint64_t value = sem_read(address, bytes);
    if (opc >= 2 && size < 3) {
        value = sem_sign_extend(value, 8 * bytes);
        if (opc == 3) {
            value = (uint32_t)value;
        }
    }
    sem_set_x(cpu, t, value);
}

/* STR and LDR (SIMD&FP) of Bt, Ht, St, Dt or Qt (`scale` 0..4); loads clear the rest of the
   register. */
static inline void sem_ldst_v(BiwaCpu* cpu, unsigned scale, bool load, unsigned t, uint64_t address) {
    const unsigned bytes = 1u << scale;
    if (load) {
        for (unsigned i = bytes; i < 16; ++i) {
            cpu->v[t][i] = 0;
        }
        sem_copy_in(cpu->v[t], address, bytes);
    } else {
        sem_copy_out(address, cpu->v[t], bytes);
    }
}

/* STP, LDP, STNP, LDNP (`size` 2 for W, 3 for X) and LDPSW (`sign`). */
static inline void sem_ldst_pair(BiwaCpu* cpu, unsigned size, bool load, bool sign, unsigned t, unsigned t2,
                                 uint64_t address) {
    const unsigned bytes = 1u << size;
    if (!load) {
        sem_write(address, bytes, sem_x(cpu, t));
        sem_write(address + bytes, bytes, sem_x(cpu, t2));
        return;
    }
    uint64_t first = sem_read(address, bytes);
    uint64_t second = sem_read(address + bytes, bytes);
    if (sign) {
        first = sem_sign_extend(first, 32);
        second = sem_sign_extend(second, 32);
    }
    /* Rt is written last: LDP with Rt == Rt2 is CONSTRAINED UNPREDICTABLE, and this matches Unicorn. */
    sem_set_x(cpu, t2, second);
    sem_set_x(cpu, t, first);
}

/* STP, LDP, STNP, LDNP (SIMD&FP) of St, Dt or Qt (`scale` 2..4). */
static inline void sem_ldst_pair_v(BiwaCpu* cpu, unsigned scale, bool load, unsigned t, unsigned t2, uint64_t address) {
    sem_ldst_v(cpu, scale, load, t, address);
    sem_ldst_v(cpu, scale, load, t2, address + (1u << scale));
}

/* LD1-LD4 and ST1-ST4 (multiple structures): `rpt` groups of `selem` registers from Vt (wrapping
   at 31), elements of 1 << size bytes interleaved across each group's registers. 64-bit loads (q
   false) clear the upper halves. Returns the bytes transferred. */
static inline uint64_t sem_ldst_multiple(BiwaCpu* cpu, bool load, bool q, unsigned size, unsigned rpt, unsigned selem,
                                         unsigned t, uint64_t address) {
    const unsigned ebytes = 1u << size;
    const unsigned elements = (q ? 16u : 8u) / ebytes;
    uint64_t offset = 0;
    if (load && !q) {
        for (unsigned r = 0; r < rpt * selem; ++r) {
            for (unsigned i = 8; i < 16; ++i) {
                cpu->v[(t + r) % 32][i] = 0;
            }
        }
    }
    for (unsigned r = 0; r < rpt; ++r) {
        for (unsigned e = 0; e < elements; ++e) {
            for (unsigned s = 0; s < selem; ++s) {
                uint8_t* lane = &cpu->v[(t + r + s) % 32][(uint64_t)e * ebytes];
                if (load) {
                    sem_copy_in(lane, address + offset, ebytes);
                } else {
                    sem_copy_out(address + offset, lane, ebytes);
                }
                offset += ebytes;
            }
        }
    }
    return offset;
}

/* LD1-LD4 and ST1-ST4 (single structure): element `index` (1 << scale bytes) of `selem`
   consecutive registers from Vt; the other elements stay. Returns the bytes transferred. */
static inline uint64_t sem_ldst_single(BiwaCpu* cpu, bool load, unsigned scale, unsigned selem, unsigned index,
                                       unsigned t, uint64_t address) {
    const unsigned ebytes = 1u << scale;
    for (unsigned s = 0; s < selem; ++s) {
        uint8_t* lane = &cpu->v[(t + s) % 32][(uint64_t)index * ebytes];
        if (load) {
            sem_copy_in(lane, address + (uint64_t)s * ebytes, ebytes);
        } else {
            sem_copy_out(address + (uint64_t)s * ebytes, lane, ebytes);
        }
    }
    return (uint64_t)selem * ebytes;
}

/* LD1R-LD4R: one element (1 << scale bytes) per register, copied to all of its elements (the low
   64 bits only when q is false; the rest is cleared). Returns the bytes transferred. */
static inline uint64_t sem_ld_replicate(BiwaCpu* cpu, bool q, unsigned scale, unsigned selem, unsigned t,
                                        uint64_t address) {
    const unsigned ebytes = 1u << scale;
    const unsigned bytes = q ? 16u : 8u;
    for (unsigned s = 0; s < selem; ++s) {
        uint8_t element[8] = {0};
        sem_copy_in(element, address + (uint64_t)s * ebytes, ebytes);
        uint8_t* reg = cpu->v[(t + s) % 32];
        for (unsigned i = 0; i < 16; ++i) {
            reg[i] = i < bytes ? element[i % ebytes] : 0;
        }
    }
    return (uint64_t)selem * ebytes;
}

/* ---- Branches and system ---- */

/* CBZ / CBNZ and TBZ / TBNZ conditions (the branch itself is the caller's: a goto in generated
   code, a pc update in the interpreter). */
static inline bool sem_is_zero(const BiwaCpu* cpu, bool sf, unsigned t) { return sem_width(sf, sem_x(cpu, t)) == 0; }

static inline bool sem_bit_is_zero(const BiwaCpu* cpu, unsigned t, unsigned bit) {
    return ((sem_x(cpu, t) >> bit) & 1) == 0;
}

/* DMB and DSB: a host fence as strong as CRm asks (loads, stores, or everything). ISB needs none. */
static inline void sem_barrier(unsigned crm) {
    switch (crm & 3) {
        case 1:
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            break;
        case 2:
            __atomic_thread_fence(__ATOMIC_RELEASE);
            break;
        default:
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            break;
    }
}

/* The console's Cortex-A57 values of the identification registers EL0 can read. */
#define SEM_CTR_EL0 0x8444C004u  /* 64-byte cache lines */
#define SEM_DCZID_EL0 4u         /* DC ZVA zeroes 64 bytes */
#define SEM_CNTFRQ_EL0 19200000u /* the system counter runs at 19.2 MHz */
/* FPCR keeps AHP, DN, FZ, RMode, Stride and Len; the A57 has no FP exception traps (RAZ/WI). */
#define SEM_FPCR_MASK 0x07F70000u
#define SEM_FPSR_MASK 0xF800009Fu

/* DC ZVA: zero the 64-byte block holding `address`. */
static inline void sem_dc_zva(uint64_t address) {
    const uint64_t zeros[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    sem_copy_out(address & ~(uint64_t)63, zeros, sizeof(zeros));
}

/* ---- Exclusives, load-acquire and store-release ---- */

/* These accesses need their natural alignment (the whole pair for LDXP / STXP); the caller checks
   it and raises an alignment fault otherwise, as the A57 does. */
static inline bool sem_is_aligned(uint64_t address, unsigned bytes) { return (address & (bytes - 1)) == 0; }

/* A single-copy atomic load of 1, 2, 4 or 8 bytes with acquire order. */
static inline uint64_t sem_read_acquire(uint64_t address, unsigned bytes) {
    void* p = sem_host(address);
    switch (bytes) {
        case 1:
            return __atomic_load_n((uint8_t*)p, __ATOMIC_ACQUIRE);
        case 2:
            return __atomic_load_n((uint16_t*)p, __ATOMIC_ACQUIRE);
        case 4:
            return __atomic_load_n((uint32_t*)p, __ATOMIC_ACQUIRE);
        default:
            return __atomic_load_n((uint64_t*)p, __ATOMIC_ACQUIRE);
    }
}

/* A store-release. Sequentially consistent, because ARM's STLR may not pass a later LDAR, which a
   C release store followed by an acquire load allows. */
static inline void sem_write_release(uint64_t address, unsigned bytes, uint64_t value) {
    void* p = sem_host(address);
    switch (bytes) {
        case 1:
            __atomic_store_n((uint8_t*)p, (uint8_t)value, __ATOMIC_SEQ_CST);
            break;
        case 2:
            __atomic_store_n((uint16_t*)p, (uint16_t)value, __ATOMIC_SEQ_CST);
            break;
        case 4:
            __atomic_store_n((uint32_t*)p, (uint32_t)value, __ATOMIC_SEQ_CST);
            break;
        default:
            __atomic_store_n((uint64_t*)p, value, __ATOMIC_SEQ_CST);
            break;
    }
}

/* Compare-and-swap of 1, 2, 4 or 8 bytes. */
static inline bool sem_cas(uint64_t address, unsigned bytes, uint64_t expected, uint64_t desired) {
    void* p = sem_host(address);
    switch (bytes) {
        case 1: {
            uint8_t e = (uint8_t)expected;
            return __atomic_compare_exchange_n((uint8_t*)p, &e, (uint8_t)desired, false, __ATOMIC_SEQ_CST,
                                               __ATOMIC_SEQ_CST);
        }
        case 2: {
            uint16_t e = (uint16_t)expected;
            return __atomic_compare_exchange_n((uint16_t*)p, &e, (uint16_t)desired, false, __ATOMIC_SEQ_CST,
                                               __ATOMIC_SEQ_CST);
        }
        case 4: {
            uint32_t e = (uint32_t)expected;
            return __atomic_compare_exchange_n((uint32_t*)p, &e, (uint32_t)desired, false, __ATOMIC_SEQ_CST,
                                               __ATOMIC_SEQ_CST);
        }
        default: {
            uint64_t e = expected;
            return __atomic_compare_exchange_n((uint64_t*)p, &e, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        }
    }
}

/* 16 bytes, low half first in memory (cmpxchg16b, part of the x86-64-v3 baseline). */
static inline void sem_read16_acquire(uint64_t address, uint64_t* lo, uint64_t* hi) {
    const unsigned __int128 value = __atomic_load_n((unsigned __int128*)sem_host(address), __ATOMIC_ACQUIRE);
    *lo = (uint64_t)value;
    *hi = (uint64_t)(value >> 64);
}

static inline bool sem_cas16(uint64_t address, uint64_t expected_lo, uint64_t expected_hi, uint64_t lo, uint64_t hi) {
    unsigned __int128 expected = ((unsigned __int128)expected_hi << 64) | expected_lo;
    const unsigned __int128 desired = ((unsigned __int128)hi << 64) | lo;
    return __atomic_compare_exchange_n((unsigned __int128*)sem_host(address), &expected, desired, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static inline void sem_monitor_set(BiwaCpu* cpu, uint64_t address, unsigned bytes, uint64_t lo, uint64_t hi) {
    cpu->excl.addr = address;
    cpu->excl.lo = lo;
    cpu->excl.hi = hi;
    cpu->excl.size = (uint8_t)bytes;
    cpu->excl.valid = 1;
}

/* Whether the monitor still marks `bytes` at `address`; it is cleared either way, as every
   store-exclusive does. */
static inline bool sem_monitor_take(BiwaCpu* cpu, uint64_t address, unsigned bytes) {
    const bool marked = cpu->excl.valid != 0 && cpu->excl.addr == address && cpu->excl.size == bytes;
    cpu->excl.valid = 0;
    return marked;
}

/* LDXR / LDAXR: load 1 << size bytes into Rt and mark them exclusive. Both have acquire order
   (stronger than LDXR needs). */
static inline void sem_ldxr(BiwaCpu* cpu, unsigned size, unsigned t, uint64_t address) {
    const unsigned bytes = 1u << size;
    const uint64_t value = sem_read_acquire(address, bytes);
    sem_monitor_set(cpu, address, bytes, value, 0);
    sem_set_x(cpu, t, value);
}

/* LDXP / LDAXP: two W (size 2) or two X (size 3), read as one atomic access and marked. */
static inline void sem_ldxp(BiwaCpu* cpu, unsigned size, unsigned t, unsigned t2, uint64_t address) {
    uint64_t lo = 0;
    uint64_t hi = 0;
    if (size == 2) {
        const uint64_t value = sem_read_acquire(address, 8);
        sem_monitor_set(cpu, address, 8, value, 0);
        lo = (uint32_t)value;
        hi = value >> 32;
    } else {
        sem_read16_acquire(address, &lo, &hi);
        sem_monitor_set(cpu, address, 16, lo, hi);
    }
    sem_set_x(cpu, t, lo);
    sem_set_x(cpu, t2, hi);
}

/* STXR / STLXR: store Rt only if the monitor still marks this address and size and memory still
   holds what the load-exclusive read (a compare-and-swap, so another thread's store in between
   makes it fail). Ws = 0 when stored, 1 otherwise. */
static inline void sem_stxr(BiwaCpu* cpu, unsigned size, unsigned s, unsigned t, uint64_t address) {
    const unsigned bytes = 1u << size;
    const uint64_t expected = cpu->excl.lo;
    const bool stored = sem_monitor_take(cpu, address, bytes) && sem_cas(address, bytes, expected, sem_x(cpu, t));
    sem_set_x(cpu, s, stored ? 0 : 1);
}

/* STXP / STLXP: as STXR for the pair Rt (lower address), Rt2. */
static inline void sem_stxp(BiwaCpu* cpu, unsigned size, unsigned s, unsigned t, unsigned t2, uint64_t address) {
    const uint64_t expected_lo = cpu->excl.lo;
    const uint64_t expected_hi = cpu->excl.hi;
    bool stored = false;
    if (sem_monitor_take(cpu, address, 2u << size)) {
        if (size == 2) {
            const uint64_t pair = (sem_x(cpu, t2) << 32) | (uint32_t)sem_x(cpu, t);
            stored = sem_cas(address, 8, expected_lo, pair);
        } else {
            stored = sem_cas16(address, expected_lo, expected_hi, sem_x(cpu, t), sem_x(cpu, t2));
        }
    }
    sem_set_x(cpu, s, stored ? 0 : 1);
}

static inline void sem_clrex(BiwaCpu* cpu) { cpu->excl.valid = 0; }

/* LDAR and STLR of 1 << size bytes. */
static inline void sem_ldar(BiwaCpu* cpu, unsigned size, unsigned t, uint64_t address) {
    sem_set_x(cpu, t, sem_read_acquire(address, 1u << size));
}

static inline void sem_stlr(const BiwaCpu* cpu, unsigned size, unsigned t, uint64_t address) {
    sem_write_release(address, 1u << size, sem_x(cpu, t));
}

/* ---- Floating point: environment, moves and compares ---- */

/* FPCR and FPSR fields. */
#define SEM_FPCR_AHP (1u << 26)
#define SEM_FPCR_DN (1u << 25)
#define SEM_FPCR_FZ (1u << 24)
#define SEM_FPCR_RMODE_SHIFT 22
#define SEM_FPSR_IOC 0x01u
#define SEM_FPSR_DZC 0x02u
#define SEM_FPSR_OFC 0x04u
#define SEM_FPSR_UFC 0x08u
#define SEM_FPSR_IXC 0x10u
#define SEM_FPSR_IDC 0x80u

/* The guest's FP environment lives partly in the host's (docs/design/04-cpu-recompiler.md,
   "Floating point and SIMD"): the host rounding mode follows FPCR.RMode, and the exception flags
   host arithmetic raises gather in the host's flags until they are folded into FPSR. Flags that
   only the helpers know (input denormals, signalling NaNs they handle themselves) go straight to
   FPSR. Flush-to-zero and default NaN are done by the helpers, so the host's FTZ and DAZ stay off. */

#if defined(__x86_64__)
#define SEM_MXCSR_FLAGS 0x3Fu
#define SEM_MXCSR_MASKS 0x1F80u
#define SEM_MXCSR_RC 0x6000u

/* ARM RMode (nearest, +inf, -inf, zero) as MXCSR.RC (nearest, -inf, +inf, zero). */
static inline unsigned sem_mxcsr_rounding(uint32_t fpcr) {
    switch ((fpcr >> SEM_FPCR_RMODE_SHIFT) & 3) {
        case 1:
            return 0x4000u;
        case 2:
            return 0x2000u;
        case 3:
            return 0x6000u;
        default:
            return 0;
    }
}
#endif

/* The host's raised flags as FPSR bits. */
static inline uint32_t sem_host_flags(void) {
#if defined(__x86_64__)
    const unsigned csr = _mm_getcsr();
    return ((csr & 0x01u) != 0 ? SEM_FPSR_IOC : 0) | ((csr & 0x04u) != 0 ? SEM_FPSR_DZC : 0) |
           ((csr & 0x08u) != 0 ? SEM_FPSR_OFC : 0) | ((csr & 0x10u) != 0 ? SEM_FPSR_UFC : 0) |
           ((csr & 0x20u) != 0 ? SEM_FPSR_IXC : 0);
#else
    const int raised = fetestexcept(FE_ALL_EXCEPT);
    return ((raised & FE_INVALID) != 0 ? SEM_FPSR_IOC : 0) | ((raised & FE_DIVBYZERO) != 0 ? SEM_FPSR_DZC : 0) |
           ((raised & FE_OVERFLOW) != 0 ? SEM_FPSR_OFC : 0) | ((raised & FE_UNDERFLOW) != 0 ? SEM_FPSR_UFC : 0) |
           ((raised & FE_INEXACT) != 0 ? SEM_FPSR_IXC : 0);
#endif
}

static inline void sem_host_clear_flags(void) {
#if defined(__x86_64__)
    _mm_setcsr(_mm_getcsr() & ~SEM_MXCSR_FLAGS);
#else
    feclearexcept(FE_ALL_EXCEPT);
#endif
}

static inline void sem_host_set_rounding(uint32_t fpcr) {
#if defined(__x86_64__)
    _mm_setcsr((_mm_getcsr() & ~SEM_MXCSR_RC) | sem_mxcsr_rounding(fpcr));
#else
    static const int modes[4] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
    fesetround(modes[(fpcr >> SEM_FPCR_RMODE_SHIFT) & 3]);
#endif
}

/* Before guest code runs on a host thread: FPCR's rounding mode, no flags raised. */
static inline void sem_fp_enter(const BiwaCpu* cpu) {
#if defined(__x86_64__)
    _mm_setcsr(SEM_MXCSR_MASKS | sem_mxcsr_rounding(cpu->fpcr));
#else
    feclearexcept(FE_ALL_EXCEPT);
    sem_host_set_rounding(cpu->fpcr);
#endif
}

/* Folds the host's raised flags into FPSR (when FPSR is read, and when guest code stops). */
static inline void sem_fp_fold(BiwaCpu* cpu) {
    cpu->fpsr |= sem_host_flags();
    sem_host_clear_flags();
}

/* FP registers: the low `bytes` of Vn; writes clear the rest of the register. */
static inline uint64_t sem_v_get(const BiwaCpu* cpu, unsigned n, unsigned bytes) {
    uint64_t bits = 0;
    for (unsigned i = 0; i < bytes; ++i) {
        bits |= (uint64_t)cpu->v[n][i] << (8 * i);
    }
    return bits;
}

static inline void sem_v_set(BiwaCpu* cpu, unsigned d, unsigned bytes, uint64_t bits) {
    for (unsigned i = 0; i < 16; ++i) {
        cpu->v[d][i] = i < bytes ? (uint8_t)(bits >> (8 * i)) : 0;
    }
}

/* Single (dbl false) and double precision as bit patterns. */
static inline uint64_t sem_fp_sign_bit(bool dbl) { return dbl ? (uint64_t)1 << 63 : (uint64_t)1 << 31; }
static inline uint64_t sem_fp_infinity(bool dbl) { return dbl ? 0x7FF0000000000000u : 0x7F800000u; }
static inline uint64_t sem_fp_quiet_bit(bool dbl) { return dbl ? (uint64_t)1 << 51 : (uint64_t)1 << 22; }
static inline uint64_t sem_fp_min_normal(bool dbl) { return dbl ? (uint64_t)1 << 52 : (uint64_t)1 << 23; }
static inline uint64_t sem_fp_magnitude(bool dbl, uint64_t x) { return x & ~sem_fp_sign_bit(dbl); }
static inline bool sem_fp_is_nan(bool dbl, uint64_t x) { return sem_fp_magnitude(dbl, x) > sem_fp_infinity(dbl); }

static inline bool sem_fp_is_snan(bool dbl, uint64_t x) {
    return sem_fp_is_nan(dbl, x) && (x & sem_fp_quiet_bit(dbl)) == 0;
}

static inline bool sem_fp_is_denormal(bool dbl, uint64_t x) {
    const uint64_t magnitude = sem_fp_magnitude(dbl, x);
    return magnitude != 0 && magnitude < sem_fp_min_normal(dbl);
}

/* An operand as the A57 reads it: with FPCR.FZ a denormal is a zero of the same sign, and FPSR.IDC
   records that. */
static inline uint64_t sem_fp_operand(BiwaCpu* cpu, bool dbl, uint64_t x) {
    if ((cpu->fpcr & SEM_FPCR_FZ) != 0 && sem_fp_is_denormal(dbl, x)) {
        cpu->fpsr |= SEM_FPSR_IDC;
        return x & sem_fp_sign_bit(dbl);
    }
    return x;
}

/* A non-NaN operand as a host double (exact for single precision too). */
static inline double sem_fp_value(bool dbl, uint64_t x) { return dbl ? sem_f64(x) : (double)sem_f32((uint32_t)x); }

/* FMOV (immediate): VFPExpandImm of imm8. */
static inline uint64_t sem_fp_expand_imm(bool dbl, uint32_t imm8) {
    const uint64_t sign = (imm8 >> 7) & 1;
    const uint64_t b6 = (imm8 >> 6) & 1;
    const uint64_t b54 = (imm8 >> 4) & 3;
    const uint64_t frac = imm8 & 15;
    if (dbl) {
        const uint64_t exponent = ((b6 ^ 1) << 10) | (b6 != 0 ? 0xFFu << 2 : 0) | b54;
        return (sign << 63) | (exponent << 52) | (frac << 48);
    }
    const uint64_t exponent = ((b6 ^ 1) << 7) | (b6 != 0 ? 0x1Fu << 2 : 0) | b54;
    return (sign << 31) | (exponent << 23) | (frac << 19);
}

/* FCMP, FCMPE and the compare of FCCMP / FCCMPE: NZCV for a compared with b (0110 equal, 1000 less,
   0010 greater, 0011 unordered). A signalling NaN raises IOC; with `signal` (the E forms) any NaN
   does. */
static inline uint32_t sem_fcmp(BiwaCpu* cpu, bool dbl, uint64_t a, uint64_t b, bool signal) {
    a = sem_fp_operand(cpu, dbl, a);
    b = sem_fp_operand(cpu, dbl, b);
    if (sem_fp_is_nan(dbl, a) || sem_fp_is_nan(dbl, b)) {
        if (signal || sem_fp_is_snan(dbl, a) || sem_fp_is_snan(dbl, b)) {
            cpu->fpsr |= SEM_FPSR_IOC;
        }
        return SEM_FLAG_C | SEM_FLAG_V;
    }
    const double x = sem_fp_value(dbl, a);
    const double y = sem_fp_value(dbl, b);
    if (x == y) {
        return SEM_FLAG_Z | SEM_FLAG_C;
    }
    return x < y ? SEM_FLAG_N : SEM_FLAG_C;
}

/* ---- Floating point: arithmetic ---- */

static inline uint64_t sem_fp_default_nan(bool dbl) { return dbl ? 0x7FF8000000000000u : 0x7FC00000u; }

static inline bool sem_fp_is_qnan(bool dbl, uint64_t x) {
    return sem_fp_is_nan(dbl, x) && (x & sem_fp_quiet_bit(dbl)) != 0;
}

static inline bool sem_fp_is_infinity(bool dbl, uint64_t x) { return sem_fp_magnitude(dbl, x) == sem_fp_infinity(dbl); }

static inline bool sem_fp_is_zero(bool dbl, uint64_t x) { return sem_fp_magnitude(dbl, x) == 0; }

/* A NaN operand's result: quieted (a signalling one raises IOC), or the default NaN with FPCR.DN. */
static inline uint64_t sem_fp_process_nan(BiwaCpu* cpu, bool dbl, uint64_t nan) {
    if (sem_fp_is_snan(dbl, nan)) {
        cpu->fpsr |= SEM_FPSR_IOC;
    }
    if ((cpu->fpcr & SEM_FPCR_DN) != 0) {
        return sem_fp_default_nan(dbl);
    }
    return nan | sem_fp_quiet_bit(dbl);
}

/* With NaN operands (at least one): the first signalling NaN, else the first quiet one. */
static inline uint64_t sem_fp_process_nans(BiwaCpu* cpu, bool dbl, uint64_t a, uint64_t b, uint64_t c, unsigned count) {
    const uint64_t operands[3] = {a, b, c};
    for (unsigned i = 0; i < count; ++i) {
        if (sem_fp_is_snan(dbl, operands[i])) {
            return sem_fp_process_nan(cpu, dbl, operands[i]);
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        if (sem_fp_is_nan(dbl, operands[i])) {
            return sem_fp_process_nan(cpu, dbl, operands[i]);
        }
    }
    return sem_fp_default_nan(dbl);
}

enum { SEM_FP_ADD, SEM_FP_SUB, SEM_FP_MUL, SEM_FP_DIV, SEM_FP_SQRT, SEM_FP_MULADD, SEM_FP_NARROW };

/* The host operation (MULADD: a * b + c, fused; NARROW: double a to single), rounded in the
   host's rounding mode and raising the host's flags. FENV_ACCESS keeps the compiler from folding it
   or moving it past changes of the rounding mode or flags. `dbl` is the result's precision. */
static inline uint64_t sem_fp_host_op(bool dbl, unsigned op, uint64_t a, uint64_t b, uint64_t c) {
#pragma STDC FENV_ACCESS ON
#pragma clang fp contract(off)
    if (op == SEM_FP_NARROW) {
        return sem_f32_bits((float)sem_f64(a));
    }
    if (dbl) {
        const double x = sem_f64(a);
        const double y = sem_f64(b);
        double r = 0;
        switch (op) {
            case SEM_FP_ADD:
                r = x + y;
                break;
            case SEM_FP_SUB:
                r = x - y;
                break;
            case SEM_FP_MUL:
                r = x * y;
                break;
            case SEM_FP_DIV:
                r = x / y;
                break;
            case SEM_FP_SQRT:
                r = __builtin_sqrt(x);
                break;
            default:
                r = __builtin_fma(x, y, sem_f64(c));
                break;
        }
        return sem_f64_bits(r);
    }
    const float x = sem_f32((uint32_t)a);
    const float y = sem_f32((uint32_t)b);
    float r = 0;
    switch (op) {
        case SEM_FP_ADD:
            r = x + y;
            break;
        case SEM_FP_SUB:
            r = x - y;
            break;
        case SEM_FP_MUL:
            r = x * y;
            break;
        case SEM_FP_DIV:
            r = x / y;
            break;
        case SEM_FP_SQRT:
            r = __builtin_sqrtf(x);
            break;
        default:
            r = __builtin_fmaf(x, y, sem_f32((uint32_t)c));
            break;
    }
    return sem_f32_bits(r);
}

/* The host's raw flags, when one operation's own flags must be told apart from earlier ones. */
#if defined(__x86_64__)
#define SEM_HOST_UNDERFLOW 0x10u
#define SEM_HOST_INEXACT 0x20u
static inline uint32_t sem_host_raw_flags(void) { return _mm_getcsr() & SEM_MXCSR_FLAGS; }
static inline void sem_host_set_raw_flags(uint32_t flags) { _mm_setcsr((_mm_getcsr() & ~SEM_MXCSR_FLAGS) | flags); }
#else
#define SEM_HOST_UNDERFLOW ((uint32_t)FE_UNDERFLOW)
#define SEM_HOST_INEXACT ((uint32_t)FE_INEXACT)
static inline uint32_t sem_host_raw_flags(void) { return (uint32_t)fetestexcept(FE_ALL_EXCEPT); }
static inline void sem_host_set_raw_flags(uint32_t flags) {
    feclearexcept(FE_ALL_EXCEPT);
    feraiseexcept((int)flags);
}
#endif

/* Whether a result the host rounded to the smallest normal magnitude was below it before rounding:
   ARM detects tininess before rounding, x86 after. Rounded towards zero, such a result stays
   below. */
static inline bool sem_fp_tiny_before_rounding(bool dbl, unsigned op, uint64_t a, uint64_t b, uint64_t c) {
#if defined(__x86_64__)
    const unsigned csr = _mm_getcsr();
    _mm_setcsr(csr | SEM_MXCSR_RC);
    const uint64_t truncated = sem_fp_host_op(dbl, op, a, b, c);
    _mm_setcsr((_mm_getcsr() & ~SEM_MXCSR_RC) | (csr & SEM_MXCSR_RC));
#else
    const int rounding = fegetround();
    fesetround(FE_TOWARDZERO);
    const uint64_t truncated = sem_fp_host_op(dbl, op, a, b, c);
    fesetround(rounding);
#endif
    return sem_fp_magnitude(dbl, truncated) < sem_fp_min_normal(dbl);
}

/* An operation on operands that are not NaNs (already read with FZ): the host computes it; an
   invalid operation's NaN becomes ARM's default NaN (x86's is negative); a result tiny before
   rounding raises UFC, and with FZ becomes a zero of its sign raising UFC alone (not IXC). */
static inline uint64_t sem_fp_arith(BiwaCpu* cpu, bool dbl, unsigned op, uint64_t a, uint64_t b, uint64_t c) {
    const bool flush = (cpu->fpcr & SEM_FPCR_FZ) != 0;
    uint32_t earlier = 0;
    if (flush) {
        earlier = sem_host_raw_flags();
        sem_host_set_raw_flags(0);
    }
    uint64_t r = sem_fp_host_op(dbl, op, a, b, c);
    if (sem_fp_is_nan(dbl, r)) {
        r = sem_fp_default_nan(dbl);
    } else {
        const uint64_t magnitude = sem_fp_magnitude(dbl, r);
        const uint64_t min_normal = sem_fp_min_normal(dbl);
        bool tiny = magnitude != 0 && magnitude < min_normal;
        if (magnitude == min_normal && sem_fp_tiny_before_rounding(dbl, op, a, b, c)) {
            tiny = true;
            cpu->fpsr |= SEM_FPSR_UFC;
        }
        if (flush && (sem_host_raw_flags() & SEM_HOST_UNDERFLOW) != 0) {
            tiny = true;  // rounded to zero (only this operation's flags are raised here)
        }
        if (flush && tiny) {
            r &= sem_fp_sign_bit(dbl);
            cpu->fpsr |= SEM_FPSR_UFC;
            sem_host_set_raw_flags(sem_host_raw_flags() & ~(SEM_HOST_UNDERFLOW | SEM_HOST_INEXACT));
        }
    }
    if (flush) {
        sem_host_set_raw_flags(earlier | sem_host_raw_flags());
    }
    return r;
}

/* FADD, FSUB, FMUL, FDIV. */
static inline uint64_t sem_fp_binary(BiwaCpu* cpu, bool dbl, unsigned op, uint64_t a, uint64_t b) {
    a = sem_fp_operand(cpu, dbl, a);
    b = sem_fp_operand(cpu, dbl, b);
    if (sem_fp_is_nan(dbl, a) || sem_fp_is_nan(dbl, b)) {
        return sem_fp_process_nans(cpu, dbl, a, b, 0, 2);
    }
    return sem_fp_arith(cpu, dbl, op, a, b, 0);
}

/* FSQRT. */
static inline uint64_t sem_fp_sqrt(BiwaCpu* cpu, bool dbl, uint64_t a) {
    a = sem_fp_operand(cpu, dbl, a);
    if (sem_fp_is_nan(dbl, a)) {
        return sem_fp_process_nan(cpu, dbl, a);
    }
    return sem_fp_arith(cpu, dbl, SEM_FP_SQRT, a, 0, 0);
}

/* FMADD and, with operands negated by the caller, FMSUB, FNMADD, FNMSUB: addend + a * b, fused.
   NaNs: the addend first; a quiet NaN addend with infinity times zero gives the default NaN and
   IOC. */
static inline uint64_t sem_fp_muladd(BiwaCpu* cpu, bool dbl, uint64_t addend, uint64_t a, uint64_t b) {
    addend = sem_fp_operand(cpu, dbl, addend);
    a = sem_fp_operand(cpu, dbl, a);
    b = sem_fp_operand(cpu, dbl, b);
    if (sem_fp_is_nan(dbl, addend) || sem_fp_is_nan(dbl, a) || sem_fp_is_nan(dbl, b)) {
        const bool infinity_times_zero = (sem_fp_is_infinity(dbl, a) && sem_fp_is_zero(dbl, b)) ||
                                         (sem_fp_is_zero(dbl, a) && sem_fp_is_infinity(dbl, b));
        if (sem_fp_is_qnan(dbl, addend) && infinity_times_zero) {
            cpu->fpsr |= SEM_FPSR_IOC;
            return sem_fp_default_nan(dbl);
        }
        return sem_fp_process_nans(cpu, dbl, addend, a, b, 3);
    }
    return sem_fp_arith(cpu, dbl, SEM_FP_MULADD, a, b, addend);
}

/* FMAX, FMIN, and with `number` FMAXNM, FMINNM (a quiet NaN against a number gives the number).
   Exact: no host arithmetic. Of two zeros, max is +0 unless both are -0, min -0 unless both are +0. */
static inline uint64_t sem_fp_max(BiwaCpu* cpu, bool dbl, uint64_t a, uint64_t b, bool max, bool number) {
    a = sem_fp_operand(cpu, dbl, a);
    b = sem_fp_operand(cpu, dbl, b);
    if (number) {
        const uint64_t loser = sem_fp_infinity(dbl) | (max ? sem_fp_sign_bit(dbl) : 0);
        const bool quiet_a = sem_fp_is_qnan(dbl, a);
        const bool quiet_b = sem_fp_is_qnan(dbl, b);
        if (quiet_a && !quiet_b) {
            a = loser;
        } else if (!quiet_a && quiet_b) {
            b = loser;
        }
    }
    if (sem_fp_is_nan(dbl, a) || sem_fp_is_nan(dbl, b)) {
        return sem_fp_process_nans(cpu, dbl, a, b, 0, 2);
    }
    if (sem_fp_is_zero(dbl, a) && sem_fp_is_zero(dbl, b)) {
        return max ? (a & b) : (a | b);
    }
    const double x = sem_fp_value(dbl, a);
    const double y = sem_fp_value(dbl, b);
    if (max) {
        return x > y ? a : b;
    }
    return x < y ? a : b;
}

/* ---- Floating point: conversions ---- */

/* Rounding: ARM's RMode values, then ties away from zero (FRINTA, FCVTA*). */
enum { SEM_ROUND_NEAREST, SEM_ROUND_UP, SEM_ROUND_DOWN, SEM_ROUND_ZERO, SEM_ROUND_AWAY };

static inline unsigned sem_fp_rounding(const BiwaCpu* cpu) { return (cpu->fpcr >> SEM_FPCR_RMODE_SHIFT) & 3; }

/* Where the bits dropped by a right shift lie against half of the last kept bit. */
enum { SEM_DROPPED_NONE, SEM_DROPPED_BELOW_HALF, SEM_DROPPED_HALF, SEM_DROPPED_ABOVE_HALF };

/* m >> shift (0 when shift >= 64), and where the dropped bits lie. */
static inline uint64_t sem_shift_right_rounding(uint64_t m, unsigned shift, unsigned* dropped) {
    if (shift == 0) {
        *dropped = SEM_DROPPED_NONE;
        return m;
    }
    if (shift >= 64) {
        *dropped = m == 0 ? SEM_DROPPED_NONE : SEM_DROPPED_BELOW_HALF; /* m < 2^53 <= half */
        return 0;
    }
    const uint64_t rest = m & (((uint64_t)1 << shift) - 1);
    const uint64_t half = (uint64_t)1 << (shift - 1);
    *dropped = rest == 0      ? SEM_DROPPED_NONE
               : rest < half  ? SEM_DROPPED_BELOW_HALF
               : rest == half ? SEM_DROPPED_HALF
                              : SEM_DROPPED_ABOVE_HALF;
    return m >> shift;
}

/* Whether a magnitude rounds up by one, given its last kept bit and the dropped bits. */
static inline bool sem_round_up(unsigned rounding, bool negative, bool odd, unsigned dropped) {
    switch (rounding) {
        case SEM_ROUND_NEAREST:
            return dropped == SEM_DROPPED_ABOVE_HALF || (dropped == SEM_DROPPED_HALF && odd);
        case SEM_ROUND_AWAY:
            return dropped >= SEM_DROPPED_HALF;
        case SEM_ROUND_UP:
            return !negative && dropped != SEM_DROPPED_NONE;
        case SEM_ROUND_DOWN:
            return negative && dropped != SEM_DROPPED_NONE;
        default:
            return false;
    }
}

/* A finite nonzero value as m * 2^e, m an integer below 2^53 (2^24 for single precision). */
static inline void sem_fp_unpack(bool dbl, uint64_t x, uint64_t* m, int* e) {
    if (dbl) {
        const int exponent = (int)((x >> 52) & 0x7FF);
        const uint64_t frac = x & 0xFFFFFFFFFFFFFu;
        *m = exponent == 0 ? frac : frac | ((uint64_t)1 << 52);
        *e = (exponent == 0 ? 1 : exponent) - 1075;
    } else {
        const int exponent = (int)((x >> 23) & 0xFF);
        const uint64_t frac = x & 0x7FFFFFu;
        *m = exponent == 0 ? frac : frac | ((uint64_t)1 << 23);
        *e = (exponent == 0 ? 1 : exponent) - 150;
    }
}

/* FCVT{N,A,P,M,Z}{S,U} (scalar) and FCVTZS / FCVTZU (fixed-point): x * 2^fbits rounded, then
   saturated to `bits` (32 or 64) signed or unsigned. A NaN or an out-of-range value raises IOC
   (a NaN gives 0), an inexact one IXC. Exact arithmetic: no host flags. */
static inline uint64_t sem_fp_to_int(BiwaCpu* cpu, bool dbl, uint64_t x, unsigned fbits, unsigned rounding,
                                     unsigned bits, bool is_unsigned) {
    x = sem_fp_operand(cpu, dbl, x);
    if (sem_fp_is_nan(dbl, x)) {
        cpu->fpsr |= SEM_FPSR_IOC;
        return 0;
    }
    const bool negative = (x & sem_fp_sign_bit(dbl)) != 0;
    uint64_t magnitude = 0;
    bool overflow = sem_fp_is_infinity(dbl, x);
    bool inexact = false;
    if (!overflow && !sem_fp_is_zero(dbl, x)) {
        uint64_t m = 0;
        int e = 0;
        sem_fp_unpack(dbl, x, &m, &e);
        e += (int)fbits;
        if (e >= 0) {
            overflow = e >= 64 || (m >> (63 - e)) > 1; /* m * 2^e needs more than 64 bits */
            magnitude = overflow ? 0 : m << e;
        } else {
            unsigned dropped = SEM_DROPPED_NONE;
            magnitude = sem_shift_right_rounding(m, (unsigned)-e, &dropped);
            if (sem_round_up(rounding, negative, (magnitude & 1) != 0, dropped)) {
                ++magnitude;
            }
            inexact = dropped != SEM_DROPPED_NONE;
        }
    }
    const uint64_t top = (uint64_t)1 << (bits - 1);
    if (!overflow) {
        if (is_unsigned) {
            overflow = (negative && magnitude != 0) || (bits == 32 && magnitude > 0xFFFFFFFFu);
        } else {
            overflow = magnitude > (negative ? top : top - 1);
        }
    }
    const uint64_t mask = bits == 64 ? ~(uint64_t)0 : 0xFFFFFFFFu;
    if (overflow) {
        cpu->fpsr |= SEM_FPSR_IOC;
        if (is_unsigned) {
            return negative ? 0 : mask;
        }
        return (negative ? top : top - 1) & mask;
    }
    if (inexact) {
        cpu->fpsr |= SEM_FPSR_IXC;
    }
    return (negative ? (uint64_t)0 - magnitude : magnitude) & mask;
}

/* SCVTF / UCVTF (scalar, integer and fixed-point): the integer in the low `bits` of `value`,
   divided by 2^fbits, rounded once in the host's rounding mode (the scaling by a power of two is
   exact: the result is never tiny). */
static inline uint64_t sem_int_to_fp(bool dbl, uint64_t value, unsigned bits, bool is_unsigned, unsigned fbits) {
#pragma STDC FENV_ACCESS ON
#pragma clang fp contract(off)
    const uint64_t scale_bits = (uint64_t)(1023 - fbits) << 52;
    if (dbl) {
        double r = 0;
        if (is_unsigned) {
            r = bits == 32 ? (double)(uint32_t)value : (double)value;
        } else {
            r = bits == 32 ? (double)(int32_t)(uint32_t)value : (double)(int64_t)value;
        }
        return sem_f64_bits(r * sem_f64(scale_bits));
    }
    float r = 0;
    if (is_unsigned) {
        r = bits == 32 ? (float)(uint32_t)value : (float)value;
    } else {
        r = bits == 32 ? (float)(int32_t)(uint32_t)value : (float)(int64_t)value;
    }
    return sem_f32_bits(r * (float)sem_f64(scale_bits));
}

/* FRINTN, FRINTP, FRINTM, FRINTZ, FRINTA (`rounding`), FRINTI and FRINTX (FPCR's rounding;
   `exact` for FRINTX raises IXC when the value changes): the nearest integral value. A result of
   zero keeps the operand's sign. Exact arithmetic: no host flags. */
static inline uint64_t sem_fp_round_int(BiwaCpu* cpu, bool dbl, uint64_t x, unsigned rounding, bool exact) {
    x = sem_fp_operand(cpu, dbl, x);
    if (sem_fp_is_nan(dbl, x)) {
        return sem_fp_process_nan(cpu, dbl, x);
    }
    const uint64_t sign = x & sem_fp_sign_bit(dbl);
    if (sem_fp_is_infinity(dbl, x) || sem_fp_is_zero(dbl, x)) {
        return x;
    }
    uint64_t m = 0;
    int e = 0;
    sem_fp_unpack(dbl, x, &m, &e);
    if (e >= 0) {
        return x; /* already integral */
    }
    unsigned dropped = SEM_DROPPED_NONE;
    uint64_t n = sem_shift_right_rounding(m, (unsigned)-e, &dropped);
    if (sem_round_up(rounding, sign != 0, (n & 1) != 0, dropped)) {
        ++n;
    }
    if (exact && dropped != SEM_DROPPED_NONE) {
        cpu->fpsr |= SEM_FPSR_IXC;
    }
    if (n == 0) {
        return sign;
    }
    /* n < 2^53: exact as a double, and as a single when it came from one. */
    return (dbl ? sem_f64_bits((double)n) : sem_f32_bits((float)n)) | sign;
}

/* FCVT between single and double precision. Widening is exact; narrowing rounds like arithmetic.
   NaNs keep the top bits of their payload. */
static inline uint64_t sem_fp_convert(BiwaCpu* cpu, bool from_dbl, uint64_t x) {
    x = sem_fp_operand(cpu, from_dbl, x);
    const bool to_dbl = !from_dbl;
    if (sem_fp_is_nan(from_dbl, x)) {
        if (sem_fp_is_snan(from_dbl, x)) {
            cpu->fpsr |= SEM_FPSR_IOC;
        }
        if ((cpu->fpcr & SEM_FPCR_DN) != 0) {
            return sem_fp_default_nan(to_dbl);
        }
        const uint64_t sign = (x & sem_fp_sign_bit(from_dbl)) != 0 ? sem_fp_sign_bit(to_dbl) : 0;
        const uint64_t payload = from_dbl ? (x >> 29) & 0x3FFFFFu : (x & 0x3FFFFFu) << 29;
        return sign | sem_fp_infinity(to_dbl) | sem_fp_quiet_bit(to_dbl) | payload;
    }
    if (to_dbl) {
        return sem_f64_bits((double)sem_f32((uint32_t)x));
    }
    return sem_fp_arith(cpu, false, SEM_FP_NARROW, x, 0, 0);
}

/* FCVT to half precision: IEEE, or with FPCR.AHP the alternative format (no infinities or NaNs:
   those raise IOC and give the largest value or zero; out-of-range values saturate with IOC).
   Rounded in software; FZ does not apply to half precision on ARMv8.0. */
static inline uint64_t sem_fp_to_half(BiwaCpu* cpu, bool dbl, uint64_t x) {
    x = sem_fp_operand(cpu, dbl, x);
    const bool ahp = (cpu->fpcr & SEM_FPCR_AHP) != 0;
    const bool negative = (x & sem_fp_sign_bit(dbl)) != 0;
    const uint64_t sign = negative ? 0x8000u : 0;
    if (sem_fp_is_nan(dbl, x)) {
        if (ahp || sem_fp_is_snan(dbl, x)) {
            cpu->fpsr |= SEM_FPSR_IOC;
        }
        if (ahp) {
            return sign;
        }
        if ((cpu->fpcr & SEM_FPCR_DN) != 0) {
            return 0x7E00u;
        }
        return sign | 0x7E00u | (dbl ? (x >> 42) & 0x1FFu : (x >> 13) & 0x1FFu);
    }
    if (sem_fp_is_infinity(dbl, x)) {
        if (ahp) {
            cpu->fpsr |= SEM_FPSR_IOC;
            return sign | 0x7FFFu;
        }
        return sign | 0x7C00u;
    }
    if (sem_fp_is_zero(dbl, x)) {
        return sign;
    }
    uint64_t m = 0;
    int e = 0;
    sem_fp_unpack(dbl, x, &m, &e);
    const int top = 63 - __builtin_clzll(m) + e; /* the value lies in [2^top, 2^(top + 1)) */
    const bool tiny = top < -14;
    int quantum = tiny ? -24 : top - 10;
    unsigned dropped = SEM_DROPPED_NONE;
    uint64_t n = 0;
    if (quantum <= e) {
        n = m << (e - quantum);
    } else {
        n = sem_shift_right_rounding(m, (unsigned)(quantum - e), &dropped);
    }
    if (sem_round_up(sem_fp_rounding(cpu), negative, (n & 1) != 0, dropped)) {
        ++n;
    }
    const bool inexact = dropped != SEM_DROPPED_NONE;
    uint64_t bits = 0;
    if (tiny) {
        bits = n; /* a denormal, or the smallest normal when it rounded up to 2^10 */
    } else {
        if (n == (uint64_t)1 << 11) {
            n >>= 1;
            ++quantum;
        }
        const int biased = quantum + 25;
        if (biased > (ahp ? 31 : 30)) {
            if (ahp) {
                cpu->fpsr |= SEM_FPSR_IOC;
                return sign | 0x7FFFu;
            }
            cpu->fpsr |= SEM_FPSR_OFC | SEM_FPSR_IXC;
            const unsigned rounding = sem_fp_rounding(cpu);
            const bool to_infinity = rounding == SEM_ROUND_NEAREST || (rounding == SEM_ROUND_UP && !negative) ||
                                     (rounding == SEM_ROUND_DOWN && negative);
            return sign | (to_infinity ? 0x7C00u : 0x7BFFu);
        }
        bits = ((uint64_t)biased << 10) | (n & 0x3FFu);
    }
    if (inexact) {
        cpu->fpsr |= tiny ? SEM_FPSR_UFC | SEM_FPSR_IXC : SEM_FPSR_IXC;
    }
    return sign | bits;
}

/* FCVT from half precision (IEEE, or the alternative format with FPCR.AHP): exact. */
static inline uint64_t sem_fp_from_half(BiwaCpu* cpu, bool dbl, uint64_t h) {
    const bool negative = (h & 0x8000u) != 0;
    const uint64_t sign = negative ? sem_fp_sign_bit(dbl) : 0;
    const unsigned exponent = (unsigned)(h >> 10) & 31;
    const uint64_t frac = h & 0x3FFu;
    if (exponent == 31 && (cpu->fpcr & SEM_FPCR_AHP) == 0) {
        if (frac == 0) {
            return sign | sem_fp_infinity(dbl);
        }
        if ((frac & 0x200u) == 0) {
            cpu->fpsr |= SEM_FPSR_IOC;
        }
        if ((cpu->fpcr & SEM_FPCR_DN) != 0) {
            return sem_fp_default_nan(dbl);
        }
        const uint64_t payload = (frac & 0x1FFu) << (dbl ? 42 : 13);
        return sign | sem_fp_infinity(dbl) | sem_fp_quiet_bit(dbl) | payload;
    }
    if (exponent == 0 && frac == 0) {
        return sign;
    }
    /* m * 2^e with m < 2^11: build the double directly (always normal). */
    const uint64_t m = exponent == 0 ? frac : frac | 0x400u;
    const int e = (exponent == 0 ? 1 : (int)exponent) - 25;
    const int top = 63 - __builtin_clzll(m);
    const uint64_t mantissa = (m << (52 - top)) & 0xFFFFFFFFFFFFFu;
    const uint64_t as_double = ((uint64_t)(top + e + 1023) << 52) | mantissa;
    if (dbl) {
        return sign | as_double;
    }
    return (negative ? sem_fp_sign_bit(false) : 0) | sem_f32_bits((float)sem_f64(as_double));
}

/* ---- Advanced SIMD: registers and lanes ---- */

/* A copy of a vector register: sources are copied first, so the destination may be one of them. */
typedef struct SemVec {
    uint8_t b[16];
} SemVec;

static inline SemVec sem_vread(const BiwaCpu* cpu, unsigned n) {
    SemVec v;
    for (unsigned i = 0; i < 16; ++i) {
        v.b[i] = cpu->v[n][i];
    }
    return v;
}

/* Writes Vd: all 128 bits, or with q false the low 64 and zeros above. */
static inline void sem_vwrite(BiwaCpu* cpu, unsigned d, const SemVec* v, bool q) {
    for (unsigned i = 0; i < 16; ++i) {
        cpu->v[d][i] = (q || i < 8) ? v->b[i] : 0;
    }
}

/* Lane i of `esize` bytes (1, 2, 4 or 8), zero-extended. */
static inline uint64_t sem_lane(const SemVec* v, unsigned i, unsigned esize) {
    uint64_t x = 0;
    for (unsigned k = 0; k < esize; ++k) {
        x |= (uint64_t)v->b[i * esize + k] << (8 * k);
    }
    return x;
}

static inline void sem_set_lane(SemVec* v, unsigned i, unsigned esize, uint64_t x) {
    for (unsigned k = 0; k < esize; ++k) {
        v->b[i * esize + k] = (uint8_t)(x >> (8 * k));
    }
}

/* ---- Advanced SIMD: copies, permutes, immediates, bitwise ---- */

/* DUP (element): lane `index` of Vn in every lane of Vd, or (scalar) in the low lane alone. */
static inline void sem_dup_element(BiwaCpu* cpu, bool q, bool scalar, unsigned esize, unsigned d, unsigned n,
                                   unsigned index) {
    const SemVec src = sem_vread(cpu, n);
    const uint64_t x = sem_lane(&src, index, esize);
    SemVec r = {{0}};
    const unsigned lanes = scalar ? 1 : (q ? 16 : 8) / esize;
    for (unsigned i = 0; i < lanes; ++i) {
        sem_set_lane(&r, i, esize, x);
    }
    sem_vwrite(cpu, d, &r, q && !scalar);
}

/* DUP (general): the low esize bytes of Rn in every lane. */
static inline void sem_dup_general(BiwaCpu* cpu, bool q, unsigned esize, unsigned d, unsigned n) {
    const uint64_t x = sem_x(cpu, n);
    SemVec r = {{0}};
    for (unsigned i = 0; i < (q ? 16 : 8) / esize; ++i) {
        sem_set_lane(&r, i, esize, x);
    }
    sem_vwrite(cpu, d, &r, q);
}

/* UMOV and SMOV (`sign`: sign-extended to 32 or 64 bits as `to_x` says). */
static inline void sem_mov_to_general(BiwaCpu* cpu, bool sign, bool to_x, unsigned esize, unsigned d, unsigned n,
                                      unsigned index) {
    const SemVec src = sem_vread(cpu, n);
    uint64_t x = sem_lane(&src, index, esize);
    if (sign) {
        x = sem_sign_extend(x, 8 * esize);
    }
    sem_set_x(cpu, d, to_x ? x : (uint32_t)x);
}

/* INS (general) and INS (element): one lane of Vd replaced, the rest kept. */
static inline void sem_ins(BiwaCpu* cpu, unsigned esize, unsigned d, unsigned index, uint64_t x) {
    SemVec r = sem_vread(cpu, d);
    sem_set_lane(&r, index, esize, x);
    sem_vwrite(cpu, d, &r, true);
}

/* EXT: bytes `index` onwards of Vm:Vn (Vn low), 8 or 16 of them. */
static inline void sem_ext(BiwaCpu* cpu, bool q, unsigned d, unsigned n, unsigned m, unsigned index) {
    const SemVec lo = sem_vread(cpu, n);
    const SemVec hi = sem_vread(cpu, m);
    const unsigned bytes = q ? 16 : 8;
    SemVec r = {{0}};
    for (unsigned i = 0; i < bytes; ++i) {
        const unsigned k = index + i;
        r.b[i] = k < bytes ? lo.b[k] : hi.b[k - bytes];
    }
    sem_vwrite(cpu, d, &r, q);
}

/* TBL and TBX: bytes of the table Vn.. (`regs` registers, wrapping at 31) picked by the bytes of Vm;
   an index past the table gives 0 (TBL) or keeps Vd's byte (TBX). */
static inline void sem_tbl(BiwaCpu* cpu, bool q, bool tbx, unsigned d, unsigned n, unsigned regs, unsigned m) {
    uint8_t table[64];
    for (unsigned r = 0; r < regs; ++r) {
        for (unsigned i = 0; i < 16; ++i) {
            table[r * 16 + i] = cpu->v[(n + r) % 32][i];
        }
    }
    const SemVec indices = sem_vread(cpu, m);
    SemVec r = sem_vread(cpu, d);
    for (unsigned i = 0; i < (q ? 16u : 8u); ++i) {
        const unsigned k = indices.b[i];
        if (k < regs * 16) {
            r.b[i] = table[k];
        } else if (!tbx) {
            r.b[i] = 0;
        }
    }
    sem_vwrite(cpu, d, &r, q);
}

enum { SEM_UZP1, SEM_UZP2, SEM_TRN1, SEM_TRN2, SEM_ZIP1, SEM_ZIP2 };

/* UZP1/2, TRN1/2, ZIP1/2. */
static inline void sem_permute(BiwaCpu* cpu, unsigned op, bool q, unsigned esize, unsigned d, unsigned n, unsigned m) {
    const SemVec a = sem_vread(cpu, n);
    const SemVec b = sem_vread(cpu, m);
    const unsigned lanes = (q ? 16 : 8) / esize;
    const unsigned part = op == SEM_UZP2 || op == SEM_TRN2 || op == SEM_ZIP2 ? 1 : 0;
    SemVec r = {{0}};
    for (unsigned i = 0; i < lanes; ++i) {
        uint64_t x = 0;
        switch (op) {
            case SEM_UZP1:
            case SEM_UZP2: {
                const unsigned k = 2 * i + part; /* of the concatenation Vm:Vn */
                x = k < lanes ? sem_lane(&a, k, esize) : sem_lane(&b, k - lanes, esize);
                break;
            }
            case SEM_TRN1:
            case SEM_TRN2:
                x = (i & 1) == 0 ? sem_lane(&a, (i & ~1u) + part, esize) : sem_lane(&b, (i & ~1u) + part, esize);
                break;
            default: {
                const unsigned k = part * lanes / 2 + i / 2;
                x = (i & 1) == 0 ? sem_lane(&a, k, esize) : sem_lane(&b, k, esize);
                break;
            }
        }
        sem_set_lane(&r, i, esize, x);
    }
    sem_vwrite(cpu, d, &r, q);
}

/* AdvSIMDExpandImm: the 64-bit pattern of MOVI, MVNI, ORR, BIC and FMOV (vector, immediate). */
static inline uint64_t sem_simd_expand_imm(unsigned op, unsigned cmode, unsigned imm8) {
    const uint64_t i = imm8;
    switch (cmode >> 1) {
        case 0:
            return sem_replicate(i, 32);
        case 1:
            return sem_replicate(i << 8, 32);
        case 2:
            return sem_replicate(i << 16, 32);
        case 3:
            return sem_replicate(i << 24, 32);
        case 4:
            return sem_replicate(i, 16);
        case 5:
            return sem_replicate(i << 8, 16);
        case 6:
            return sem_replicate((cmode & 1) == 0 ? (i << 8) | 0xFF : (i << 16) | 0xFFFF, 32);
        default:
            break;
    }
    if ((cmode & 1) == 0) {
        if (op == 0) {
            return sem_replicate(i, 8);
        }
        uint64_t bytes = 0;
        for (unsigned k = 0; k < 8; ++k) {
            bytes |= ((i >> k) & 1) != 0 ? (uint64_t)0xFF << (8 * k) : 0;
        }
        return bytes;
    }
    if (op == 0) {
        return sem_replicate(sem_fp_expand_imm(false, imm8), 32);
    }
    return sem_fp_expand_imm(true, imm8);
}

/* MOVI, MVNI, ORR and BIC (vector, immediate), FMOV (vector, immediate): by cmode and op. */
static inline void sem_movi(BiwaCpu* cpu, bool q, unsigned op, unsigned cmode, unsigned imm8, unsigned d) {
    const uint64_t imm = sem_simd_expand_imm(op, cmode, imm8);
    /* ORR / BIC for the shifted 32- and 16-bit forms with cmode<0> set; MVNI where op is set. */
    const bool logical = (cmode & 1) != 0 && cmode < 12;
    SemVec r = sem_vread(cpu, d);
    for (unsigned half = 0; half < 2; ++half) {
        uint64_t x = sem_lane(&r, half, 8);
        if (logical) {
            x = op == 0 ? x | imm : x & ~imm;
        } else if (op != 0 && cmode < 14) {
            x = ~imm;
        } else {
            x = imm;
        }
        sem_set_lane(&r, half, 8, x);
    }
    sem_vwrite(cpu, d, &r, q);
}

enum { SEM_VAND, SEM_VBIC, SEM_VORR, SEM_VORN, SEM_VEOR, SEM_VBSL, SEM_VBIT, SEM_VBIF };

/* AND, BIC, ORR, ORN, EOR, BSL, BIT, BIF (vector, register). */
static inline void sem_vlogic(BiwaCpu* cpu, unsigned op, bool q, unsigned d, unsigned n, unsigned m) {
    const SemVec a = sem_vread(cpu, n);
    const SemVec b = sem_vread(cpu, m);
    SemVec r = sem_vread(cpu, d);
    for (unsigned half = 0; half < 2; ++half) {
        const uint64_t x = sem_lane(&a, half, 8);
        const uint64_t y = sem_lane(&b, half, 8);
        const uint64_t z = sem_lane(&r, half, 8);
        uint64_t out = 0;
        switch (op) {
            case SEM_VAND:
                out = x & y;
                break;
            case SEM_VBIC:
                out = x & ~y;
                break;
            case SEM_VORR:
                out = x | y;
                break;
            case SEM_VORN:
                out = x | ~y;
                break;
            case SEM_VEOR:
                out = x ^ y;
                break;
            case SEM_VBSL:
                out = (z & x) | (~z & y);
                break;
            case SEM_VBIT:
                out = (y & x) | (~y & z);
                break;
            default:
                out = (~y & x) | (y & z);
                break;
        }
        sem_set_lane(&r, half, 8, out);
    }
    sem_vwrite(cpu, d, &r, q);
}

enum { SEM_VNOT, SEM_VRBIT, SEM_VCNT, SEM_VCLS, SEM_VCLZ };

/* NOT, RBIT, CNT (on bytes), CLS and CLZ (on lanes of esize bytes). */
static inline void sem_vbits(BiwaCpu* cpu, unsigned op, bool q, unsigned esize, unsigned d, unsigned n) {
    const SemVec a = sem_vread(cpu, n);
    SemVec r = {{0}};
    const unsigned width = 8 * esize;
    for (unsigned i = 0; i < 16 / esize; ++i) {
        const uint64_t x = sem_lane(&a, i, esize);
        uint64_t out = 0;
        switch (op) {
            case SEM_VNOT:
                out = ~x;
                break;
            case SEM_VRBIT:
                for (unsigned k = 0; k < width; ++k) {
                    out |= ((x >> k) & 1) << (width - 1 - k);
                }
                break;
            case SEM_VCNT:
                out = (uint64_t)__builtin_popcountll(x);
                break;
            default: {
                /* CLZ of x (CLS: of x ^ (x >> 1) below the sign bit, less one). */
                uint64_t y = op == SEM_VCLS ? (x ^ (x >> 1)) & (((uint64_t)1 << (width - 1)) - 1) : x;
                unsigned count = width;
                if (y != 0) {
                    count = (unsigned)__builtin_clzll(y) - (64 - width);
                }
                out = op == SEM_VCLS ? count - 1 : count;
                break;
            }
        }
        sem_set_lane(&r, i, esize, out);
    }
    sem_vwrite(cpu, d, &r, q);
}

/* REV16, REV32, REV64 (vector): the esize-byte lanes reversed within each container. */
static inline void sem_vrev(BiwaCpu* cpu, bool q, unsigned esize, unsigned container, unsigned d, unsigned n) {
    const SemVec a = sem_vread(cpu, n);
    SemVec r = {{0}};
    const unsigned per = container / esize;
    for (unsigned i = 0; i < 16 / esize; ++i) {
        const unsigned base = i - i % per;
        sem_set_lane(&r, i, esize, sem_lane(&a, base + (per - 1 - i % per), esize));
    }
    sem_vwrite(cpu, d, &r, q);
}

/* ---- Advanced SIMD: integer arithmetic ---- */

#define SEM_FPSR_QC (1u << 27)

static inline uint64_t sem_lane_mask(unsigned bits) { return bits >= 64 ? ~(uint64_t)0 : ((uint64_t)1 << bits) - 1; }

/* A lane's value as a wide integer: zero-extended (`is_unsigned`) or sign-extended. */
static inline __int128 sem_lane_value(uint64_t x, unsigned bits, bool is_unsigned) {
    if (bits >= 64) {
        return is_unsigned ? (__int128)x : (__int128)(int64_t)x;
    }
    return is_unsigned ? (__int128)(x & sem_lane_mask(bits)) : (__int128)(int64_t)sem_sign_extend(x, bits);
}

/* `v` saturated to a lane of `bits`, signed or unsigned; FPSR.QC records a saturation. */
static inline uint64_t sem_saturate(BiwaCpu* cpu, __int128 v, unsigned bits, bool is_unsigned) {
    const __int128 hi = is_unsigned ? ((__int128)1 << bits) - 1 : ((__int128)1 << (bits - 1)) - 1;
    const __int128 lo = is_unsigned ? 0 : -((__int128)1 << (bits - 1));
    if (v > hi) {
        cpu->fpsr |= SEM_FPSR_QC;
        v = hi;
    } else if (v < lo) {
        cpu->fpsr |= SEM_FPSR_QC;
        v = lo;
    }
    return (uint64_t)v & sem_lane_mask(bits);
}

/* SSHL, USHL, SRSHL, URSHL, SQSHL, UQSHL, SQRSHL, UQRSHL (register): `a` shifted left by `shift`
   (the low byte of the other operand, signed: negative shifts right), with rounding and saturation
   as asked. */
static inline uint64_t sem_shift_lane(BiwaCpu* cpu, bool is_unsigned, bool rounding, bool saturating, unsigned bits,
                                      uint64_t a, int shift) {
    const __int128 x = sem_lane_value(a, bits, is_unsigned);
    const uint64_t mask = sem_lane_mask(bits);
    if (shift >= 0) {
        if (x == 0) {
            return 0;
        }
        if (!saturating) {
            return shift >= (int)bits ? 0 : (uint64_t)((unsigned __int128)x << shift) & mask;
        }
        if (shift >= (int)bits) {
            return sem_saturate(cpu, x < 0 ? -((__int128)1 << 100) : ((__int128)1 << 100), bits, is_unsigned);
        }
        return sem_saturate(cpu, x * ((__int128)1 << shift), bits, is_unsigned);
    }
    const int right = -shift > 100 ? 100 : -shift; /* past 65 the result no longer changes */
    const __int128 v = (x + (rounding ? (__int128)1 << (right - 1) : 0)) >> right;
    return saturating ? sem_saturate(cpu, v, bits, is_unsigned) : (uint64_t)v & mask;
}

/* PMUL: the low byte of the carry-less product of two bytes. */
static inline uint64_t sem_pmul8(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (unsigned k = 0; k < 8; ++k) {
        if (((b >> k) & 1) != 0) {
            r ^= a << k;
        }
    }
    return r & 0xFF;
}

/* One lane of the integer three-same forms; `opcode` is the instruction's bits 15..11 and `u` its
   bit 29 (unsigned, or the second form of the pair). `acc` is Vd's lane (ABA, MLA, MLS). */
static inline uint64_t sem_int3_lane(BiwaCpu* cpu, bool u, unsigned opcode, unsigned bits, uint64_t a, uint64_t b,
                                     uint64_t acc) {
    const uint64_t mask = sem_lane_mask(bits);
    const __int128 x = sem_lane_value(a, bits, u);
    const __int128 y = sem_lane_value(b, bits, u);
    switch (opcode) {
        case 0x00: /* SHADD, UHADD */
            return (uint64_t)((x + y) >> 1) & mask;
        case 0x01: /* SQADD, UQADD */
            return sem_saturate(cpu, x + y, bits, u);
        case 0x02: /* SRHADD, URHADD */
            return (uint64_t)((x + y + 1) >> 1) & mask;
        case 0x04: /* SHSUB, UHSUB */
            return (uint64_t)((x - y) >> 1) & mask;
        case 0x05: /* SQSUB, UQSUB */
            return sem_saturate(cpu, x - y, bits, u);
        case 0x06: /* CMGT, CMHI */
            return x > y ? mask : 0;
        case 0x07: /* CMGE, CMHS */
            return x >= y ? mask : 0;
        case 0x08: /* SSHL, USHL */
        case 0x09: /* SQSHL, UQSHL */
        case 0x0A: /* SRSHL, URSHL */
        case 0x0B: /* SQRSHL, UQRSHL */
            return sem_shift_lane(cpu, u, (opcode & 2) != 0, (opcode & 1) != 0, bits, a, (int8_t)(uint8_t)b);
        case 0x0C: /* SMAX, UMAX */
            return (x > y ? a : b) & mask;
        case 0x0D: /* SMIN, UMIN */
            return (x < y ? a : b) & mask;
        case 0x0E: /* SABD, UABD */
            return (uint64_t)(x > y ? x - y : y - x) & mask;
        case 0x0F: /* SABA, UABA */
            return (acc + (uint64_t)(x > y ? x - y : y - x)) & mask;
        case 0x10: /* ADD, SUB */
            return (u ? a - b : a + b) & mask;
        case 0x11: /* CMTST, CMEQ */
            return (u ? (a & mask) == (b & mask) : (a & b & mask) != 0) ? mask : 0;
        case 0x12: /* MLA, MLS */
            return (u ? acc - a * b : acc + a * b) & mask;
        case 0x13: /* MUL, PMUL */
            return u ? sem_pmul8(a, b) : (a * b) & mask;
        default: { /* 0x16: SQDMULH, SQRDMULH */
            const __int128 sx = sem_lane_value(a, bits, false);
            const __int128 sy = sem_lane_value(b, bits, false);
            const __int128 product = 2 * sx * sy + (u ? (__int128)1 << (bits - 1) : 0);
            return sem_saturate(cpu, product >> bits, bits, false);
        }
    }
}

/* The integer three-same forms, vector or scalar (one lane, the rest of Vd cleared). The pairwise
   ones (SMAXP, SMINP, UMAXP, UMINP, ADDP: opcodes 0x14, 0x15, 0x17) take adjacent lanes of Vm:Vn. */
static inline void sem_simd_int3_v(BiwaCpu* cpu, bool u, unsigned opcode, bool q, bool scalar, unsigned esize,
                                   unsigned d, SemVec a, SemVec b) {
    const SemVec acc = sem_vread(cpu, d);
    const unsigned bits = 8 * esize;
    const unsigned lanes = scalar ? 1 : (q ? 16 : 8) / esize;
    SemVec r = {{0}};
    for (unsigned i = 0; i < lanes; ++i) {
        uint64_t out = 0;
        if (opcode == 0x14 || opcode == 0x15 || opcode == 0x17) {
            const SemVec* src = 2 * i < lanes ? &a : &b;
            const unsigned k = (2 * i) % lanes;
            const uint64_t x = sem_lane(src, k, esize);
            const uint64_t y = sem_lane(src, k + 1, esize);
            /* MAXP / MINP as MAX / MIN, ADDP as ADD */
            out = sem_int3_lane(cpu, opcode == 0x17 ? false : u,
                                opcode == 0x14   ? 0x0C
                                : opcode == 0x15 ? 0x0D
                                                 : 0x10,
                                bits, x, y, 0);
        } else {
            out = sem_int3_lane(cpu, u, opcode, bits, sem_lane(&a, i, esize), sem_lane(&b, i, esize),
                                sem_lane(&acc, i, esize));
        }
        sem_set_lane(&r, i, esize, out);
    }
    sem_vwrite(cpu, d, &r, q && !scalar);
}

static inline void sem_simd_int3(BiwaCpu* cpu, bool u, unsigned opcode, bool q, bool scalar, unsigned esize, unsigned d,
                                 unsigned n, unsigned m) {
    sem_simd_int3_v(cpu, u, opcode, q, scalar, esize, d, sem_vread(cpu, n), sem_vread(cpu, m));
}

/* The integer two-register forms on lanes of one size, vector or scalar; `opcode` is bits 16..12:
   SUQADD / USQADD (0x03), SQABS / SQNEG (0x07), CMGT / CMGE (0x08), CMEQ / CMLE (0x09), CMLT (0x0A)
   against zero, ABS / NEG (0x0B). */
static inline void sem_simd_int2(BiwaCpu* cpu, bool u, unsigned opcode, bool q, bool scalar, unsigned esize, unsigned d,
                                 unsigned n) {
    const SemVec a = sem_vread(cpu, n);
    const SemVec acc = sem_vread(cpu, d);
    const unsigned bits = 8 * esize;
    const uint64_t mask = sem_lane_mask(bits);
    const unsigned lanes = scalar ? 1 : (q ? 16 : 8) / esize;
    SemVec r = {{0}};
    for (unsigned i = 0; i < lanes; ++i) {
        const uint64_t raw = sem_lane(&a, i, esize);
        const __int128 x = sem_lane_value(raw, bits, false);
        uint64_t out = 0;
        switch (opcode) {
            case 0x03: {
                const uint64_t old = sem_lane(&acc, i, esize);
                out = u ? sem_saturate(cpu, sem_lane_value(old, bits, true) + x, bits, true)
                        : sem_saturate(cpu, sem_lane_value(old, bits, false) + sem_lane_value(raw, bits, true), bits,
                                       false);
                break;
            }
            case 0x07:
                out = sem_saturate(cpu, u ? -x : (x < 0 ? -x : x), bits, false);
                break;
            case 0x08:
                out = (u ? x >= 0 : x > 0) ? mask : 0;
                break;
            case 0x09:
                out = (u ? x <= 0 : x == 0) ? mask : 0;
                break;
            case 0x0A:
                out = x < 0 ? mask : 0;
                break;
            default:
                out = (uint64_t)(u ? -x : (x < 0 ? -x : x)) & mask;
                break;
        }
        sem_set_lane(&r, i, esize, out);
    }
    sem_vwrite(cpu, d, &r, q && !scalar);
}

/* SADDLP, UADDLP, SADALP, UADALP (`accumulate`): adjacent lanes added into lanes twice as wide. */
static inline void sem_simd_addlp(BiwaCpu* cpu, bool u, bool accumulate, bool q, unsigned esize, unsigned d,
                                  unsigned n) {
    const SemVec a = sem_vread(cpu, n);
    const SemVec acc = sem_vread(cpu, d);
    const unsigned bits = 8 * esize;
    SemVec r = {{0}};
    for (unsigned i = 0; i < (q ? 16 : 8) / (2 * esize); ++i) {
        const __int128 sum = sem_lane_value(sem_lane(&a, 2 * i, esize), bits, u) +
                             sem_lane_value(sem_lane(&a, 2 * i + 1, esize), bits, u);
        sem_set_lane(&r, i, 2 * esize, (uint64_t)sum + (accumulate ? sem_lane(&acc, i, 2 * esize) : 0));
    }
    sem_vwrite(cpu, d, &r, q);
}

enum { SEM_XTN, SEM_SQXTN, SEM_UQXTN, SEM_SQXTUN };

/* XTN, SQXTN, UQXTN, SQXTUN: lanes of 2 * esize narrowed to esize. The vector forms write the low
   half of Vd (clearing the upper) or, with q (the "2" forms), the upper half keeping the low one;
   the scalar forms write one lane. */
static inline void sem_simd_narrow(BiwaCpu* cpu, unsigned op, bool q, bool scalar, unsigned esize, unsigned d,
                                   unsigned n) {
    const SemVec a = sem_vread(cpu, n);
    SemVec r = {{0}};
    if (q && !scalar) {
        r = sem_vread(cpu, d);
    }
    const unsigned bits = 8 * esize;
    const unsigned lanes = scalar ? 1 : 8 / esize;
    const unsigned first = q && !scalar ? lanes : 0;
    for (unsigned i = 0; i < lanes; ++i) {
        const uint64_t wide = sem_lane(&a, i, 2 * esize);
        uint64_t out = 0;
        switch (op) {
            case SEM_XTN:
                out = wide & sem_lane_mask(bits);
                break;
            case SEM_SQXTN:
                out = sem_saturate(cpu, sem_lane_value(wide, 2 * bits, false), bits, false);
                break;
            case SEM_UQXTN:
                out = sem_saturate(cpu, sem_lane_value(wide, 2 * bits, true), bits, true);
                break;
            default:
                out = sem_saturate(cpu, sem_lane_value(wide, 2 * bits, false), bits, true);
                break;
        }
        sem_set_lane(&r, first + i, esize, out);
    }
    sem_vwrite(cpu, d, &r, true);
}

/* SHLL, SHLL2 (`upper`): lanes of the low or upper half widened and shifted left by their width. */
static inline void sem_simd_shll(BiwaCpu* cpu, bool upper, unsigned esize, unsigned d, unsigned n) {
    const SemVec a = sem_vread(cpu, n);
    SemVec r = {{0}};
    const unsigned lanes = 8 / esize;
    for (unsigned i = 0; i < lanes; ++i) {
        sem_set_lane(&r, i, 2 * esize, sem_lane(&a, (upper ? lanes : 0) + i, esize) << (8 * esize));
    }
    sem_vwrite(cpu, d, &r, true);
}

enum { SEM_ADDV, SEM_SMAXV, SEM_UMAXV, SEM_SMINV, SEM_UMINV, SEM_SADDLV, SEM_UADDLV };

/* ADDV, SMAXV, UMAXV, SMINV, UMINV, SADDLV, UADDLV: all lanes into one, in the low lane of Vd (twice
   as wide for the long sums), the rest cleared. */
static inline void sem_simd_across(BiwaCpu* cpu, unsigned op, bool q, unsigned esize, unsigned d, unsigned n) {
    const SemVec a = sem_vread(cpu, n);
    const unsigned bits = 8 * esize;
    const bool is_unsigned = op == SEM_UMAXV || op == SEM_UMINV || op == SEM_UADDLV;
    __int128 acc = sem_lane_value(sem_lane(&a, 0, esize), bits, is_unsigned);
    for (unsigned i = 1; i < (q ? 16 : 8) / esize; ++i) {
        const __int128 x = sem_lane_value(sem_lane(&a, i, esize), bits, is_unsigned);
        switch (op) {
            case SEM_SMAXV:
            case SEM_UMAXV:
                acc = x > acc ? x : acc;
                break;
            case SEM_SMINV:
            case SEM_UMINV:
                acc = x < acc ? x : acc;
                break;
            default:
                acc += x;
                break;
        }
    }
    const unsigned out = op == SEM_SADDLV || op == SEM_UADDLV ? 2 * esize : esize;
    SemVec r = {{0}};
    sem_set_lane(&r, 0, out, (uint64_t)acc & sem_lane_mask(8 * out));
    sem_vwrite(cpu, d, &r, false);
}

/* ---- Advanced SIMD: long, wide, narrowing, by-element and shift-by-immediate integer forms ---- */

/* Lane `index` of Vm in every lane (the by-element forms). */
static inline SemVec sem_broadcast(const BiwaCpu* cpu, unsigned m, unsigned index, unsigned esize) {
    const SemVec v = sem_vread(cpu, m);
    const uint64_t x = sem_lane(&v, index, esize);
    SemVec r = {{0}};
    for (unsigned i = 0; i < 16 / esize; ++i) {
        sem_set_lane(&r, i, esize, x);
    }
    return r;
}

/* PMULL, PMULL2: carry-less products of bytes into halfwords, or (esize 8, the crypto extension) of
   one doubleword pair into 128 bits. */
static inline void sem_pmull(BiwaCpu* cpu, bool q, unsigned esize, unsigned d, SemVec a, SemVec b) {
    SemVec r = {{0}};
    const unsigned first = q ? 8 / esize : 0;
    if (esize == 8) {
        const uint64_t x = sem_lane(&a, first, 8);
        const uint64_t y = sem_lane(&b, first, 8);
        uint64_t lo = 0;
        uint64_t hi = 0;
        for (unsigned k = 0; k < 64; ++k) {
            if (((y >> k) & 1) != 0) {
                lo ^= x << k;
                hi ^= k == 0 ? 0 : x >> (64 - k);
            }
        }
        sem_set_lane(&r, 0, 8, lo);
        sem_set_lane(&r, 1, 8, hi);
    } else {
        for (unsigned i = 0; i < 8; ++i) {
            const uint64_t x = sem_lane(&a, first + i, 1);
            const uint64_t y = sem_lane(&b, first + i, 1);
            uint64_t p = 0;
            for (unsigned k = 0; k < 8; ++k) {
                if (((y >> k) & 1) != 0) {
                    p ^= x << k;
                }
            }
            sem_set_lane(&r, i, 2, p);
        }
    }
    sem_vwrite(cpu, d, &r, true);
}

/* The three-different forms (opcode: bits 15..12) and the long by-element ones, vector or scalar:
   SADDL / UADDL (0), SADDW / UADDW (1), SSUBL / USUBL (2), SSUBW / USUBW (3), ADDHN / RADDHN (4),
   SABAL / UABAL (5), SUBHN / RSUBHN (6), SABDL / UABDL (7), SMLAL / UMLAL (8), SQDMLAL (9),
   SMLSL / UMLSL (10), SQDMLSL (11), SMULL / UMULL (12), SQDMULL (13). Lanes of esize widen to
   2 * esize (the W forms take wide lanes from Vn); the "2" forms (q) read the upper halves; the
   HN forms narrow the high halves of wide sums into the low or (q) upper half of Vd. */
static inline void sem_simd_long(BiwaCpu* cpu, bool u, unsigned opcode, bool q, bool scalar, unsigned esize, unsigned d,
                                 SemVec a, SemVec b) {
    const SemVec acc = sem_vread(cpu, d);
    const unsigned bits = 8 * esize;
    const unsigned wide = 2 * esize;
    SemVec r = {{0}};
    if (opcode == 4 || opcode == 6) {
        if (q) {
            r = acc;
        }
        for (unsigned i = 0; i < 8 / esize; ++i) {
            const uint64_t x = sem_lane(&a, i, wide);
            const uint64_t y = sem_lane(&b, i, wide);
            uint64_t sum = opcode == 4 ? x + y : x - y;
            if (u) {
                sum += (uint64_t)1 << (bits - 1);
            }
            sem_set_lane(&r, (q ? 8 / esize : 0) + i, esize, (sum >> bits) & sem_lane_mask(bits));
        }
        sem_vwrite(cpu, d, &r, true);
        return;
    }
    const unsigned first = q && !scalar ? 8 / esize : 0;
    const unsigned lanes = scalar ? 1 : 8 / esize;
    for (unsigned i = 0; i < lanes; ++i) {
        const __int128 x = opcode == 1 || opcode == 3 ? sem_lane_value(sem_lane(&a, i, wide), 2 * bits, u)
                                                      : sem_lane_value(sem_lane(&a, first + i, esize), bits, u);
        const __int128 y = sem_lane_value(sem_lane(&b, first + i, esize), bits, u);
        const uint64_t old = sem_lane(&acc, i, wide);
        __int128 out = 0;
        switch (opcode) {
            case 0:
            case 1:
                out = x + y;
                break;
            case 2:
            case 3:
                out = x - y;
                break;
            case 5:
                out = (__int128)old + (x > y ? x - y : y - x);
                break;
            case 7:
                out = x > y ? x - y : y - x;
                break;
            case 8:
                out = (__int128)old + x * y;
                break;
            case 10:
                out = (__int128)old - x * y;
                break;
            case 12:
                out = x * y;
                break;
            default: { /* SQDMLAL, SQDMLSL, SQDMULL: the doubled product saturated, then the sum */
                const uint64_t product = sem_saturate(cpu, 2 * x * y, 2 * bits, false);
                if (opcode == 13) {
                    out = (__int128)product;
                    break;
                }
                const __int128 p = sem_lane_value(product, 2 * bits, false);
                const __int128 o = sem_lane_value(old, 2 * bits, false);
                out = (__int128)sem_saturate(cpu, opcode == 9 ? o + p : o - p, 2 * bits, false);
                break;
            }
        }
        sem_set_lane(&r, i, wide, (uint64_t)out & sem_lane_mask(2 * bits));
    }
    sem_vwrite(cpu, d, &r, !scalar);
}

/* The shift-by-immediate forms (opcode: bits 15..11), vector or scalar, lanes of esize:
   SSHR / USHR (0x00), SSRA / USRA (0x02), SRSHR / URSHR (0x04), SRSRA / URSRA (0x06), SRI (0x08),
   SHL / SLI (0x0A), SQSHLU (0x0C), SQSHL / UQSHL (0x0E) by `shift` (right shifts 1..bits, left
   0..bits - 1). */
static inline void sem_simd_shift_imm(BiwaCpu* cpu, bool u, unsigned opcode, bool q, bool scalar, unsigned esize,
                                      unsigned shift, unsigned d, unsigned n) {
    const SemVec a = sem_vread(cpu, n);
    const SemVec acc = sem_vread(cpu, d);
    const unsigned bits = 8 * esize;
    const uint64_t mask = sem_lane_mask(bits);
    const unsigned lanes = scalar ? 1 : (q ? 16 : 8) / esize;
    SemVec r = {{0}};
    for (unsigned i = 0; i < lanes; ++i) {
        const uint64_t raw = sem_lane(&a, i, esize);
        const uint64_t old = sem_lane(&acc, i, esize);
        const __int128 x = sem_lane_value(raw, bits, u);
        uint64_t out = 0;
        switch (opcode) {
            case 0x00:
            case 0x02:
            case 0x04:
            case 0x06: {
                const bool rounding = opcode == 0x04 || opcode == 0x06;
                const __int128 shifted = (x + (rounding ? (__int128)1 << (shift - 1) : 0)) >> shift;
                out = ((uint64_t)shifted + (opcode == 0x02 || opcode == 0x06 ? old : 0)) & mask;
                break;
            }
            case 0x08: { /* SRI: Vd keeps its top `shift` bits */
                const uint64_t keep = shift >= bits ? mask : ~(mask >> shift) & mask;
                out = (old & keep) | (shift >= bits ? 0 : raw >> shift);
                break;
            }
            case 0x0A: /* SHL; SLI keeps Vd's low `shift` bits */
                out = ((raw << shift) & mask) | (u ? old & (((uint64_t)1 << shift) - 1) : 0);
                break;
            case 0x0C: /* SQSHLU: signed in, unsigned saturation */
                out = sem_saturate(cpu, sem_lane_value(raw, bits, false) * ((__int128)1 << shift), bits, true);
                break;
            default:
                out = sem_saturate(cpu, x * ((__int128)1 << shift), bits, u);
                break;
        }
        sem_set_lane(&r, i, esize, out);
    }
    sem_vwrite(cpu, d, &r, q && !scalar);
}

/* The narrowing shifts (opcode: bits 15..11): SHRN / SQSHRUN (0x10), RSHRN / SQRSHRUN (0x11),
   SQSHRN / UQSHRN (0x12), SQRSHRN / UQRSHRN (0x13): lanes of 2 * esize shifted right by `shift`
   (1..8 * esize) into esize, in the low half of Vd or (q, the "2" forms) its upper half; scalar:
   one lane. */
static inline void sem_simd_shift_narrow(BiwaCpu* cpu, bool u, unsigned opcode, bool q, bool scalar, unsigned esize,
                                         unsigned shift, unsigned d, unsigned n) {
    const SemVec a = sem_vread(cpu, n);
    const unsigned bits = 8 * esize;
    const bool rounding = (opcode & 1) != 0;
    /* SHRN / RSHRN truncate; SQSHRUN / SQRSHRUN read signed and saturate unsigned; the others read
       and saturate as signed (S) or unsigned (U). */
    const bool plain = opcode <= 0x11 && !u;
    const bool source_unsigned = opcode >= 0x12 && u;
    SemVec r = {{0}};
    if (q && !scalar) {
        r = sem_vread(cpu, d);
    }
    const unsigned lanes = scalar ? 1 : 8 / esize;
    const unsigned first = q && !scalar ? lanes : 0;
    for (unsigned i = 0; i < lanes; ++i) {
        const __int128 x = sem_lane_value(sem_lane(&a, i, 2 * esize), 2 * bits, source_unsigned || plain);
        const __int128 shifted = (x + (rounding ? (__int128)1 << (shift - 1) : 0)) >> shift;
        const uint64_t out = plain ? (uint64_t)shifted & sem_lane_mask(bits) : sem_saturate(cpu, shifted, bits, u);
        sem_set_lane(&r, first + i, esize, out);
    }
    sem_vwrite(cpu, d, &r, true);
}

/* SSHLL, USHLL (and "2"): lanes of the low or upper half widened and shifted left by `shift`. */
static inline void sem_simd_shll_imm(BiwaCpu* cpu, bool u, bool q, unsigned esize, unsigned shift, unsigned d,
                                     unsigned n) {
    const SemVec a = sem_vread(cpu, n);
    SemVec r = {{0}};
    const unsigned lanes = 8 / esize;
    for (unsigned i = 0; i < lanes; ++i) {
        const __int128 x = sem_lane_value(sem_lane(&a, (q ? lanes : 0) + i, esize), 8 * esize, u);
        sem_set_lane(&r, i, 2 * esize, (uint64_t)(x * ((__int128)1 << shift)));
    }
    sem_vwrite(cpu, d, &r, true);
}

#ifdef __cplusplus
}
#pragma clang diagnostic pop
#endif

#endif /* BIWA_A64_SEM_H */
