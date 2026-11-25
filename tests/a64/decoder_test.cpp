// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/a64/decoder.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string_view>

namespace {

using biwa::a64::Decode;
using biwa::a64::Field;
using biwa::a64::Op;

TEST(A64DecoderTest, DecodesCommonInstructions) {
    struct Case {
        std::uint32_t word;
        Op op;
    };
    for (const Case c : {
             Case{0x91000420, Op::ADD_imm},          // add x0, x1, #1
             Case{0x30091A20, Op::ADR},              // adr x0, .+0x12345
             Case{0xD65F03C0, Op::RET},              // ret
             Case{0x14000040, Op::B_uncond},         // b .+0x100
             Case{0xF9400441, Op::STRx_LDRx_imm_2},  // ldr x1, [x2, #8]
             Case{0xD503201F, Op::NOP},              // nop
             Case{0xD4000001, Op::SVC},              // svc #0
             Case{0x4F00E400, Op::MOVI},             // movi v0.16b, #0
             Case{0x1E602820, Op::FADD_float},       // fadd d0, d1, d0
             Case{0xC85F7C20, Op::LDXR},             // ldxr x0, [x1]
             Case{0xD53BD040, Op::MRS},              // mrs x0, tpidr_el0
             Case{0x9B027C20, Op::MADD},             // mul x0, x1, x2
         }) {
        const auto op = Decode(c.word);
        ASSERT_TRUE(op.has_value()) << std::hex << c.word;
        EXPECT_EQ(op.value_or(Op::NOP), c.op)
            << std::hex << c.word << " is " << biwa::a64::Info(op.value_or(Op::NOP)).id;
    }
}

TEST(A64DecoderTest, RejectsUnallocatedEncodings) {
    EXPECT_FALSE(Decode(0x00000000).has_value());
    EXPECT_FALSE(Decode(0xFFFFFFFF).has_value());
}

TEST(A64DecoderTest, ExtractsFields) {
    // add x0, x1, #1
    EXPECT_EQ(Field(Op::ADD_imm, 0x91000420, 'z'), 1u);
    EXPECT_EQ(Field(Op::ADD_imm, 0x91000420, 'i'), 1u);
    EXPECT_EQ(Field(Op::ADD_imm, 0x91000420, 'n'), 1u);
    EXPECT_EQ(Field(Op::ADD_imm, 0x91000420, 'd'), 0u);
    EXPECT_EQ(Field(Op::ADD_imm, 0x91000420, 'Q'), 0u);  // not in the pattern
    // ldr x1, [x2, #8]: size 11, opc 01, imm12 1
    EXPECT_EQ(Field(Op::STRx_LDRx_imm_2, 0xF9400441, 'z'), 3u);
    EXPECT_EQ(Field(Op::STRx_LDRx_imm_2, 0xF9400441, 'o'), 1u);
    EXPECT_EQ(Field(Op::STRx_LDRx_imm_2, 0xF9400441, 't'), 1u);
    // A split field is concatenated in pattern order: ADR gives immlo (bits 30..29) then immhi.
    EXPECT_EQ(Field(Op::ADR, 0x30091A20, 'i'), (1u << 19) | 0x48D1u);
}

TEST(A64DecoderTest, TheTableIsWellFormed) {
    std::set<std::string_view> ids;
    std::size_t unallocated = 0;
    for (std::size_t i = 0; i < biwa::a64::kOpCount; ++i) {
        const biwa::a64::OpInfo& info = biwa::a64::Info(static_cast<Op>(i));
        EXPECT_EQ(info.pattern.size(), 32u) << info.id;
        EXPECT_TRUE(ids.insert(info.id).second) << info.id;
        EXPECT_EQ(info.value & ~info.mask, 0u) << info.id;
        unallocated += info.unallocated ? 1 : 0;
    }
    EXPECT_EQ(biwa::a64::kOpCount, 643u);
    EXPECT_EQ(unallocated, 9u);
}

}  // namespace
