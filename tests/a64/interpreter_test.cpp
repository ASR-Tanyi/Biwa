// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/a64/interpreter.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

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

TEST(A64InterpreterTest, RunsLoadsAndStores) {
    // Guest addresses are host addresses: point the base register into a local buffer.
    alignas(16) std::array<std::uint8_t, 256> memory{};
    const auto at = [&](std::size_t offset) { return reinterpret_cast<std::uintptr_t>(&memory[offset]); };
    BiwaCpu cpu = Cpu();
    cpu.x[10] = at(64);
    ASSERT_EQ(Step(cpu, 0xF9000541), StepResult::kOk);  // str x1, [x10, #8]
    std::uint64_t stored = 0;
    std::memcpy(&stored, &memory[72], 8);
    EXPECT_EQ(stored, cpu.x[1]);
    ASSERT_EQ(Step(cpu, 0xB9400940), StepResult::kOk);  // ldr w0, [x10, #8]
    EXPECT_EQ(cpu.x[0], 0x9ABCDEF0u);
    ASSERT_EQ(Step(cpu, 0x39802140), StepResult::kOk);  // ldrsb x0, [x10, #8]
    EXPECT_EQ(cpu.x[0], 0xFFFFFFFFFFFFFFF0u);
    ASSERT_EQ(Step(cpu, 0x79C01140), StepResult::kOk);  // ldrsh w0, [x10, #8]
    EXPECT_EQ(cpu.x[0], 0xFFFFDEF0u);
    ASSERT_EQ(Step(cpu, 0xA9BF0941), StepResult::kOk);  // stp x1, x2, [x10, #-16]!
    EXPECT_EQ(cpu.x[10], at(48));
    ASSERT_EQ(Step(cpu, 0xA8C11143), StepResult::kOk);  // ldp x3, x4, [x10], #16
    EXPECT_EQ(cpu.x[3], cpu.x[1]);
    EXPECT_EQ(cpu.x[4], cpu.x[2]);
    EXPECT_EQ(cpu.x[10], at(64));
    ASSERT_EQ(Step(cpu, 0x697E0D40), StepResult::kOk);  // ldpsw x0, x3, [x10, #-16]
    EXPECT_EQ(cpu.x[0], 0xFFFFFFFF9ABCDEF0u);
    EXPECT_EQ(cpu.x[3], 0x12345678u);
    for (std::uint8_t i = 0; i < 16; ++i) {
        cpu.v[5][i] = static_cast<std::uint8_t>(0xA0 + i);
        cpu.v[6][i] = 0xFF;
    }
    cpu.x[11] = 2;
    ASSERT_EQ(Step(cpu, 0x3CAB7945), StepResult::kOk);  // str q5, [x10, x11, lsl #4]
    EXPECT_EQ(std::memcmp(&memory[96], cpu.v[5], 16), 0);
    cpu.x[11] = 4;
    ASSERT_EQ(Step(cpu, 0xFC6B7946), StepResult::kOk);  // ldr d6, [x10, x11, lsl #3]
    EXPECT_EQ(std::memcmp(cpu.v[6], cpu.v[5], 8), 0);
    EXPECT_EQ(cpu.v[6][8], 0);  // the rest of the register is cleared
    EXPECT_EQ(cpu.v[6][15], 0);
    ASSERT_EQ(Step(cpu, 0xB85FF140), StepResult::kOk);  // ldur w0, [x10, #-1] (unaligned)
    EXPECT_EQ(cpu.x[0], 0x0Fu);                         // the top byte of x2, stored by the stp
    ASSERT_EQ(Step(cpu, 0xF9800140), StepResult::kOk);  // prfm pldl1keep, [x10]
    cpu.x[12] = 0xDEADBEEFFFFFFFFE;                     // w12 = -2
    ASSERT_EQ(Step(cpu, 0xF86CD940), StepResult::kOk);  // ldr x0, [x10, w12, sxtw #3]
    EXPECT_EQ(cpu.x[0], cpu.x[1]);
    cpu.pc = at(128);
    const std::uint64_t literal = 0x1122334455667788;
    std::memcpy(&memory[144], &literal, 8);
    ASSERT_EQ(Step(cpu, 0x58000080), StepResult::kOk);  // ldr x0, .+16
    EXPECT_EQ(cpu.x[0], literal);
    EXPECT_EQ(Step(cpu, 0xF8620820), StepResult::kUndefined);  // ldr x0, [x1, w2, uxtb]: reserved
}

