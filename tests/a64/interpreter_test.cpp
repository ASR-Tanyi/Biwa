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

TEST(A64InterpreterTest, RunsDataProcessingRegister) {
    BiwaCpu cpu = Cpu();
    cpu.sp = 0x8000;
    cpu.x[4] = 0x80000000;
    cpu.x[5] = 0xFFFFFFFF;
    cpu.x[6] = 0xFFFFFFFF;
    cpu.x[7] = 'a';
    cpu.x[0] = 1;
    ASSERT_EQ(Step(cpu, 0x9AC30820), StepResult::kOk);  // udiv x0, x1, x3 (x3 is 0)
    EXPECT_EQ(cpu.x[0], 0u);
    ASSERT_EQ(Step(cpu, 0x1AC50C80), StepResult::kOk);  // sdiv w0, w4, w5 (INT32_MIN / -1)
    EXPECT_EQ(cpu.x[0], 0x80000000u);
    ASSERT_EQ(Step(cpu, 0x1AC740C0), StepResult::kOk);              // crc32b w0, w6, w7
    EXPECT_EQ(~static_cast<std::uint32_t>(cpu.x[0]), 0xE8B7BE43u);  // CRC-32 of "a"
    ASSERT_EQ(Step(cpu, 0x1AC750C0), StepResult::kOk);              // crc32cb w0, w6, w7
    EXPECT_EQ(~static_cast<std::uint32_t>(cpu.x[0]), 0xC1D04330u);  // CRC-32C of "a"
    ASSERT_EQ(Step(cpu, 0xAB021020), StepResult::kOk);              // adds x0, x1, x2, lsl #4
    EXPECT_EQ(cpu.x[0], 0x032547698BADCFE0u);
    EXPECT_EQ(cpu.nzcv, SEM_FLAG_C);
    ASSERT_EQ(Step(cpu, 0x8B214BE0), StepResult::kOk);  // add x0, sp, w1, uxtw #2
    EXPECT_EQ(cpu.x[0], 0x26AF3FBC0u);
    ASSERT_EQ(Step(cpu, 0xFA431825), StepResult::kOk);  // ccmp x1, #3, #0b0101, ne (C only: ne holds)
    EXPECT_EQ(cpu.nzcv, SEM_FLAG_C);                    // x1 - 3: positive, no borrow
    cpu.nzcv = SEM_FLAG_Z;
    ASSERT_EQ(Step(cpu, 0xFA431825), StepResult::kOk);  // ccmp x1, #3, #0b0101, ne (ne fails)
    EXPECT_EQ(cpu.nzcv, SEM_FLAG_Z | SEM_FLAG_V);
    ASSERT_EQ(Step(cpu, 0x9A9F17E0), StepResult::kOk);  // cset x0, eq
    EXPECT_EQ(cpu.x[0], 1u);
    ASSERT_EQ(Step(cpu, 0x5AC017E0), StepResult::kOk);  // cls w0, wzr
    EXPECT_EQ(cpu.x[0], 31u);
    ASSERT_EQ(Step(cpu, 0xDAC013E0), StepResult::kOk);  // clz x0, xzr
    EXPECT_EQ(cpu.x[0], 64u);
    ASSERT_EQ(Step(cpu, 0xDAC00C20), StepResult::kOk);  // rev x0, x1
    EXPECT_EQ(cpu.x[0], 0xF0DEBC9A78563412u);
    ASSERT_EQ(Step(cpu, 0x1AC22C20), StepResult::kOk);  // ror w0, w1, w2 (by 15)
    EXPECT_EQ(cpu.x[0], 0xBDE13579u);
    ASSERT_EQ(Step(cpu, 0x9BC27C20), StepResult::kOk);  // umulh x0, x1, x2
    EXPECT_EQ(cpu.x[0], 0x0112233445566777u);
    ASSERT_EQ(Step(cpu, 0x9B020C20), StepResult::kOk);  // madd x0, x1, x2, x3
    EXPECT_EQ(cpu.x[0], 0x8675645342312010u);
}

TEST(A64InterpreterTest, RejectsWhatItCannotRun) {
    BiwaCpu cpu = Cpu();
    EXPECT_EQ(Step(cpu, 0x91800000), StepResult::kUndefined);       // add with a reserved shift
    EXPECT_EQ(Step(cpu, 0x8BC20020), StepResult::kUndefined);       // add (shifted) with ror
    EXPECT_EQ(Step(cpu, 0x5AC00C20), StepResult::kUndefined);       // rev with the 64-bit opc on w
    EXPECT_EQ(Step(cpu, 0x00000000), StepResult::kUndefined);       // udf
    EXPECT_EQ(Step(cpu, 0xD65F03C0), StepResult::kNotImplemented);  // ret, not yet
    EXPECT_EQ(cpu.pc, 0x1000u);
}

TEST(A64InterpreterTest, TheSemanticsCompileAsC) { EXPECT_EQ(biwa_test_sem_in_c(), 0); }

}  // namespace
