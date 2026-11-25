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

StepResult Execute(BiwaCpu& cpu, Op op, std::uint32_t word) {
    switch (op) {
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
            return StepResult::kNotImplemented;
    }
}

}  // namespace

StepResult Step(BiwaCpu& cpu, std::uint32_t word) {
    const std::optional<Op> op = Decode(word);
    if (!op) {
        return StepResult::kUndefined;
    }
    const StepResult result = Execute(cpu, *op, word);
    if (result == StepResult::kOk) {
        cpu.pc += 4;
    }
    return result;
}

}  // namespace biwa::a64