TEST(A64InterpreterTest, RunsStructureLoadsAndStores) {
    alignas(16) std::array<std::uint32_t, 32> memory{};
    for (std::uint32_t i = 0; i < 8; ++i) {
        memory[i] = i;
    }
    memory[8] = 0x5A;
    const auto lane = [](const BiwaCpu& cpu, unsigned v, unsigned i) {
        std::uint32_t value = 0;
        std::memcpy(&value, &cpu.v[v][std::size_t{4} * i], 4);
        return value;
    };
    BiwaCpu cpu = Cpu();
    const std::uint64_t base = reinterpret_cast<std::uintptr_t>(memory.data());
    cpu.x[10] = base;
    ASSERT_EQ(Step(cpu, 0x4CDF8940), StepResult::kOk);  // ld2 {v0.4s, v1.4s}, [x10], #32
    for (unsigned i = 0; i < 4; ++i) {
        EXPECT_EQ(lane(cpu, 0, i), 2 * i);
        EXPECT_EQ(lane(cpu, 1, i), 2 * i + 1);
    }
    EXPECT_EQ(cpu.x[10], base + 32);
    ASSERT_EQ(Step(cpu, 0x0D40C144), StepResult::kOk);  // ld1r {v4.8b}, [x10]
    EXPECT_EQ(lane(cpu, 4, 0), 0x5A5A5A5Au);
    EXPECT_EQ(lane(cpu, 4, 1), 0x5A5A5A5Au);
    EXPECT_EQ(lane(cpu, 4, 2), 0u);
    ASSERT_EQ(Step(cpu, 0x4D009141), StepResult::kOk);  // st1 {v1.s}[3], [x10]
    EXPECT_EQ(memory[8], 7u);
    cpu.x[11] = 100;
    ASSERT_EQ(Step(cpu, 0x4CCBAD42), StepResult::kOk);  // ld1 {v2.2d, v3.2d}, [x10], x11
    EXPECT_EQ(lane(cpu, 2, 0), 7u);
    EXPECT_EQ(cpu.x[10], base + 132);
}

TEST(A64InterpreterTest, RunsBranchesAndSystemInstructions) {
    BiwaCpu cpu = Cpu();
    cpu.nzcv = SEM_FLAG_Z;
    cpu.x[3] = 0xFFFFFFFF00000000;  // w3 = 0
    cpu.x[5] = 0x2000;
    ASSERT_EQ(Step(cpu, 0x14000002), StepResult::kOk);  // b .+8
    EXPECT_EQ(cpu.pc, 0x1008u);
    ASSERT_EQ(Step(cpu, 0x97FFFFFF), StepResult::kOk);  // bl .-4
    EXPECT_EQ(cpu.pc, 0x1004u);
    EXPECT_EQ(cpu.x[30], 0x100Cu);
    ASSERT_EQ(Step(cpu, 0x54000201), StepResult::kOk);  // b.ne .+64 (not taken)
    EXPECT_EQ(cpu.pc, 0x1008u);
    ASSERT_EQ(Step(cpu, 0x34000083), StepResult::kOk);  // cbz w3, .+16
    EXPECT_EQ(cpu.pc, 0x1018u);
    ASSERT_EQ(Step(cpu, 0xB7E00101), StepResult::kOk);  // tbnz x1, #60, .+32
    EXPECT_EQ(cpu.pc, 0x1038u);
    ASSERT_EQ(Step(cpu, 0xD63F00A0), StepResult::kOk);  // blr x5
    EXPECT_EQ(cpu.pc, 0x2000u);
    EXPECT_EQ(cpu.x[30], 0x103Cu);
    ASSERT_EQ(Step(cpu, 0xD65F03C0), StepResult::kOk);  // ret
    EXPECT_EQ(cpu.pc, 0x103Cu);
    ASSERT_EQ(Step(cpu, 0xD53B0020), StepResult::kOk);  // mrs x0, ctr_el0
    EXPECT_EQ(cpu.x[0], 0x8444C004u);
    cpu.x[6] = 0xFFFFFFFF;
    ASSERT_EQ(Step(cpu, 0xD51B4406), StepResult::kOk);  // msr fpcr, x6
    ASSERT_EQ(Step(cpu, 0xD53B4400), StepResult::kOk);  // mrs x0, fpcr
    EXPECT_EQ(cpu.x[0], 0x07F70000u);                   // no trap enables on the A57
    const std::uint64_t pc = cpu.pc;
    EXPECT_EQ(Step(cpu, 0xD40004C1), StepResult::kSupervisorCall);  // svc #0x26
    EXPECT_EQ(cpu.pc, pc + 4);
    EXPECT_EQ(Step(cpu, 0xD4200020), StepResult::kBreakpoint);  // brk #1
    EXPECT_EQ(cpu.pc, pc + 4);
    alignas(64) std::array<std::uint8_t, 192> memory{};
    memory.fill(0xEE);
    cpu.x[7] = reinterpret_cast<std::uintptr_t>(&memory[64 + 6]);
    ASSERT_EQ(Step(cpu, 0xD50B7427), StepResult::kOk);  // dc zva, x7
    EXPECT_EQ(memory[63], 0xEE);
    EXPECT_EQ(memory[64], 0);
    EXPECT_EQ(memory[127], 0);
    EXPECT_EQ(memory[128], 0xEE);
    EXPECT_EQ(Step(cpu, 0xD500401F), StepResult::kUndefined);  // cfinv: ARMv8.4
}

TEST(A64InterpreterTest, RejectsWhatItCannotRun) {
    BiwaCpu cpu = Cpu();
    EXPECT_EQ(Step(cpu, 0x91800000), StepResult::kUndefined);       // add with a reserved shift
    EXPECT_EQ(Step(cpu, 0x8BC20020), StepResult::kUndefined);       // add (shifted) with ror
    EXPECT_EQ(Step(cpu, 0x5AC00C20), StepResult::kUndefined);       // rev with the 64-bit opc on w
    EXPECT_EQ(Step(cpu, 0x00000000), StepResult::kUndefined);       // udf
    EXPECT_EQ(Step(cpu, 0xC85F7C20), StepResult::kNotImplemented);  // ldxr x0, [x1], not yet
    EXPECT_EQ(cpu.pc, 0x1000u);
}

TEST(A64InterpreterTest, TheSemanticsCompileAsC) { EXPECT_EQ(biwa_test_sem_in_c(), 0); }

}  // namespace
