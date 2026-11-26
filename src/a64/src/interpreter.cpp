// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/a64/interpreter.h"

#include <optional>

#include "biwa/a64/a64_sem.h"
#include "biwa/a64/decoder.h"

namespace biwa::a64 {
namespace {

std::int64_t SignExtend(std::uint64_t value, unsigned bits) {
    const std::uint64_t sign = std::uint64_t{1} << (bits - 1);
    return static_cast<std::int64_t>((value ^ sign) - sign);
}

// The decoded fields of `word`, read with the positions of `form` (alias forms such as ASR or
// SXTB share them with their general form).
struct Fields {
    Op form;
    std::uint32_t word;
    [[nodiscard]] std::uint32_t operator()(char letter) const { return Field(form, word, letter); }
};

StepResult AddSubImmediate(BiwaCpu& cpu, Op op, std::uint32_t word) {
    const Fields f{op, word};
    const std::uint32_t shift = f('s');
    if (shift > 1) {
        return StepResult::kUndefined;
    }
    const bool sub = op == Op::SUB_imm || op == Op::SUBS_imm;
    const bool setflags = op == Op::ADDS_imm || op == Op::SUBS_imm;
    sem_addsub_imm(&cpu, f('z') != 0, sub, setflags, f('d'), f('n'), std::uint64_t{f('i')} << (12 * shift));
    return StepResult::kOk;
}

StepResult LogicalImmediate(BiwaCpu& cpu, Op op, std::uint32_t word) {
    const Fields f{op, word};
    const bool sf = f('z') != 0;
    if (!sf && f('N') != 0) {
        return StepResult::kUndefined;
    }
    std::uint64_t wmask = 0;
    std::uint64_t tmask = 0;
    if (!sem_decode_bit_masks(f('N'), f('s'), f('r'), true, sf ? 64 : 32, &wmask, &tmask)) {
        return StepResult::kUndefined;
    }
    const unsigned opc = op == Op::AND_imm ? 0 : op == Op::ORR_imm ? 1 : op == Op::EOR_imm ? 2 : 3;
    sem_logical_imm(&cpu, sf, opc, f('d'), f('n'), wmask);
    return StepResult::kOk;
}

StepResult MoveWide(BiwaCpu& cpu, Op op, std::uint32_t word) {
    const Fields f{op, word};
    const bool sf = f('z') != 0;
    const std::uint32_t hw = f('s');
    if (!sf && hw > 1) {
        return StepResult::kUndefined;
    }
    const unsigned opc = op == Op::MOVN ? 0 : op == Op::MOVZ ? 2 : 3;
    sem_move_wide(&cpu, sf, opc, f('d'), hw * 16, f('i'));
    return StepResult::kOk;
}

StepResult Bitfield(BiwaCpu& cpu, Op op, std::uint32_t word) {
    // The alias forms (ASR, SXTB, ...) are SBFM encodings: read them as SBFM.
    const Op form = op == Op::BFM ? Op::BFM : op == Op::UBFM ? Op::UBFM : Op::SBFM;
    const Fields f{form, word};
    const bool sf = f('z') != 0;
    const std::uint32_t n = f('N');
    const std::uint32_t immr = f('r');
    const std::uint32_t imms = f('s');
    if (n != static_cast<std::uint32_t>(sf) || (!sf && (immr > 31 || imms > 31))) {
        return StepResult::kUndefined;
    }
    std::uint64_t wmask = 0;
    std::uint64_t tmask = 0;
    if (!sem_decode_bit_masks(n, imms, immr, false, sf ? 64 : 32, &wmask, &tmask)) {
        return StepResult::kUndefined;
    }
    const unsigned opc = form == Op::SBFM ? 0 : form == Op::BFM ? 1 : 2;
    sem_bitfield(&cpu, sf, opc, f('d'), f('n'), immr, imms, wmask, tmask);
    return StepResult::kOk;
}

StepResult Extract(BiwaCpu& cpu, std::uint32_t word) {
    const Fields f{Op::EXTR, word};
    const bool sf = f('z') != 0;
    const std::uint32_t lsb = f('s');
    if (f('N') != static_cast<std::uint32_t>(sf) || (!sf && lsb > 31)) {
        return StepResult::kUndefined;
    }
    sem_extr(&cpu, sf, f('d'), f('n'), f('m'), lsb);
    return StepResult::kOk;
}

StepResult LogicalShifted(BiwaCpu& cpu, Op op, std::uint32_t word) {
    const Fields f{op, word};
    const bool sf = f('z') != 0;
    const std::uint32_t amount = f('i');
    if (!sf && amount > 31) {
        return StepResult::kUndefined;
    }
    unsigned opc = 0;
    bool invert = false;
    switch (op) {
        case Op::BIC_shift:
            invert = true;
            break;
        case Op::ORR_shift:
            opc = 1;
            break;
        case Op::ORN_shift:
            opc = 1;
            invert = true;
            break;
        case Op::EOR_shift:
            opc = 2;
            break;
        case Op::EON:
            opc = 2;
            invert = true;
            break;
        case Op::ANDS_shift:
            opc = 3;
            break;
        case Op::BICS:
            opc = 3;
            invert = true;
            break;
        default:
            break;
    }
    sem_logical_shift(&cpu, sf, opc, invert, f('d'), f('n'), f('m'), f('s'), amount);
    return StepResult::kOk;
}

StepResult AddSubShifted(BiwaCpu& cpu, Op op, std::uint32_t word) {
    const Fields f{op, word};
    const bool sf = f('z') != 0;
    const std::uint32_t type = f('s');
    const std::uint32_t amount = f('i');
    if (type == 3 || (!sf && amount > 31)) {
        return StepResult::kUndefined;
    }
    const bool sub = op == Op::SUB_shift || op == Op::SUBS_shift;
    const bool setflags = op == Op::ADDS_shift || op == Op::SUBS_shift;
    sem_addsub_shift(&cpu, sf, sub, setflags, f('d'), f('n'), f('m'), type, amount);
    return StepResult::kOk;
}

StepResult AddSubExtended(BiwaCpu& cpu, Op op, std::uint32_t word) {
    const Fields f{op, word};
    const std::uint32_t shift = f('i');
    if (shift > 4) {
        return StepResult::kUndefined;
    }
    const bool sub = op == Op::SUB_ext || op == Op::SUBS_ext;
    const bool setflags = op == Op::ADDS_ext || op == Op::SUBS_ext;
    sem_addsub_ext(&cpu, f('z') != 0, sub, setflags, f('d'), f('n'), f('m'), f('x'), shift);
    return StepResult::kOk;
}

StepResult OneSource(BiwaCpu& cpu, Op op, std::uint32_t word) {
    const Fields f{op, word};
    const bool sf = f('z') != 0;
    const std::uint32_t d = f('d');
    const std::uint32_t n = f('n');
    switch (op) {
        case Op::RBIT_int:
            sem_rbit(&cpu, sf, d, n);
            break;
        case Op::REV16_int:
            sem_rev(&cpu, sf, 2, d, n);
            break;
        case Op::REV32_int:
            sem_rev(&cpu, true, 4, d, n);
            break;
        case Op::REV:
            // opc 10 is REV (32-bit) and opc 11 is REV (64-bit); 64-bit opc 10 is REV32, decoded on
            // its own, and 32-bit opc 11 is unallocated.
            if (f('o') != static_cast<std::uint32_t>(sf)) {
                return StepResult::kUndefined;
            }
            sem_rev(&cpu, sf, sf ? 8 : 4, d, n);
            break;
        case Op::CLZ_int:
            sem_clz(&cpu, sf, false, d, n);
            break;
        default:
            sem_clz(&cpu, sf, true, d, n);
            break;
    }
    return StepResult::kOk;
}

StepResult TwoSource(BiwaCpu& cpu, Op op, std::uint32_t word) {
    const Fields f{op, word};
    const bool sf = f('z') != 0;
    const std::uint32_t d = f('d');
    const std::uint32_t n = f('n');
    const std::uint32_t m = f('m');
    switch (op) {
        case Op::UDIV:
        case Op::SDIV:
            sem_div(&cpu, sf, op == Op::SDIV, d, n, m);
            break;
        case Op::LSLV:
            sem_shiftv(&cpu, sf, 0, d, n, m);
            break;
        case Op::LSRV:
            sem_shiftv(&cpu, sf, 1, d, n, m);
            break;
        case Op::ASRV:
            sem_shiftv(&cpu, sf, 2, d, n, m);
            break;
        default:
            sem_shiftv(&cpu, sf, 3, d, n, m);
            break;
    }
    return StepResult::kOk;
}

StepResult Crc32(BiwaCpu& cpu, Op op, std::uint32_t word) {
    const Fields f{op, word};
    // The pattern names sf and sz both 'z': the field reads sf:sz.
    const std::uint32_t sf_sz = f('z');
    const std::uint32_t sz = sf_sz & 3;
    if ((sf_sz >> 2) != static_cast<std::uint32_t>(sz == 3)) {
        return StepResult::kUndefined;  // CRC32X needs a 64-bit Rm; the others a 32-bit one
    }
    sem_crc32(&cpu, op == Op::CRC32C, 1u << sz, f('d'), f('n'), f('m'));
    return StepResult::kOk;
}

StepResult WithCarry(BiwaCpu& cpu, Op op, std::uint32_t word) {
    const Fields f{op, word};
    const bool sub = op == Op::SBC || op == Op::SBCS;
    const bool setflags = op == Op::ADCS || op == Op::SBCS;
    sem_adc(&cpu, f('z') != 0, sub, setflags, f('d'), f('n'), f('m'));
    return StepResult::kOk;
}

StepResult ConditionalCompare(BiwaCpu& cpu, Op op, std::uint32_t word) {
    const Fields f{op, word};
    const bool sf = f('z') != 0;
    const bool immediate = op == Op::CCMN_imm || op == Op::CCMP_imm;
    const std::uint64_t operand = immediate ? f('i') : sem_x(&cpu, f('m'));
    const bool sub = op == Op::CCMP_reg || op == Op::CCMP_imm;
    sem_ccmp(&cpu, sf, sub, f('n'), operand, f('c'), f('f'));
    return StepResult::kOk;
}

StepResult ConditionalSelect(BiwaCpu& cpu, Op op, std::uint32_t word) {
    const Fields f{op, word};
    const unsigned kind = op == Op::CSEL ? 0 : op == Op::CSINC ? 1 : op == Op::CSINV ? 2 : 3;
    sem_csel(&cpu, f('z') != 0, kind, f('d'), f('n'), f('m'), f('c'));
    return StepResult::kOk;
}

StepResult ThreeSource(BiwaCpu& cpu, Op op, std::uint32_t word) {
    const Fields f{op, word};
    const std::uint32_t d = f('d');
    const std::uint32_t n = f('n');
    const std::uint32_t m = f('m');
    switch (op) {
        case Op::MADD:
        case Op::MSUB:
            sem_madd(&cpu, f('z') != 0, op == Op::MSUB, d, n, m, f('a'));
            break;
        case Op::SMADDL:
        case Op::SMSUBL:
            sem_maddl(&cpu, true, op == Op::SMSUBL, d, n, m, f('a'));
            break;
        case Op::UMADDL:
        case Op::UMSUBL:
            sem_maddl(&cpu, false, op == Op::UMSUBL, d, n, m, f('a'));
            break;
        default:
            sem_mulh(&cpu, op == Op::SMULH, d, n, m);
            break;
    }
    return StepResult::kOk;
}

// ---- Loads and stores ----

// The load/store register classes keep size in bits 31..30 and opc in bits 23..22.
std::uint32_t LoadStoreSize(std::uint32_t word) { return word >> 30; }
std::uint32_t LoadStoreOpc(std::uint32_t word) { return (word >> 22) & 3; }

// A general-register access with `size` and `opc`: kUndefined for the combinations that are not
// allocated; size 3 with opc 2 is a prefetch, which does nothing here.
StepResult GeneralAccess(BiwaCpu& cpu, std::uint32_t size, std::uint32_t opc, std::uint32_t t, std::uint64_t address) {
    if ((size == 3 && opc == 3) || (size == 2 && opc == 3)) {
        return StepResult::kUndefined;
    }
    if (size == 3 && opc == 2) {
        return StepResult::kOk;  // PRFM
    }
    sem_ldst(&cpu, size, opc, t, address);
    return StepResult::kOk;
}

// The base register (SP for 31), then the access at base + offset (or at base when post-indexed),
// then the writeback. With writeback and Rt == Rn the result is CONSTRAINED UNPREDICTABLE; doing
// the access first and the writeback last matches Unicorn.
template <typename Access>
StepResult Indexed(BiwaCpu& cpu, std::uint32_t n, std::int64_t offset, bool writeback, bool postindex, Access access) {
    const std::uint64_t base = sem_xsp(&cpu, n);
    const std::uint64_t address = postindex ? base : base + static_cast<std::uint64_t>(offset);
    const StepResult result = access(address);
    if (result == StepResult::kOk && writeback) {
        sem_set_xsp(&cpu, n, base + static_cast<std::uint64_t>(offset));
    }
    return result;
}

StepResult LoadLiteral(BiwaCpu& cpu, Op op, std::uint32_t word) {
    const Fields f{op, word};
    const std::uint64_t address = cpu.pc + static_cast<std::uint64_t>(SignExtend(f('i'), 19) * 4);
    const std::uint32_t opc = word >> 30;
    const std::uint32_t t = f('t');
    if (op == Op::LDR_lit_fpsimd) {
        if (opc == 3) {
            return StepResult::kUndefined;
        }
        sem_ldst_v(&cpu, opc + 2, true, t, address);
        return StepResult::kOk;
    }
    switch (opc) {
        case 0:
            sem_ldst(&cpu, 2, 1, t, address);
            break;
        case 1:
            sem_ldst(&cpu, 3, 1, t, address);
            break;
        case 2:
            sem_ldst(&cpu, 2, 2, t, address);  // LDRSW
            break;
        default:
            break;  // PRFM
    }
    return StepResult::kOk;
}

// STP, LDP, LDPSW, STNP, LDNP on general or SIMD&FP registers.
StepResult LoadStorePair(BiwaCpu& cpu, Op op, std::uint32_t word) {
    const Fields f{op, word};
    const bool vector = op == Op::STP_LDP_fpsimd || op == Op::STNP_LDNP_fpsimd;
    const bool no_allocate = op == Op::STNP_LDNP_gen || op == Op::STNP_LDNP_fpsimd;
    const std::uint32_t opc = word >> 30;
    const bool load = ((word >> 22) & 1) != 0;
    // Index mode in bits 24..23: 1 post-index, 2 signed offset, 3 pre-index (0 is no-allocate).
    const std::uint32_t mode = (word >> 23) & 3;
    if (opc == 3 || (!vector && opc == 1 && (!load || no_allocate))) {
        return StepResult::kUndefined;  // STGP and the 64-bit LDPSW slots are not ARMv8.0
    }
    const std::uint32_t t = f('t');
    const std::uint32_t t2 = f('u');
    const std::uint32_t scale = vector ? opc + 2 : (opc == 2 ? 3 : 2);
    const std::int64_t offset = SignExtend(f('i'), 7) * (std::int64_t{1} << scale);
    const bool writeback = !no_allocate && mode != 2;
    const bool postindex = !no_allocate && mode == 1;
    return Indexed(cpu, f('n'), offset, writeback, postindex, [&](std::uint64_t address) {
        if (vector) {
            sem_ldst_pair_v(&cpu, scale, load, t, t2, address);
        } else {
            sem_ldst_pair(&cpu, scale, load, opc == 1, t, t2, address);
        }
        return StepResult::kOk;
    });
}

// The SIMD&FP register size: opc bit 1 joined to size, 0 (B) to 4 (Q).
std::uint32_t VectorScale(std::uint32_t word) { return (((word >> 23) & 1) << 2) | (word >> 30); }

// STR / LDR (and STUR / LDUR, unprivileged) on one register with an immediate: `scaled` for the
// unsigned 12-bit offset, otherwise a signed 9-bit one, pre- or post-indexed when `writeback`.
StepResult LoadStoreImmediate(BiwaCpu& cpu, Op op, std::uint32_t word, bool vector, bool scaled, bool writeback) {
    const Fields f{op, word};
    const std::uint32_t size = vector ? VectorScale(word) : LoadStoreSize(word);
    if (vector && size > 4) {
        return StepResult::kUndefined;
    }
    const std::int64_t offset =
        scaled ? static_cast<std::int64_t>(std::uint64_t{f('i')} << size) : SignExtend(f('i'), 9);
    const bool postindex = writeback && ((word >> 11) & 1) == 0;
    const std::uint32_t t = f('t');
    return Indexed(cpu, f('n'), offset, writeback, postindex, [&](std::uint64_t address) {
        if (vector) {
            sem_ldst_v(&cpu, size, ((word >> 22) & 1) != 0, t, address);
            return StepResult::kOk;
        }
        return GeneralAccess(cpu, size, LoadStoreOpc(word), t, address);
    });
}

// STR / LDR (register offset): Rm extended by option (UXTW, LSL, SXTW, SXTX) and scaled when S is set.
StepResult LoadStoreRegisterOffset(BiwaCpu& cpu, Op op, std::uint32_t word, bool vector) {
    const Fields f{op, word};
    const std::uint32_t size = vector ? VectorScale(word) : LoadStoreSize(word);
    const std::uint32_t option = f('x');
    if ((option & 2) == 0 || (vector && size > 4)) {
        return StepResult::kUndefined;
    }
    const std::uint64_t index = sem_extend(sem_x(&cpu, f('m')), option, f('S') != 0 ? size : 0);
    const std::uint64_t address = sem_xsp(&cpu, f('n')) + index;
    if (vector) {
        sem_ldst_v(&cpu, size, ((word >> 22) & 1) != 0, f('t'), address);
        return StepResult::kOk;
    }
    return GeneralAccess(cpu, size, LoadStoreOpc(word), f('t'), address);
}

// The post-index writeback of the structure forms: Xm, or the bytes transferred when Rm is 31.
void StructureWriteback(BiwaCpu& cpu, std::uint32_t word, std::uint64_t base, std::uint64_t bytes) {
    if (((word >> 23) & 1) == 0) {
        return;
    }
    const std::uint32_t m = (word >> 16) & 31;
    sem_set_xsp(&cpu, word >> 5 & 31, base + (m == 31 ? bytes : sem_x(&cpu, m)));
}

// LD1-LD4 / ST1-ST4 (multiple structures).
StepResult LoadStoreMultiple(BiwaCpu& cpu, std::uint32_t word) {
    const bool q = ((word >> 30) & 1) != 0;
    const bool load = ((word >> 22) & 1) != 0;
    const std::uint32_t size = (word >> 10) & 3;
    unsigned rpt = 1;
    unsigned selem = 1;
    switch ((word >> 12) & 15) {
        case 0:  // LD4 / ST4
            selem = 4;
            break;
        case 2:  // LD1 / ST1, four registers
            rpt = 4;
            break;
        case 4:  // LD3 / ST3
            selem = 3;
            break;
        case 6:  // LD1 / ST1, three registers
            rpt = 3;
            break;
        case 7:  // LD1 / ST1, one register
            break;
        case 8:  // LD2 / ST2
            selem = 2;
            break;
        case 10:  // LD1 / ST1, two registers
            rpt = 2;
            break;
        default:
            return StepResult::kUndefined;
    }
    if (size == 3 && !q && selem != 1) {
        return StepResult::kUndefined;  // 1D arrangements only for LD1 / ST1
    }
    const std::uint64_t base = sem_xsp(&cpu, (word >> 5) & 31);
    const std::uint64_t bytes = sem_ldst_multiple(&cpu, load, q, size, rpt, selem, word & 31, base);
    StructureWriteback(cpu, word, base, bytes);
    return StepResult::kOk;
}

// LD1-LD4 / ST1-ST4 (single structure) and LD1R-LD4R.
StepResult LoadStoreSingle(BiwaCpu& cpu, std::uint32_t word) {
    const std::uint32_t q = (word >> 30) & 1;
    const bool load = ((word >> 22) & 1) != 0;
    const std::uint32_t r = (word >> 21) & 1;
    const std::uint32_t opcode = (word >> 13) & 7;
    const std::uint32_t s = (word >> 12) & 1;
    const std::uint32_t size = (word >> 10) & 3;
    const unsigned selem = (((opcode & 1) << 1) | r) + 1;
    std::uint32_t scale = opcode >> 1;
    std::uint32_t index = 0;
    const std::uint64_t base = sem_xsp(&cpu, (word >> 5) & 31);
    switch (scale) {
        case 3: {
            if (!load || s != 0) {
                return StepResult::kUndefined;
            }
            const std::uint64_t bytes = sem_ld_replicate(&cpu, q != 0, size, selem, word & 31, base);
            StructureWriteback(cpu, word, base, bytes);
            return StepResult::kOk;
        }
        case 0:
            index = (q << 3) | (s << 2) | size;
            break;
        case 1:
            if ((size & 1) != 0) {
                return StepResult::kUndefined;
            }
            index = (q << 2) | (s << 1) | (size >> 1);
            break;
        default:
            if ((size & 2) != 0 || (size == 1 && s != 0)) {
                return StepResult::kUndefined;
            }
            if (size == 0) {
                index = (q << 1) | s;
            } else {
                index = q;
                scale = 3;
            }
            break;
    }
    const std::uint64_t bytes = sem_ldst_single(&cpu, load, scale, selem, index, word & 31, base);
    StructureWriteback(cpu, word, base, bytes);
    return StepResult::kOk;
}

StepResult LoadStore(BiwaCpu& cpu, Op op, std::uint32_t word) {
    switch (op) {
        case Op::LDR_lit_gen:
        case Op::LDRSW_lit:
        case Op::PRFM_lit:
        case Op::LDR_lit_fpsimd:
            return LoadLiteral(cpu, op, word);
        case Op::STNP_LDNP_gen:
        case Op::STNP_LDNP_fpsimd:
        case Op::STP_LDP_gen:
        case Op::STP_LDP_fpsimd:
            return LoadStorePair(cpu, op, word);
        case Op::PRFM_imm:
        case Op::PRFM_unscaled_imm:
            return StepResult::kOk;
        case Op::STURx_LDURx:
        case Op::STTRB:
        case Op::LDTRB:
        case Op::LDTRSB:
        case Op::STTRH:
        case Op::LDTRH:
        case Op::LDTRSH:
        case Op::STTR:
        case Op::LDTR:
        case Op::LDTRSW:
            // Unprivileged accesses are ordinary ones at EL0.
            return LoadStoreImmediate(cpu, op, word, false, false, false);
        case Op::STUR_fpsimd:
        case Op::LDUR_fpsimd:
            return LoadStoreImmediate(cpu, op, word, true, false, false);
        case Op::STRx_LDRx_imm_1:
            return LoadStoreImmediate(cpu, op, word, false, false, true);
        case Op::STRx_LDRx_imm_2:
            return LoadStoreImmediate(cpu, op, word, false, true, false);
        case Op::STR_imm_fpsimd_1:
        case Op::LDR_imm_fpsimd_1:
            return LoadStoreImmediate(cpu, op, word, true, false, true);
        case Op::STR_imm_fpsimd_2:
        case Op::LDR_imm_fpsimd_2:
            return LoadStoreImmediate(cpu, op, word, true, true, false);
        case Op::STRx_reg:
        case Op::LDRx_reg:
            return LoadStoreRegisterOffset(cpu, op, word, false);
        case Op::STR_reg_fpsimd:
        case Op::LDR_reg_fpsimd:
            return LoadStoreRegisterOffset(cpu, op, word, true);
        case Op::STx_mult_1:
        case Op::STx_mult_2:
        case Op::LDx_mult_1:
        case Op::LDx_mult_2:
            return LoadStoreMultiple(cpu, word);
        case Op::ST1_sngl_1:
        case Op::ST1_sngl_2:
        case Op::ST2_sngl_1:
        case Op::ST2_sngl_2:
        case Op::ST3_sngl_1:
        case Op::ST3_sngl_2:
        case Op::ST4_sngl_1:
        case Op::ST4_sngl_2:
        case Op::LD1_sngl_1:
        case Op::LD1_sngl_2:
        case Op::LD2_sngl_1:
        case Op::LD2_sngl_2:
        case Op::LD3_sngl_1:
        case Op::LD3_sngl_2:
        case Op::LD4_sngl_1:
        case Op::LD4_sngl_2:
        case Op::LD1R_1:
        case Op::LD1R_2:
        case Op::LD2R_1:
        case Op::LD2R_2:
        case Op::LD3R_1:
        case Op::LD3R_2:
        case Op::LD4R_1:
        case Op::LD4R_2:
            return LoadStoreSingle(cpu, word);
        default:
            return StepResult::kNotImplemented;
    }
}

// ---- Branches and system ----

// B, BL, B.cond, CBZ, CBNZ, TBZ, TBNZ, BR, BLR, RET: sets pc and `branched` when the branch is
// taken; a branch not taken leaves pc to Step.
StepResult Branch(BiwaCpu& cpu, Op op, std::uint32_t word, bool& branched) {
    const Fields f{op, word};
    std::uint64_t target = 0;
    bool taken = true;
    switch (op) {
        case Op::B_uncond:
        case Op::BL:
            target = cpu.pc + static_cast<std::uint64_t>(SignExtend(f('i'), 26) * 4);
            if (op == Op::BL) {
                cpu.x[30] = cpu.pc + 4;
            }
            break;
        case Op::B_cond:
            target = cpu.pc + static_cast<std::uint64_t>(SignExtend(f('i'), 19) * 4);
            taken = sem_condition_holds(cpu.nzcv, f('c'));
            break;
        case Op::CBZ:
        case Op::CBNZ:
            target = cpu.pc + static_cast<std::uint64_t>(SignExtend(f('i'), 19) * 4);
            taken = sem_is_zero(&cpu, f('z') != 0, f('t')) == (op == Op::CBZ);
            break;
        case Op::TBZ:
        case Op::TBNZ:
            target = cpu.pc + static_cast<std::uint64_t>(SignExtend(f('i'), 14) * 4);
            taken = sem_bit_is_zero(&cpu, f('t'), f('b')) == (op == Op::TBZ);
            break;
        default:  // BR, BLR, RET: Rn is read before BLR writes the link register
            target = sem_x(&cpu, f('n'));
            if (op == Op::BLR) {
                cpu.x[30] = cpu.pc + 4;
            }
            break;
    }
    if (taken) {
        cpu.pc = target;
        branched = true;
    }
    return StepResult::kOk;
}

// The system register an MRS or MSR names: op0 (2 or 3), op1, CRn, CRm, op2 packed as in the
// instruction (bits 19..5).
constexpr std::uint32_t SystemRegister(std::uint32_t op0, std::uint32_t op1, std::uint32_t crn, std::uint32_t crm,
                                       std::uint32_t op2) {
    return ((op0 & 1) << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2;
}

constexpr std::uint32_t kCtrEl0 = SystemRegister(3, 3, 0, 0, 1);
constexpr std::uint32_t kDczidEl0 = SystemRegister(3, 3, 0, 0, 7);
constexpr std::uint32_t kNzcv = SystemRegister(3, 3, 4, 2, 0);
constexpr std::uint32_t kFpcr = SystemRegister(3, 3, 4, 4, 0);
constexpr std::uint32_t kFpsr = SystemRegister(3, 3, 4, 4, 1);
constexpr std::uint32_t kPmuserenrEl0 = SystemRegister(3, 3, 9, 14, 0);
constexpr std::uint32_t kTpidrEl0 = SystemRegister(3, 3, 13, 0, 2);
constexpr std::uint32_t kTpidrroEl0 = SystemRegister(3, 3, 13, 0, 3);
constexpr std::uint32_t kCntfrqEl0 = SystemRegister(3, 3, 14, 0, 0);
constexpr std::uint32_t kCntpctEl0 = SystemRegister(3, 3, 14, 0, 1);
constexpr std::uint32_t kCntvctEl0 = SystemRegister(3, 3, 14, 0, 2);

// MRS and MSR (register) on the registers EL0 may use; anything else is undefined at EL0.
StepResult SystemRegisterAccess(BiwaCpu& cpu, std::uint32_t word, bool read) {
    const std::uint32_t reg = (word >> 5) & 0x7FFF;
    const std::uint32_t t = word & 31;
    if (read) {
        std::uint64_t value = 0;
        switch (reg) {
            case kCtrEl0:
                value = SEM_CTR_EL0;
                break;
            case kDczidEl0:
                value = SEM_DCZID_EL0;
                break;
            case kNzcv:
                value = cpu.nzcv;
                break;
            case kFpcr:
                value = cpu.fpcr;
                break;
            case kFpsr:
                sem_fp_fold(&cpu);
                value = cpu.fpsr;
                break;
            case kPmuserenrEl0:
                value = 0;  // no performance monitor access from EL0
                break;
            case kTpidrEl0:
                value = cpu.tpidr_el0;
                break;
            case kTpidrroEl0:
                value = cpu.tpidrro_el0;
                break;
            case kCntfrqEl0:
                value = SEM_CNTFRQ_EL0;
                break;
            case kCntpctEl0:
            case kCntvctEl0:
                return StepResult::kNotImplemented;  // the counter comes from the runtime
            default:
                return StepResult::kUndefined;
        }
        sem_set_x(&cpu, t, value);
        return StepResult::kOk;
    }
    const std::uint64_t value = sem_x(&cpu, t);
    switch (reg) {
        case kNzcv:
            cpu.nzcv = static_cast<std::uint32_t>(value) & 0xF0000000;
            break;
        case kFpcr:
            cpu.fpcr = static_cast<std::uint32_t>(value) & SEM_FPCR_MASK;
            sem_host_set_rounding(cpu.fpcr);
            break;
        case kFpsr:
            cpu.fpsr = static_cast<std::uint32_t>(value) & SEM_FPSR_MASK;
            sem_host_clear_flags();
            break;
        case kTpidrEl0:
            cpu.tpidr_el0 = value;
            break;
        default:
            return StepResult::kUndefined;
    }
    return StepResult::kOk;
}

// ---- Exclusives, load-acquire and store-release ----

StepResult Exclusive(BiwaCpu& cpu, Op op, std::uint32_t word) {
    const std::uint32_t size = word >> 30;  // pairs: 2 + sz
    const std::uint32_t s = (word >> 16) & 31;
    const std::uint32_t t2 = (word >> 10) & 31;
    const std::uint32_t t = word & 31;
    const std::uint64_t address = sem_xsp(&cpu, (word >> 5) & 31);
    const bool pair = op == Op::LDXP || op == Op::LDAXP || op == Op::STXP || op == Op::STLXP;
    if (!sem_is_aligned(address, pair ? 2u << size : 1u << size)) {
        return StepResult::kAlignmentFault;
    }
    switch (op) {
        case Op::LDXR:
        case Op::LDAXR:
            sem_ldxr(&cpu, size, t, address);
            break;
        case Op::LDXP:
        case Op::LDAXP:
            sem_ldxp(&cpu, size, t, t2, address);
            break;
        case Op::STXR:
        case Op::STLXR:
            sem_stxr(&cpu, size, s, t, address);
            break;
        case Op::STXP:
        case Op::STLXP:
            sem_stxp(&cpu, size, s, t, t2, address);
            break;
        case Op::LDAR:
            sem_ldar(&cpu, size, t, address);
            break;
        default:  // STLR
            sem_stlr(&cpu, size, t, address);
            break;
    }
    return StepResult::kOk;
}

// ---- Floating point ----

// The precision in bits 23..22: single (0) and double (1). Half precision (3) is ARMv8.2 and 2 is
// not allocated: both undefined on the A57.
std::optional<bool> FpDouble(std::uint32_t word) {
    switch ((word >> 22) & 3) {
        case 0:
            return false;
        case 1:
            return true;
        default:
            return std::nullopt;
    }
}

unsigned FpBytes(bool dbl) { return dbl ? 8 : 4; }

// FMOV (general): between a general register and S, D or the upper half of V (type 2, rmode 1).
StepResult FpMoveGeneral(BiwaCpu& cpu, std::uint32_t word) {
    const bool sf = (word >> 31) != 0;
    const std::uint32_t type = (word >> 22) & 3;
    const std::uint32_t rmode = (word >> 19) & 3;
    const bool to_vector = ((word >> 16) & 1) != 0;
    const std::uint32_t n = (word >> 5) & 31;
    const std::uint32_t d = word & 31;
    const bool single = !sf && type == 0 && rmode == 0;
    const bool dbl = sf && type == 1 && rmode == 0;
    const bool upper = sf && type == 2 && rmode == 1;
    if (!single && !dbl && !upper) {
        return StepResult::kUndefined;  // the half-precision forms are ARMv8.2
    }
    const unsigned bytes = single ? 4 : 8;
    if (!upper) {
        if (to_vector) {
            sem_v_set(&cpu, d, bytes, sem_x(&cpu, n));
        } else {
            sem_set_x(&cpu, d, sem_v_get(&cpu, n, bytes));
        }
        return StepResult::kOk;
    }
    if (to_vector) {
        const std::uint64_t value = sem_x(&cpu, n);
        for (unsigned i = 0; i < 8; ++i) {
            cpu.v[d][8 + i] = static_cast<std::uint8_t>(value >> (8 * i));
        }
    } else {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i) {
            value |= std::uint64_t{cpu.v[n][8 + i]} << (8 * i);
        }
        sem_set_x(&cpu, d, value);
    }
    return StepResult::kOk;
}

// FCVT between single, double and half precision: source type in bits 23..22, destination in
// bits 16..15 (0 single, 1 double, 3 half).
StepResult FpConvertPrecision(BiwaCpu& cpu, std::uint32_t word) {
    const std::uint32_t from = (word >> 22) & 3;
    const std::uint32_t to = (word >> 15) & 3;
    if (from == to || from == 2 || to == 2) {
        return StepResult::kUndefined;
    }
    const std::uint32_t n = (word >> 5) & 31;
    const std::uint32_t d = word & 31;
    const unsigned from_bytes = from == 3 ? 2 : FpBytes(from == 1);
    const unsigned to_bytes = to == 3 ? 2 : FpBytes(to == 1);
    const std::uint64_t x = sem_v_get(&cpu, n, from_bytes);
    std::uint64_t result = 0;
    if (from == 3) {
        result = sem_fp_from_half(&cpu, to == 1, x);
    } else if (to == 3) {
        result = sem_fp_to_half(&cpu, from == 1, x);
    } else {
        result = sem_fp_convert(&cpu, from == 1, x);
    }
    sem_v_set(&cpu, d, to_bytes, result);
    return StepResult::kOk;
}

// Conversions between FP and integer or fixed point (sf in bit 31; for the fixed-point forms the
// scale in bits 15..10, fbits = 64 - scale, at most 32 for W).
StepResult FpConvertInteger(BiwaCpu& cpu, Op op, std::uint32_t word) {
    const std::optional<bool> precision = FpDouble(word);
    if (!precision) {
        return StepResult::kUndefined;  // half precision is ARMv8.2
    }
    const bool dbl = *precision;
    const bool sf = (word >> 31) != 0;
    const unsigned bits = sf ? 64 : 32;
    const bool fixed = op == Op::SCVTF_float_fix || op == Op::UCVTF_float_fix || op == Op::FCVTZS_float_fix ||
                       op == Op::FCVTZU_float_fix;
    const std::uint32_t scale = (word >> 10) & 63;
    if (fixed && !sf && scale < 32) {
        return StepResult::kUndefined;
    }
    const unsigned fbits = fixed ? 64 - scale : 0;
    const std::uint32_t n = (word >> 5) & 31;
    const std::uint32_t d = word & 31;
    if (op == Op::SCVTF_float_int || op == Op::UCVTF_float_int || op == Op::SCVTF_float_fix ||
        op == Op::UCVTF_float_fix) {
        const bool is_unsigned = op == Op::UCVTF_float_int || op == Op::UCVTF_float_fix;
        sem_v_set(&cpu, d, FpBytes(dbl), sem_int_to_fp(dbl, sem_x(&cpu, n), bits, is_unsigned, fbits));
        return StepResult::kOk;
    }
    unsigned rounding = SEM_ROUND_ZERO;
    bool is_unsigned = false;
    switch (op) {
        case Op::FCVTNS_float:
        case Op::FCVTNU_float:
            rounding = SEM_ROUND_NEAREST;
            is_unsigned = op == Op::FCVTNU_float;
            break;
        case Op::FCVTAS_float:
        case Op::FCVTAU_float:
            rounding = SEM_ROUND_AWAY;
            is_unsigned = op == Op::FCVTAU_float;
            break;
        case Op::FCVTPS_float:
        case Op::FCVTPU_float:
            rounding = SEM_ROUND_UP;
            is_unsigned = op == Op::FCVTPU_float;
            break;
        case Op::FCVTMS_float:
        case Op::FCVTMU_float:
            rounding = SEM_ROUND_DOWN;
            is_unsigned = op == Op::FCVTMU_float;
            break;
        default:  // FCVTZS, FCVTZU (integer and fixed-point)
            is_unsigned = op == Op::FCVTZU_float_int || op == Op::FCVTZU_float_fix;
            break;
    }
    const std::uint64_t x = sem_v_get(&cpu, n, FpBytes(dbl));
    sem_set_x(&cpu, d, sem_fp_to_int(&cpu, dbl, x, fbits, rounding, bits, is_unsigned));
    return StepResult::kOk;
}

StepResult FloatingPoint(BiwaCpu& cpu, Op op, std::uint32_t word) {
    switch (op) {
        case Op::FMOV_float_gen:
            return FpMoveGeneral(cpu, word);
        case Op::FCVT_float:
            return FpConvertPrecision(cpu, word);
        case Op::SCVTF_float_fix:
        case Op::UCVTF_float_fix:
        case Op::FCVTZS_float_fix:
        case Op::FCVTZU_float_fix:
        case Op::FCVTNS_float:
        case Op::FCVTNU_float:
        case Op::SCVTF_float_int:
        case Op::UCVTF_float_int:
        case Op::FCVTAS_float:
        case Op::FCVTAU_float:
        case Op::FCVTPS_float:
        case Op::FCVTPU_float:
        case Op::FCVTMS_float:
        case Op::FCVTMU_float:
        case Op::FCVTZS_float_int:
        case Op::FCVTZU_float_int:
            return FpConvertInteger(cpu, op, word);
        default:
            break;
    }
    const std::optional<bool> precision = FpDouble(word);
    if (!precision) {
        return StepResult::kUndefined;
    }
    const bool dbl = *precision;
    const unsigned bytes = FpBytes(dbl);
    const std::uint32_t d = word & 31;
    const std::uint32_t n = (word >> 5) & 31;
    const std::uint32_t m = (word >> 16) & 31;
    const std::uint64_t sign = sem_fp_sign_bit(dbl);
    switch (op) {
        case Op::FMOV_float:
            sem_v_set(&cpu, d, bytes, sem_v_get(&cpu, n, bytes));
            break;
        case Op::FABS_float:
            sem_v_set(&cpu, d, bytes, sem_v_get(&cpu, n, bytes) & ~sign);
            break;
        case Op::FNEG_float:
            sem_v_set(&cpu, d, bytes, sem_v_get(&cpu, n, bytes) ^ sign);
            break;
        case Op::FMOV_float_imm:
            sem_v_set(&cpu, d, bytes, sem_fp_expand_imm(dbl, (word >> 13) & 0xFF));
            break;
        case Op::FCSEL_float: {
            const bool holds = sem_condition_holds(cpu.nzcv, (word >> 12) & 15);
            sem_v_set(&cpu, d, bytes, sem_v_get(&cpu, holds ? n : m, bytes));
            break;
        }
        case Op::FCMP_float:
        case Op::FCMPE_float: {
            const std::uint64_t b = ((word >> 3) & 1) != 0 ? 0 : sem_v_get(&cpu, m, bytes);  // #0.0 form
            cpu.nzcv = sem_fcmp(&cpu, dbl, sem_v_get(&cpu, n, bytes), b, op == Op::FCMPE_float);
            break;
        }
        case Op::FADD_float:
        case Op::FSUB_float:
        case Op::FMUL_float:
        case Op::FDIV_float:
        case Op::FNMUL_float: {
            const unsigned kind = op == Op::FADD_float   ? SEM_FP_ADD
                                  : op == Op::FSUB_float ? SEM_FP_SUB
                                  : op == Op::FDIV_float ? SEM_FP_DIV
                                                         : SEM_FP_MUL;
            std::uint64_t result = sem_fp_binary(&cpu, dbl, kind, sem_v_get(&cpu, n, bytes), sem_v_get(&cpu, m, bytes));
            if (op == Op::FNMUL_float) {
                result ^= sign;  // negated after the multiply, default NaN included
            }
            sem_v_set(&cpu, d, bytes, result);
            break;
        }
        case Op::FSQRT_float:
            sem_v_set(&cpu, d, bytes, sem_fp_sqrt(&cpu, dbl, sem_v_get(&cpu, n, bytes)));
            break;
        case Op::FRINTN_float:
        case Op::FRINTP_float:
        case Op::FRINTM_float:
        case Op::FRINTZ_float:
        case Op::FRINTA_float:
        case Op::FRINTX_float:
        case Op::FRINTI_float: {
            const unsigned rounding = op == Op::FRINTN_float   ? SEM_ROUND_NEAREST
                                      : op == Op::FRINTP_float ? SEM_ROUND_UP
                                      : op == Op::FRINTM_float ? SEM_ROUND_DOWN
                                      : op == Op::FRINTZ_float ? SEM_ROUND_ZERO
                                      : op == Op::FRINTA_float ? SEM_ROUND_AWAY
                                                               : sem_fp_rounding(&cpu);
            sem_v_set(&cpu, d, bytes,
                      sem_fp_round_int(&cpu, dbl, sem_v_get(&cpu, n, bytes), rounding, op == Op::FRINTX_float));
            break;
        }
        case Op::FMAX_float:
        case Op::FMIN_float:
        case Op::FMAXNM_float:
        case Op::FMINNM_float: {
            const bool max = op == Op::FMAX_float || op == Op::FMAXNM_float;
            const bool number = op == Op::FMAXNM_float || op == Op::FMINNM_float;
            sem_v_set(&cpu, d, bytes,
                      sem_fp_max(&cpu, dbl, sem_v_get(&cpu, n, bytes), sem_v_get(&cpu, m, bytes), max, number));
            break;
        }
        case Op::FMADD_float:
        case Op::FMSUB_float:
        case Op::FNMADD_float:
        case Op::FNMSUB_float: {
            // FMSUB: a - n * m; FNMADD: -a - n * m; FNMSUB: -a + n * m. The negations come before
            // the operation, so a NaN operand keeps its flipped sign.
            const bool negate_product = op == Op::FMSUB_float || op == Op::FNMADD_float;
            const bool negate_addend = op == Op::FNMADD_float || op == Op::FNMSUB_float;
            const std::uint64_t addend = sem_v_get(&cpu, (word >> 10) & 31, bytes) ^ (negate_addend ? sign : 0);
            const std::uint64_t a = sem_v_get(&cpu, n, bytes) ^ (negate_product ? sign : 0);
            sem_v_set(&cpu, d, bytes, sem_fp_muladd(&cpu, dbl, addend, a, sem_v_get(&cpu, m, bytes)));
            break;
        }
        case Op::FCCMP_float:
        case Op::FCCMPE_float:
            if (sem_condition_holds(cpu.nzcv, (word >> 12) & 15)) {
                cpu.nzcv =
                    sem_fcmp(&cpu, dbl, sem_v_get(&cpu, n, bytes), sem_v_get(&cpu, m, bytes), op == Op::FCCMPE_float);
            } else {
                cpu.nzcv = (word & 15) << 28;
            }
            break;
        default:
            return StepResult::kNotImplemented;
    }
    return StepResult::kOk;
}

// ---- Advanced SIMD ----

// The element size (log2 of bytes) and index an imm5 field names: its lowest set bit gives the
// size; none in the low four bits is reserved.
struct Imm5 {
    unsigned size;
    unsigned index;
};

std::optional<Imm5> DecodeImm5(std::uint32_t imm5) {
    if ((imm5 & 15) == 0) {
        return std::nullopt;
    }
    const auto size = static_cast<unsigned>(__builtin_ctz(imm5));
    return Imm5{size, imm5 >> (size + 1)};
}

// DUP, SMOV, UMOV, INS.
StepResult SimdCopy(BiwaCpu& cpu, Op op, std::uint32_t word) {
    const bool q = ((word >> 30) & 1) != 0;
    const std::uint32_t n = (word >> 5) & 31;
    const std::uint32_t d = word & 31;
    const std::optional<Imm5> imm5 = DecodeImm5((word >> 16) & 31);
    if (!imm5) {
        return StepResult::kUndefined;
    }
    const unsigned size = imm5->size;
    const unsigned esize = 1u << size;
    switch (op) {
        case Op::DUP_elt_1:
            sem_dup_element(&cpu, false, true, esize, d, n, imm5->index);
            break;
        case Op::DUP_elt_2:
        case Op::DUP_gen:
            if (size == 3 && !q) {
                return StepResult::kUndefined;
            }
            if (op == Op::DUP_gen) {
                sem_dup_general(&cpu, q, esize, d, n);
            } else {
                sem_dup_element(&cpu, q, false, esize, d, n, imm5->index);
            }
            break;
        case Op::SMOV:
            if (size > (q ? 2u : 1u)) {
                return StepResult::kUndefined;
            }
            sem_mov_to_general(&cpu, true, q, esize, d, n, imm5->index);
            break;
        case Op::UMOV:
            if (q ? size != 3 : size > 2) {
                return StepResult::kUndefined;
            }
            sem_mov_to_general(&cpu, false, q, esize, d, n, imm5->index);
            break;
        case Op::INS_gen:
            sem_ins(&cpu, esize, d, imm5->index, sem_x(&cpu, n));
            break;
        default: {  // INS (element): the source index in imm4
            const SemVec src = sem_vread(&cpu, n);
            const unsigned from = ((word >> 11) & 15) >> size;
            sem_ins(&cpu, esize, d, imm5->index, sem_lane(&src, from, esize));
            break;
        }
    }
    return StepResult::kOk;
}

StepResult SimdPermute(BiwaCpu& cpu, Op op, std::uint32_t word) {
    const bool q = ((word >> 30) & 1) != 0;
    const std::uint32_t size = (word >> 22) & 3;
    const std::uint32_t m = (word >> 16) & 31;
    const std::uint32_t n = (word >> 5) & 31;
    const std::uint32_t d = word & 31;
    switch (op) {
        case Op::EXT: {
            const std::uint32_t index = (word >> 11) & 15;
            if (!q && index > 7) {
                return StepResult::kUndefined;
            }
            sem_ext(&cpu, q, d, n, m, index);
            return StepResult::kOk;
        }
        case Op::TBL:
        case Op::TBX:
            sem_tbl(&cpu, q, op == Op::TBX, d, n, ((word >> 13) & 3) + 1, m);
            return StepResult::kOk;
        default:
            break;
    }
    if (size == 3 && !q) {
        return StepResult::kUndefined;
    }
    const unsigned kind = op == Op::UZP1   ? SEM_UZP1
                          : op == Op::UZP2 ? SEM_UZP2
                          : op == Op::TRN1 ? SEM_TRN1
                          : op == Op::TRN2 ? SEM_TRN2
                          : op == Op::ZIP1 ? SEM_ZIP1
                                           : SEM_ZIP2;
    sem_permute(&cpu, kind, q, 1u << size, d, n, m);
    return StepResult::kOk;
}

// MOVI, MVNI, ORR / BIC (immediate), FMOV (vector, immediate); the bitwise forms; REV, CNT, CLS,
// CLZ, NOT, RBIT.
StepResult SimdBitwise(BiwaCpu& cpu, Op op, std::uint32_t word) {
    const bool q = ((word >> 30) & 1) != 0;
    const std::uint32_t size = (word >> 22) & 3;
    const std::uint32_t m = (word >> 16) & 31;
    const std::uint32_t n = (word >> 5) & 31;
    const std::uint32_t d = word & 31;
    switch (op) {
        case Op::MOVI:
        case Op::FMOV_2: {
            // MOVI's pattern also covers FMOV (vector, immediate): cmode 1111 (the decoder tries
            // MOVI first, as dynarmic does).
            const std::uint32_t o = (word >> 29) & 1;
            const std::uint32_t cmode = (word >> 12) & 15;
            if (cmode == 15 && o != 0 && !q) {
                return StepResult::kUndefined;  // FMOV (vector, double) needs Q
            }
            const std::uint32_t imm8 = (((word >> 16) & 7) << 5) | ((word >> 5) & 31);
            sem_movi(&cpu, q, o, cmode, imm8, d);
            return StepResult::kOk;
        }
        case Op::AND_asimd:
        case Op::BIC_asimd_reg:
        case Op::ORR_asimd_reg:
        case Op::ORN_asimd:
        case Op::EOR_asimd:
        case Op::BSL:
        case Op::BIT:
        case Op::BIF: {
            const unsigned kind = op == Op::AND_asimd       ? SEM_VAND
                                  : op == Op::BIC_asimd_reg ? SEM_VBIC
                                  : op == Op::ORR_asimd_reg ? SEM_VORR
                                  : op == Op::ORN_asimd     ? SEM_VORN
                                  : op == Op::EOR_asimd     ? SEM_VEOR
                                  : op == Op::BSL           ? SEM_VBSL
                                  : op == Op::BIT           ? SEM_VBIT
                                                            : SEM_VBIF;
            sem_vlogic(&cpu, kind, q, d, n, m);
            return StepResult::kOk;
        }
        case Op::NOT:
            sem_vbits(&cpu, SEM_VNOT, q, 1, d, n);
            return StepResult::kOk;
        case Op::RBIT_asimd:
            sem_vbits(&cpu, SEM_VRBIT, q, 1, d, n);
            return StepResult::kOk;
        case Op::CNT:
            if (size != 0) {
                return StepResult::kUndefined;
            }
            sem_vbits(&cpu, SEM_VCNT, q, 1, d, n);
            return StepResult::kOk;
        case Op::CLS_asimd:
        case Op::CLZ_asimd:
            if (size == 3) {
                return StepResult::kUndefined;
            }
            sem_vbits(&cpu, op == Op::CLS_asimd ? SEM_VCLS : SEM_VCLZ, q, 1u << size, d, n);
            return StepResult::kOk;
        default: {  // REV16, REV32, REV64: the container must be wider than the lanes
            const unsigned container = op == Op::REV16_asimd ? 2 : op == Op::REV32_asimd ? 4 : 8;
            if ((1u << size) >= container) {
                return StepResult::kUndefined;
            }
            sem_vrev(&cpu, q, 1u << size, container, d, n);
            return StepResult::kOk;
        }
    }
}

StepResult Simd(BiwaCpu& cpu, Op op, std::uint32_t word) {
    switch (op) {
        case Op::DUP_elt_1:
        case Op::DUP_elt_2:
        case Op::DUP_gen:
        case Op::SMOV:
        case Op::UMOV:
        case Op::INS_gen:
        case Op::INS_elt:
            return SimdCopy(cpu, op, word);
        case Op::EXT:
        case Op::TBL:
        case Op::TBX:
        case Op::UZP1:
        case Op::UZP2:
        case Op::TRN1:
        case Op::TRN2:
        case Op::ZIP1:
        case Op::ZIP2:
            return SimdPermute(cpu, op, word);
        case Op::MOVI:
        case Op::FMOV_2:
        case Op::AND_asimd:
        case Op::BIC_asimd_reg:
        case Op::ORR_asimd_reg:
        case Op::ORN_asimd:
        case Op::EOR_asimd:
        case Op::BSL:
        case Op::BIT:
        case Op::BIF:
        case Op::NOT:
        case Op::RBIT_asimd:
        case Op::CNT:
        case Op::CLS_asimd:
        case Op::CLZ_asimd:
        case Op::REV16_asimd:
        case Op::REV32_asimd:
        case Op::REV64_asimd:
            return SimdBitwise(cpu, op, word);
        case Op::FMOV_3:    // half precision (ARMv8.2)
        case Op::SDOT_vec:  // ARMv8.2 and later: dot products, complex numbers, SHA-3, SHA-512, SM3, SM4
        case Op::UDOT_vec:
        case Op::SDOT_elt:
        case Op::UDOT_elt:
        case Op::FCMLA_vec:
        case Op::FCADD_vec:
        case Op::FCMLA_elt:
        case Op::EOR3:
        case Op::BCAX:
        case Op::RAX1:
        case Op::XAR:
        case Op::SHA512H:
        case Op::SHA512H2:
        case Op::SHA512SU0:
        case Op::SHA512SU1:
        case Op::SM3SS1:
        case Op::SM3TT1A:
        case Op::SM3TT1B:
        case Op::SM3TT2A:
        case Op::SM3TT2B:
        case Op::SM3PARTW1:
        case Op::SM3PARTW2:
        case Op::SM4E:
        case Op::SM4EKEY:
            return StepResult::kUndefined;
        default:
            return StepResult::kNotImplemented;
    }
}

StepResult System(BiwaCpu& cpu, Op op, std::uint32_t word) {
    switch (op) {
        case Op::SVC:
            cpu.pc += 4;
            return StepResult::kSupervisorCall;
        case Op::BRK:
            return StepResult::kBreakpoint;
        case Op::HINT:  // every hint Biwa does not name is a NOP on ARMv8.0 (PAC, BTI, ESB, ...)
        case Op::NOP:
        case Op::YIELD:
        case Op::WFE:
        case Op::WFI:
        case Op::SEV:
        case Op::SEVL:
            return StepResult::kOk;
        case Op::DSB:
        case Op::DMB:
            sem_barrier((word >> 8) & 15);
            return StepResult::kOk;
        case Op::ISB:
            return StepResult::kOk;
        case Op::CLREX:
            sem_clrex(&cpu);
            return StepResult::kOk;
        case Op::LDXR:
        case Op::LDAXR:
        case Op::LDXP:
        case Op::LDAXP:
        case Op::STXR:
        case Op::STLXR:
        case Op::STXP:
        case Op::STLXP:
        case Op::LDAR:
        case Op::STLR:
            return Exclusive(cpu, op, word);
        case Op::LDLAR:  // ARMv8.1 (limited ordering regions)
        case Op::STLLR:
            return StepResult::kUndefined;
        case Op::MRS:
            return SystemRegisterAccess(cpu, word, true);
        case Op::MSR_reg:
            return SystemRegisterAccess(cpu, word, false);
        case Op::DC_ZVA:
            sem_dc_zva(sem_x(&cpu, word & 31));
            return StepResult::kOk;
        case Op::DC_CVAC:  // cache maintenance by address: nothing to do on the host
        case Op::DC_CVAU:
        case Op::DC_CIVAC:
        case Op::IC_IVAU:
            return StepResult::kOk;
        case Op::DC_IVAC:  // EL1 only
        case Op::DC_ISW:
        case Op::DC_CSW:
        case Op::DC_CISW:
        case Op::IC_IALLU:
        case Op::IC_IALLUIS:
        case Op::DC_CVAP:  // ARMv8.2
        case Op::CFINV:    // ARMv8.4 and later
        case Op::RMIF:
        case Op::XAFlag:
        case Op::AXFlag:
            return StepResult::kUndefined;
        case Op::FMOV_float:
        case Op::FABS_float:
        case Op::FNEG_float:
        case Op::FMOV_float_imm:
        case Op::FMOV_float_gen:
        case Op::FCSEL_float:
        case Op::FCMP_float:
        case Op::FCMPE_float:
        case Op::FCCMP_float:
        case Op::FCCMPE_float:
        case Op::FADD_float:
        case Op::FSUB_float:
        case Op::FMUL_float:
        case Op::FDIV_float:
        case Op::FNMUL_float:
        case Op::FSQRT_float:
        case Op::FMAX_float:
        case Op::FMIN_float:
        case Op::FMAXNM_float:
        case Op::FMINNM_float:
        case Op::FMADD_float:
        case Op::FMSUB_float:
        case Op::FNMADD_float:
        case Op::FNMSUB_float:
        case Op::FRINTN_float:
        case Op::FRINTP_float:
        case Op::FRINTM_float:
        case Op::FRINTZ_float:
        case Op::FRINTA_float:
        case Op::FRINTX_float:
        case Op::FRINTI_float:
        case Op::FCVT_float:
        case Op::SCVTF_float_fix:
        case Op::UCVTF_float_fix:
        case Op::FCVTZS_float_fix:
        case Op::FCVTZU_float_fix:
        case Op::FCVTNS_float:
        case Op::FCVTNU_float:
        case Op::SCVTF_float_int:
        case Op::UCVTF_float_int:
        case Op::FCVTAS_float:
        case Op::FCVTAU_float:
        case Op::FCVTPS_float:
        case Op::FCVTPU_float:
        case Op::FCVTMS_float:
        case Op::FCVTMU_float:
        case Op::FCVTZS_float_int:
        case Op::FCVTZU_float_int:
            return FloatingPoint(cpu, op, word);
        default: {
            const StepResult result = LoadStore(cpu, op, word);
            return result == StepResult::kNotImplemented ? Simd(cpu, op, word) : result;
        }
    }
}

StepResult Execute(BiwaCpu& cpu, Op op, std::uint32_t word, bool& branched) {
    switch (op) {
        case Op::B_uncond:
        case Op::BL:
        case Op::B_cond:
        case Op::CBZ:
        case Op::CBNZ:
        case Op::TBZ:
        case Op::TBNZ:
        case Op::BR:
        case Op::BLR:
        case Op::RET:
            return Branch(cpu, op, word, branched);
        case Op::ADR:
        case Op::ADRP: {
            const std::uint32_t raw = Field(op, word, 'i');  // immlo (2 bits) then immhi (19 bits)
            const std::uint64_t imm = ((raw & 0x7FFFF) << 2) | (raw >> 19);
            const std::uint32_t d = Field(op, word, 'd');
            if (op == Op::ADR) {
                sem_adr(&cpu, d, SignExtend(imm, 21));
            } else {
                sem_adrp(&cpu, d, SignExtend(imm, 21) * 4096);
            }
            return StepResult::kOk;
        }
        case Op::ADD_imm:
        case Op::ADDS_imm:
        case Op::SUB_imm:
        case Op::SUBS_imm:
            return AddSubImmediate(cpu, op, word);
        case Op::AND_imm:
        case Op::ORR_imm:
        case Op::EOR_imm:
        case Op::ANDS_imm:
            return LogicalImmediate(cpu, op, word);
        case Op::MOVN:
        case Op::MOVZ:
        case Op::MOVK:
            return MoveWide(cpu, op, word);
        case Op::SBFM:
        case Op::BFM:
        case Op::UBFM:
        case Op::ASR_1:
        case Op::ASR_2:
        case Op::SXTB_1:
        case Op::SXTB_2:
        case Op::SXTH_1:
        case Op::SXTH_2:
        case Op::SXTW:
            return Bitfield(cpu, op, word);
        case Op::EXTR:
            return Extract(cpu, word);
        case Op::AND_shift:
        case Op::BIC_shift:
        case Op::ORR_shift:
        case Op::ORN_shift:
        case Op::EOR_shift:
        case Op::EON:
        case Op::ANDS_shift:
        case Op::BICS:
            return LogicalShifted(cpu, op, word);
        case Op::ADD_shift:
        case Op::ADDS_shift:
        case Op::SUB_shift:
        case Op::SUBS_shift:
            return AddSubShifted(cpu, op, word);
        case Op::ADD_ext:
        case Op::ADDS_ext:
        case Op::SUB_ext:
        case Op::SUBS_ext:
            return AddSubExtended(cpu, op, word);
        case Op::RBIT_int:
        case Op::REV16_int:
        case Op::REV:
        case Op::REV32_int:
        case Op::CLZ_int:
        case Op::CLS_int:
            return OneSource(cpu, op, word);
        case Op::UDIV:
        case Op::SDIV:
        case Op::LSLV:
        case Op::LSRV:
        case Op::ASRV:
        case Op::RORV:
            return TwoSource(cpu, op, word);
        case Op::CRC32:
        case Op::CRC32C:
            return Crc32(cpu, op, word);
        case Op::ADC:
        case Op::ADCS:
        case Op::SBC:
        case Op::SBCS:
            return WithCarry(cpu, op, word);
        case Op::CCMN_reg:
        case Op::CCMP_reg:
        case Op::CCMN_imm:
        case Op::CCMP_imm:
            return ConditionalCompare(cpu, op, word);
        case Op::CSEL:
        case Op::CSINC:
        case Op::CSINV:
        case Op::CSNEG:
            return ConditionalSelect(cpu, op, word);
        case Op::MADD:
        case Op::MSUB:
        case Op::SMADDL:
        case Op::SMSUBL:
        case Op::SMULH:
        case Op::UMADDL:
        case Op::UMSUBL:
        case Op::UMULH:
            return ThreeSource(cpu, op, word);
        default:
            return System(cpu, op, word);
    }
}

}  // namespace

StepResult Step(BiwaCpu& cpu, std::uint32_t word) {
    const std::optional<Op> op = Decode(word);
    if (!op) {
        return StepResult::kUndefined;
    }
    bool branched = false;
    const StepResult result = Execute(cpu, *op, word, branched);
    if (result == StepResult::kOk && !branched) {
        cpu.pc += 4;
    }
    return result;
}

}  // namespace biwa::a64
