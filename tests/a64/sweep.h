// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace biwa::test {

// Compares Biwa's decoder with Capstone over a set of encodings (docs/design/04-cpu-recompiler.md,
// "Decoder"): every encoding Capstone decodes must decode in Biwa, unless it is an instruction
// Biwa does not support (after ARMv8.0, or not reachable at EL0).
struct SweepResult {
    std::uint64_t checked = 0;
    std::uint64_t capstone_decoded = 0;
    std::uint64_t both = 0;
    // Decoded by Capstone only, and on the not-supported list: count per reason.
    std::map<std::string, std::uint64_t> not_supported;
    std::uint64_t biwa_only = 0;  // decoded by Biwa only: reported, not an error
    // Encodings Capstone decodes and Biwa does not, by Capstone mnemonic: count and one example.
    std::map<std::string, std::pair<std::uint64_t, std::uint32_t>> missing;
    // Encodings only Biwa decodes, by Biwa form: count and one example.
    std::map<std::string, std::pair<std::uint64_t, std::uint32_t>> extra;

    void Merge(const SweepResult& other);
};

// Checks `count` encodings: word(i) for i in [first, first + count).
SweepResult Sweep(std::uint64_t first, std::uint64_t count, const std::function<std::uint32_t(std::uint64_t)>& word);

}  // namespace biwa::test
