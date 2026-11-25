// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "a64/cpu_state.h"

#include <format>

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

}  // namespace

CpuState RandomState(std::mt19937_64& rng) {
    CpuState state;
    for (std::uint64_t& x : state.x) {
        x = RandomValue(rng);
    }
    state.nzcv = static_cast<std::uint32_t>(rng() & 0xF) << 28;
    // AHP (26), DN (25), FZ (24), RMode (23..22); the trap enables stay clear as on the console.
    state.fpcr = static_cast<std::uint32_t>(rng() & 0x1F) << 22;
    for (auto& lane : state.v) {
        for (std::size_t i = 0; i < lane.size(); i += 8) {
            const std::uint64_t value = RandomValue(rng);
            for (std::size_t b = 0; b < 8; ++b) {
                lane[i + b] = static_cast<std::uint8_t>(value >> (8 * b));
            }
        }
    }
    state.tpidr_el0 = rng();
    state.tpidrro_el0 = rng();
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
