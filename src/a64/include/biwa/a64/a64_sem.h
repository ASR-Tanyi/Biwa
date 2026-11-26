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

#ifdef __cplusplus
}
#pragma clang diagnostic pop
#endif

#endif /* BIWA_A64_SEM_H */
