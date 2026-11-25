// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/a64/decoder.h"

#include <algorithm>
#include <array>
#include <bit>
#include <vector>

namespace biwa::a64 {
namespace {

struct Entry {
    std::string_view id;
    std::string_view name;
    std::string_view pattern;
};

constexpr std::array<Entry, kOpCount> kEntries = {{
#define BIWA_A64_OP(id, name, pattern) {#id, name, pattern},
#include "biwa/a64/ops.inc"
#undef BIWA_A64_OP
}};

// Patterns that must be tried before more specific-looking ones: the modified-immediate forms
// overlap their own unallocated pattern.
constexpr std::array<std::string_view, 3> kFirst = {"MOVI, MVNI, ORR, BIC (vector, immediate)",
                                                    "FMOV (vector, immediate)", "Unallocated SIMD modified immediate"};

// Words are looked up first by 12 of their bits (29..22 and 13..10), which split the encoding
// space well; each bucket lists the patterns that can match there, most specific first.
constexpr std::uint32_t kBucketMask = 0x3FC03C00;
constexpr std::size_t BucketOf(std::uint32_t word) { return ((word >> 18) & 0xFF0) | ((word >> 10) & 0xF); }

struct FieldRun {
    std::uint8_t shift = 0;
    std::uint8_t width = 0;
};

struct FieldSpec {
    char letter = 0;
    std::vector<FieldRun> runs;  // most significant first
};

struct Table {
    std::array<OpInfo, kOpCount> infos{};
    std::array<std::vector<FieldSpec>, kOpCount> fields{};
    std::array<std::vector<Op>, 4096> buckets{};
};

Table Build() {
    Table table;
    for (std::size_t i = 0; i < kOpCount; ++i) {
        const Entry& entry = kEntries[i];
        OpInfo& info = table.infos[i];
        info.id = entry.id;
        info.name = entry.name;
        info.pattern = entry.pattern;
        info.unallocated = entry.id.starts_with("UnallocatedEncoding");
        std::vector<FieldSpec>& fields = table.fields[i];
        for (std::size_t p = 0; p < 32; ++p) {
            const char c = entry.pattern[p];
            const auto bit = static_cast<std::uint8_t>(31 - p);
            if (c == '0' || c == '1') {
                info.mask |= 1u << bit;
                info.value |= (c == '1' ? 1u : 0u) << bit;
                continue;
            }
            if (c == '-') {
                continue;
            }
            auto spec = std::ranges::find(fields, c, &FieldSpec::letter);
            if (spec == fields.end()) {
                fields.push_back({c, {}});
                spec = fields.end() - 1;
            }
            // Extend the current run if this bit continues it, or start a new one.
            const bool continues = p > 0 && entry.pattern[p - 1] == c;
            if (continues) {
                FieldRun& run = spec->runs.back();
                run.shift = bit;
                ++run.width;
            } else {
                spec->runs.push_back({bit, 1});
            }
        }
    }

    std::vector<Op> order(kOpCount);
    for (std::size_t i = 0; i < kOpCount; ++i) {
        order[i] = static_cast<Op>(i);
    }
    auto rank = [&](Op op) {
        const OpInfo& info = table.infos[static_cast<std::size_t>(op)];
        const bool first = std::ranges::find(kFirst, info.name) != kFirst.end();
        return first ? 33 : std::popcount(info.mask);
    };
    std::ranges::stable_sort(order, [&](Op a, Op b) { return rank(a) > rank(b); });
    for (std::size_t bucket = 0; bucket < table.buckets.size(); ++bucket) {
        // The word bits this bucket stands for, at their places.
        const std::uint32_t bits =
            ((static_cast<std::uint32_t>(bucket) & 0xFF0) << 18) | ((static_cast<std::uint32_t>(bucket) & 0xF) << 10);
        for (const Op op : order) {
            const OpInfo& info = table.infos[static_cast<std::size_t>(op)];
            const std::uint32_t mask = info.mask & kBucketMask;
            if ((bits & mask) == (info.value & mask)) {
                table.buckets[bucket].push_back(op);
            }
        }
    }
    return table;
}

const Table& GetTable() {
    static const Table table = Build();
    return table;
}

}  // namespace

const OpInfo& Info(Op op) { return GetTable().infos[static_cast<std::size_t>(op)]; }

std::optional<Op> Decode(std::uint32_t word) {
    const Table& table = GetTable();
    for (const Op op : table.buckets[BucketOf(word)]) {
        const OpInfo& info = table.infos[static_cast<std::size_t>(op)];
        if ((word & info.mask) == info.value) {
            if (info.unallocated) {
                return std::nullopt;
            }
            return op;
        }
    }
    return std::nullopt;
}

std::uint32_t Field(Op op, std::uint32_t word, char letter) {
    const std::vector<FieldSpec>& fields = GetTable().fields[static_cast<std::size_t>(op)];
    const auto spec = std::ranges::find(fields, letter, &FieldSpec::letter);
    if (spec == fields.end()) {
        return 0;
    }
    std::uint32_t value = 0;
    for (const FieldRun& run : spec->runs) {
        const std::uint32_t bits = (word >> run.shift) & ((1u << run.width) - 1u);
        value = (value << run.width) | bits;
    }
    return value;
}

}  // namespace biwa::a64
