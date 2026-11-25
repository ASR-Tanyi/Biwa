// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

// The Unicorn harness itself: states go in and come out as expected for a few known instructions.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <random>

#include "a64/cpu_state.h"
#include "a64/unicorn_cpu.h"

namespace {

using biwa::test::CpuState;
using biwa::test::Diff;
using biwa::test::UnicornCpu;

CpuState Start(std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    CpuState state = biwa::test::RandomState(rng);
    state.sp = UnicornCpu::kDataAddress + 0x8000;
    return state;
}

TEST(UnicornCpuTest, ANopKeepsEveryRegister) {
    UnicornCpu cpu;
    for (std::uint64_t seed = 0; seed < 20; ++seed) {
        const CpuState in = Start(seed);
        const auto out = cpu.Run(in, std::array<std::uint32_t, 1>{0xD503201F});  // nop
        ASSERT_TRUE(out.completed) << out.error;
        CpuState expected = in;
        expected.pc = UnicornCpu::kCodeAddress + 4;
        EXPECT_EQ(Diff(expected, out.state), "") << "seed " << seed;
    }
}

TEST(UnicornCpuTest, RunsArithmeticAndSetsFlags) {
    UnicornCpu cpu;
    CpuState in = Start(1);
    in.x[1] = ~std::uint64_t{0};
    in.x[2] = 1;
    // add x0, x1, #1; adds x5, x1, x2
    const auto out = cpu.Run(in, std::array<std::uint32_t, 2>{0x91000420, 0xAB020025});
    ASSERT_TRUE(out.completed) << out.error;
    CpuState expected = in;
    expected.x[0] = 0;
    expected.x[5] = 0;
    expected.nzcv = 0x6u << 28;  // Z and C
    expected.pc = UnicornCpu::kCodeAddress + 8;
    EXPECT_EQ(Diff(expected, out.state), "");
}

TEST(UnicornCpuTest, LoadsAndStoresData) {
    UnicornCpu cpu;
    CpuState in = Start(2);
    in.x[3] = UnicornCpu::kDataAddress + 0x100;
    // str x2, [x3]; ldr x4, [x3]
    const auto out = cpu.Run(in, std::array<std::uint32_t, 2>{0xF9000062, 0xF9400064});
    ASSERT_TRUE(out.completed) << out.error;
    EXPECT_EQ(out.state.x[4], in.x[2]);
    std::array<std::uint8_t, 8> stored{};
    cpu.ReadData(0x100, stored);
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= std::uint64_t{stored[i]} << (8 * i);
    }
    EXPECT_EQ(value, in.x[2]);
}

TEST(UnicornCpuTest, MovesVectorRegisters) {
    UnicornCpu cpu;
    const CpuState in = Start(3);
    const auto out = cpu.Run(in, std::array<std::uint32_t, 1>{0x4EA11C20});  // mov v0.16b, v1.16b
    ASSERT_TRUE(out.completed) << out.error;
    EXPECT_EQ(out.state.v[0], in.v[1]);
}

TEST(UnicornCpuTest, StopsOnAnUndefinedInstruction) {
    UnicornCpu cpu;
    const auto out = cpu.Run(Start(4), std::array<std::uint32_t, 1>{0x00000000});  // udf #0
    EXPECT_FALSE(out.completed);
    EXPECT_FALSE(out.error.empty());
}

TEST(UnicornCpuTest, RandomStatesVary) {
    const CpuState a = Start(5);
    const CpuState b = Start(6);
    EXPECT_NE(Diff(a, b), "");
    EXPECT_EQ(a.fpcr & ~(0x1Fu << 22), 0u);  // no FP trap enabled
}

}  // namespace
