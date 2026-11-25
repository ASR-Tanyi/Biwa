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
