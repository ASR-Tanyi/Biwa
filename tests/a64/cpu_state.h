// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <array>
#include <cstdint>
#include <random>
#include <string>

#include "biwa/a64/cpu.h"

namespace biwa::test {

// The A64 state the CPU tests compare: what an EL0 instruction can read or change.
struct CpuState {
    std::array<std::uint64_t, 31> x{};
    std::uint64_t sp = 0;
    std::uint64_t pc = 0;
    std::uint32_t nzcv = 0;  // N, Z, C, V in bits 31..28
    std::uint32_t fpcr = 0;
    std::uint32_t fpsr = 0;
    std::array<std::array<std::uint8_t, 16>, 32> v{};
    std::uint64_t tpidr_el0 = 0;
    std::uint64_t tpidrro_el0 = 0;

    bool operator==(const CpuState&) const = default;
};

// Random registers, flags and FPCR settings (rounding mode, flush-to-zero, default NaN, AHP).
// Integer registers sometimes hold small or edge values (0, -1, sign bits) so flags and carries are
// exercised; vector registers sometimes hold FP edge values (NaNs, infinities, denormals); FPSR
// sometimes has flags raised. `sp` and `pc` are left to the caller.
CpuState RandomState(std::mt19937_64& rng);

// Between the test state and the runtime's BiwaCpu (the exclusive monitor and thread stay clear).
BiwaCpu ToBiwa(const CpuState& state);
CpuState FromBiwa(const BiwaCpu& cpu);

// The registers that differ, one per line ("x3: 0x... != 0x..."); empty when equal.
std::string Diff(const CpuState& expected, const CpuState& actual);

}  // namespace biwa::test
