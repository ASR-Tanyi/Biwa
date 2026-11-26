// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "a64/cpu_state.h"

#include <algorithm>
#include <bit>
#include <format>
#include <iterator>

namespace biwa::test {
namespace {

std::uint64_t RandomValue(std::mt19937_64& rng) {
    static constexpr std::array<std::uint64_t, 8> kEdges = {
        0,          1,          ~std::uint64_t{0}, std::uint64_t{1} << 63, (std::uint64_t{1} << 63) - 1,
        0x80000000, 0x7FFFFFFF, 0xFFFFFFFF};
    switch (rng() % 8) {
        case 0:
            return kEdges[rng() % kEdges.size()];
        case 1:
            return rng() & 0xFF;
        default:
            return rng();
    }
}

// Floating-point values worth hitting: zeros, infinities, quiet and signalling NaNs with payloads,
// denormals, the normal range's ends, small integers and halves; else random bits.
std::uint64_t RandomDouble(std::mt19937_64& rng) {
    const std::uint64_t sign = (rng() & 1) << 63;
    const std::uint64_t frac = rng() & 0xFFFFFFFFFFFFF;
    switch (rng() % 12) {
        case 0:
            return sign;  // zero
        case 1:
            return sign | 0x7FF0000000000000;  // infinity
        case 2:
            return sign | 0x7FF8000000000000 | (frac >> 1);  // quiet NaN
        case 3:
            return sign | 0x7FF0000000000000 | ((frac >> 1) | 1);  // signalling NaN
        case 4:
            return sign | (frac == 0 ? 1 : frac);  // denormal
        case 5:
            return sign | 0x0010000000000000 | (rng() % 4 == 0 ? 0 : frac & 0xF);  // near the smallest normal
        case 6:
            return sign | 0x7FEFFFFFFFFFFFFF;  // largest normal
        case 7: {
            static constexpr std::array<double, 6> kSmall = {1.0, 2.0, 0.5, 1.5, 3.0, 100.0};
            return std::bit_cast<std::uint64_t>(kSmall[rng() % kSmall.size()]) | sign;
        }
        default:
            return rng();
    }
}

std::uint32_t RandomFloat(std::mt19937_64& rng) {
    const std::uint32_t sign = static_cast<std::uint32_t>(rng() & 1) << 31;
    const auto frac = static_cast<std::uint32_t>(rng() & 0x7FFFFF);
    switch (rng() % 12) {
        case 0:
            return sign;
        case 1:
            return sign | 0x7F800000;
        case 2:
            return sign | 0x7FC00000 | (frac >> 1);
        case 3:
            return sign | 0x7F800000 | ((frac >> 1) | 1);
        case 4:
            return sign | (frac == 0 ? 1 : frac);
        case 5:
            return sign | 0x00800000 | (rng() % 4 == 0 ? 0 : frac & 0xF);
        case 6:
            return sign | 0x7F7FFFFF;
        case 7: {
            static constexpr std::array<float, 6> kSmall = {1.0F, 2.0F, 0.5F, 1.5F, 3.0F, 100.0F};
            return std::bit_cast<std::uint32_t>(kSmall[rng() % kSmall.size()]) | sign;
        }
        default:
            return static_cast<std::uint32_t>(rng());
    }
}

// Eight bytes of a vector register: integer edges, a double, two singles, or random bits.
std::uint64_t RandomVectorBits(std::mt19937_64& rng) {
    switch (rng() % 4) {
        case 0:
            return RandomValue(rng);
        case 1:
            return RandomDouble(rng);
        case 2:
            return RandomFloat(rng) | (std::uint64_t{RandomFloat(rng)} << 32);
        default:
            return rng();
    }
}

}  // namespace

CpuState RandomState(std::mt19937_64& rng) {
    CpuState state;
    for (std::uint64_t& x : state.x) {
        x = RandomValue(rng);
    }
    state.nzcv = static_cast<std::uint32_t>(rng() & 0xF) << 28;
    // AHP (26), DN (25), FZ (24), RMode (23..22); the trap enables stay clear as on the console.
    state.fpcr = static_cast<std::uint32_t>(rng() & 0x1F) << 22;
    // Cumulative exception flags (IOC, DZC, OFC, UFC, IXC, IDC) already raised, sometimes.
    state.fpsr = rng() % 4 == 0 ? static_cast<std::uint32_t>(rng() & 0x9F) : 0;
    for (auto& lane : state.v) {
        for (std::size_t i = 0; i < lane.size(); i += 8) {
            const std::uint64_t value = RandomVectorBits(rng);
            for (std::size_t b = 0; b < 8; ++b) {
                lane[i + b] = static_cast<std::uint8_t>(value >> (8 * b));
            }
        }
    }
    state.tpidr_el0 = rng();
    state.tpidrro_el0 = rng();
    return state;
}

BiwaCpu ToBiwa(const CpuState& state) {
    BiwaCpu cpu{};
    std::ranges::copy(state.x, std::begin(cpu.x));
    cpu.sp = state.sp;
    cpu.pc = state.pc;
    cpu.nzcv = state.nzcv;
    cpu.fpcr = state.fpcr;
    cpu.fpsr = state.fpsr;
    for (std::size_t i = 0; i < state.v.size(); ++i) {
        std::ranges::copy(state.v[i], std::begin(cpu.v[i]));
    }
    cpu.tpidr_el0 = state.tpidr_el0;
    cpu.tpidrro_el0 = state.tpidrro_el0;
    return cpu;
}

CpuState FromBiwa(const BiwaCpu& cpu) {
    CpuState state;
    std::ranges::copy(cpu.x, state.x.begin());
    state.sp = cpu.sp;
    state.pc = cpu.pc;
    state.nzcv = cpu.nzcv;
    state.fpcr = cpu.fpcr;
    state.fpsr = cpu.fpsr;
    for (std::size_t i = 0; i < state.v.size(); ++i) {
        std::ranges::copy(cpu.v[i], state.v[i].begin());
    }
    state.tpidr_el0 = cpu.tpidr_el0;
    state.tpidrro_el0 = cpu.tpidrro_el0;
    return state;
}

std::string Diff(const CpuState& expected, const CpuState& actual) {
    std::string out;
    auto check = [&](std::string_view name, std::uint64_t a, std::uint64_t b) {
        if (a != b) {
            out += std::format("{}: {:#x} != {:#x}\n", name, a, b);
        }
    };
    for (std::size_t i = 0; i < expected.x.size(); ++i) {
        check(std::format("x{}", i), expected.x[i], actual.x[i]);
    }
    check("sp", expected.sp, actual.sp);
    check("pc", expected.pc, actual.pc);
    check("nzcv", expected.nzcv, actual.nzcv);
    check("fpcr", expected.fpcr, actual.fpcr);
    check("fpsr", expected.fpsr, actual.fpsr);
    for (std::size_t i = 0; i < expected.v.size(); ++i) {
        if (expected.v[i] != actual.v[i]) {
            std::string a;
            std::string b;
            for (std::size_t k = 16; k-- > 0;) {
                a += std::format("{:02x}", expected.v[i][k]);
                b += std::format("{:02x}", actual.v[i][k]);
            }
            out += std::format("v{}: {} != {}\n", i, a, b);
        }
    }
    check("tpidr_el0", expected.tpidr_el0, actual.tpidr_el0);
    check("tpidrro_el0", expected.tpidrro_el0, actual.tpidrro_el0);
    return out;
}

}  // namespace biwa::test
