// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

// The semantics, run through the interpreter, against Unicorn: random encodings of each form on
// random states must leave the same state, and encodings Biwa rejects must fault in Unicorn too
// (docs/design/09-testing-and-debugging.md, "Instruction semantics").

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <format>
#include <random>
#include <string>

#include "a64/cpu_state.h"
#include "a64/unicorn_cpu.h"
#include "biwa/a64/decoder.h"
#include "biwa/a64/interpreter.h"

namespace {

using biwa::a64::Op;
using biwa::a64::StepResult;
using biwa::test::CpuState;
using biwa::test::UnicornCpu;

constexpr int kSamples = 2000;

class SemanticsTest : public testing::TestWithParam<Op> {};

TEST_P(SemanticsTest, MatchesUnicorn) {
    const Op op = GetParam();
    const biwa::a64::OpInfo& info = biwa::a64::Info(op);
    UnicornCpu unicorn;
    std::mt19937_64 rng(static_cast<std::uint64_t>(op) * 7919 + 1);
    int run = 0;
    int undefined = 0;
    for (int tries = 0; run < kSamples && tries < kSamples * 50; ++tries) {
        const std::uint32_t word = (static_cast<std::uint32_t>(rng()) & ~info.mask) | info.value;
        if (biwa::a64::Decode(word) != op) {
            continue;  // a more specific form owns this encoding
        }
        CpuState in = biwa::test::RandomState(rng);
        in.sp = rng() & ~std::uint64_t{0xF};
        in.pc = UnicornCpu::kCodeAddress;
        BiwaCpu cpu = biwa::test::ToBiwa(in);
        const StepResult result = biwa::a64::Step(cpu, word);
        const UnicornCpu::Outcome reference = unicorn.Run(in, std::array<std::uint32_t, 1>{word});
        ++run;
        if (result == StepResult::kUndefined) {
            ++undefined;
            EXPECT_FALSE(reference.completed) << std::format("{:08x}: Biwa rejects it, Unicorn runs it", word);
            continue;
        }
        ASSERT_EQ(result, StepResult::kOk) << std::format("{:08x}", word);
        ASSERT_TRUE(reference.completed) << std::format("{:08x}: Unicorn: {}", word, reference.error);
        const std::string diff = biwa::test::Diff(reference.state, biwa::test::FromBiwa(cpu));
        ASSERT_EQ(diff, "") << std::format("{:08x} ({}), Unicorn != Biwa:\n", word, info.name) << diff;
    }
    EXPECT_EQ(run, kSamples);
    EXPECT_LT(undefined, run);
}

std::string Name(const testing::TestParamInfo<Op>& param) { return std::string(biwa::a64::Info(param.param).id); }

// Data processing: immediate.
INSTANTIATE_TEST_SUITE_P(Immediate, SemanticsTest,
                         testing::Values(Op::ADR, Op::ADRP, Op::ADD_imm, Op::ADDS_imm, Op::SUB_imm, Op::SUBS_imm,
                                         Op::AND_imm, Op::ORR_imm, Op::EOR_imm, Op::ANDS_imm, Op::MOVN, Op::MOVZ,
                                         Op::MOVK, Op::SBFM, Op::BFM, Op::UBFM, Op::ASR_1, Op::ASR_2, Op::SXTB_1,
                                         Op::SXTB_2, Op::SXTH_1, Op::SXTH_2, Op::SXTW, Op::EXTR),
                         Name);

// Data processing: register.
INSTANTIATE_TEST_SUITE_P(Register, SemanticsTest,
                         testing::Values(Op::AND_shift, Op::BIC_shift, Op::ORR_shift, Op::ORN_shift, Op::EOR_shift,
                                         Op::EON, Op::ANDS_shift, Op::BICS, Op::ADD_shift, Op::ADDS_shift,
                                         Op::SUB_shift, Op::SUBS_shift, Op::ADD_ext, Op::ADDS_ext, Op::SUB_ext,
                                         Op::SUBS_ext, Op::RBIT_int, Op::REV16_int, Op::REV, Op::REV32_int, Op::CLZ_int,
                                         Op::CLS_int, Op::UDIV, Op::SDIV, Op::LSLV, Op::LSRV, Op::ASRV, Op::RORV,
                                         Op::CRC32, Op::CRC32C, Op::ADC, Op::ADCS, Op::SBC, Op::SBCS, Op::CCMN_reg,
                                         Op::CCMP_reg, Op::CCMN_imm, Op::CCMP_imm, Op::CSEL, Op::CSINC, Op::CSINV,
                                         Op::CSNEG, Op::MADD, Op::MSUB, Op::SMADDL, Op::SMSUBL, Op::SMULH, Op::UMADDL,
                                         Op::UMSUBL, Op::UMULH),
                         Name);

}  // namespace
