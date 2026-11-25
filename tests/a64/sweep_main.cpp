// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

// a64_sweep: checks Biwa's decoder against Capstone over all 2^32 encodings (or a range) on
// several threads, and prints what differs. Not run by ctest: it takes minutes.
//
//   a64_sweep [threads] [first] [count]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <thread>
#include <vector>

#include "a64/sweep.h"

namespace {

int Run(int argc, char** argv) {
    unsigned threads = argc > 1 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 0))
                                : std::max(1u, std::thread::hardware_concurrency());
    threads = std::clamp(threads, 1u, 256u);
    const std::uint64_t first = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 0;
    const std::uint64_t count = argc > 3 ? std::strtoull(argv[3], nullptr, 0) : std::uint64_t{1} << 32;
    std::vector<biwa::test::SweepResult> results(threads);
    std::vector<std::thread> pool;
    pool.reserve(threads);
    const std::uint64_t part = (count + threads - 1) / threads;
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            const std::uint64_t begin = first + std::uint64_t{t} * part;
            const std::uint64_t end = std::min(first + count, begin + part);
            if (begin < end) {
                results[t] = biwa::test::Sweep(begin, end - begin,
                                               [](std::uint64_t i) { return static_cast<std::uint32_t>(i); });
            }
        });
    }
    for (std::thread& thread : pool) {
        thread.join();
    }
    biwa::test::SweepResult total;
    for (const biwa::test::SweepResult& result : results) {
        total.Merge(result);
    }
    std::printf("checked %llu, Capstone decodes %llu, both %llu, Biwa only %llu\n",
                static_cast<unsigned long long>(total.checked), static_cast<unsigned long long>(total.capstone_decoded),
                static_cast<unsigned long long>(total.both), static_cast<unsigned long long>(total.biwa_only));
    std::printf("Capstone only, not supported by Biwa:\n");
    for (const auto& [reason, n] : total.not_supported) {
        std::printf("  %-52s %10llu\n", reason.c_str(), static_cast<unsigned long long>(n));
    }
    std::printf("missing in Biwa (%zu mnemonics):\n", total.missing.size());
    for (const auto& [mnemonic, entry] : total.missing) {
        std::printf("  %-14s %10llu  e.g. %08x\n", mnemonic.c_str(), static_cast<unsigned long long>(entry.first),
                    entry.second);
    }
    std::printf("only in Biwa (%zu forms):\n", total.extra.size());
    for (const auto& [form, entry] : total.extra) {
        std::printf("  %-24s %10llu  e.g. %08x\n", form.c_str(), static_cast<unsigned long long>(entry.first),
                    entry.second);
    }
    return total.missing.empty() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return Run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "a64_sweep: %s\n", e.what());
        return 2;
    }
}
