// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/a64/interpreter.h"

#include <gtest/gtest.h>

#include <cstdint>

#include "biwa/a64/a64_sem.h"

extern "C" int biwa_test_sem_in_c(void);

namespace {

using biwa::a64::Step;
using biwa::a64::StepResult;

BiwaCpu Cpu() {
    BiwaCpu cpu{};
    cpu.pc = 0x1000;
    cpu.x[1] = 0x123456789ABCDEF0;
    cpu.x[2] = 0x0F0F0F0F0F0F0F0F;
    return cpu;
}

TEST(A64InterpreterTest, RunsDataProcessingImmediate) {
    BiwaCpu cpu = Cpu();
    ASSERT_EQ(Step(cpu, 0x92401C20), StepResult::kOk);  // and x0, x1, #0xff
    EXPECT_EQ(cpu.x[0], 0xF0u);
    EXPECT_EQ(cpu.pc, 0x1004u);
    ASSERT_EQ(Step(cpu, 0xD2824680), StepResult::kOk);  // movz x0, #0x1234
    ASSERT_EQ(Step(cpu, 0xF2A00020), StepResult::kOk);  // movk x0, #1, lsl #16
    EXPECT_EQ(cpu.x[0], 0x11234u);
    ASSERT_EQ(Step(cpu, 0xD3441C20), StepResult::kOk);  // ubfx x0, x1, #4, #4
    EXPECT_EQ(cpu.x[0], 0xFu);
    ASSERT_EQ(Step(cpu, 0x93C10840), StepResult::kOk);  // extr x0, x2, x1, #2
    EXPECT_EQ(cpu.x[0], (cpu.x[1] >> 2) | (cpu.x[2] << 62));
    cpu.x[1] = ~std::uint64_t{0};
    ASSERT_EQ(Step(cpu, 0xB1000420), StepResult::kOk);  // adds x0, x1, #1
    EXPECT_EQ(cpu.x[0], 0u);
    EXPECT_EQ(cpu.nzcv, SEM_FLAG_Z | SEM_FLAG_C);
    ASSERT_EQ(Step(cpu, 0x10000080), StepResult::kOk);  // adr x0, .+16 (at 0x1018)
    EXPECT_EQ(cpu.x[0], 0x1028u);
}

TEST(A64InterpreterTest, RejectsWhatItCannotRun) {
    BiwaCpu cpu = Cpu();
    EXPECT_EQ(Step(cpu, 0x91800000), StepResult::kUndefined);       // add with a reserved shift
    EXPECT_EQ(Step(cpu, 0x00000000), StepResult::kUndefined);       // udf
    EXPECT_EQ(Step(cpu, 0xD65F03C0), StepResult::kNotImplemented);  // ret, not yet
    EXPECT_EQ(cpu.pc, 0x1000u);
}

TEST(A64InterpreterTest, TheSemanticsCompileAsC) { EXPECT_EQ(biwa_test_sem_in_c(), 0); }

}  // namespace
