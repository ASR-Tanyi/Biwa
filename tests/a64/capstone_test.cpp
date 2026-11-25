// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>

#include "a64/sweep.h"

namespace {

// A spread-out sample of the 2^32 encodings: an odd multiplier visits distinct words. The whole
// space is checked by the a64_sweep tool.
std::uint32_t Sample(std::uint64_t i) { return static_cast<std::uint32_t>(i * 0x9E3779B1u); }

TEST(A64CapstoneTest, EveryEncodingCapstoneDecodesDecodesInBiwa) {
    const biwa::test::SweepResult result = biwa::test::Sweep(0, std::uint64_t{1} << 20, Sample);
    std::ostringstream missing;
    for (const auto& [mnemonic, entry] : result.missing) {
        missing << mnemonic << " x" << entry.first << " (e.g. " << std::hex << entry.second << std::dec << ")\n";
    }
    EXPECT_TRUE(result.missing.empty()) << missing.str();
    EXPECT_GT(result.both, result.checked / 4);  // the sample really reaches instructions
}

}  // namespace
