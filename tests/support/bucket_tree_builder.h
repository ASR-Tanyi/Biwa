// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "support/pfs0_builder.h"

namespace biwa::test {

// A bucket tree with 0x4000-byte nodes: one L1 node, then entry sets, each as full as it can be.
// Each entry is `entry_size` bytes and starts with its offset (s64).
inline std::vector<std::byte> BuildBucketTree(const std::vector<std::vector<std::byte>>& entries,
                                              std::size_t entry_size, std::uint64_t end_offset) {
    constexpr std::size_t kNode = 0x4000;
    const std::size_t per_set = (kNode - 0x10) / entry_size;
    const std::size_t sets = (entries.size() + per_set - 1) / per_set;
    std::vector<std::byte> out(kNode * (1 + sets));
    auto offset_of = [&](std::size_t i) {
        std::uint64_t offset = 0;
        std::memcpy(&offset, entries[i].data(), sizeof(offset));
        return offset;
    };
    PutU32(out, 0, 0);
    PutU32(out, 4, static_cast<std::uint32_t>(sets));
    PutU64(out, 8, end_offset);
    for (std::size_t set = 0; set < sets; ++set) {
        const std::size_t first = set * per_set;
        const std::size_t count = std::min(per_set, entries.size() - first);
        PutU64(out, 0x10 + set * 8, offset_of(first));
        const std::size_t node = kNode * (1 + set);
        PutU32(out, node + 0, static_cast<std::uint32_t>(set));
        PutU32(out, node + 4, static_cast<std::uint32_t>(count));
        PutU64(out, node + 8, first + count < entries.size() ? offset_of(first + count) : end_offset);
        for (std::size_t i = 0; i < count; ++i) {
            std::copy_n(entries[first + i].begin(), entry_size,
                        out.begin() + static_cast<std::ptrdiff_t>(node + 0x10 + i * entry_size));
        }
    }
    return out;
}

// The 0x20-byte {offset, size, "BKTR", version 1, entry count} that points to a tree.
inline std::array<std::byte, 0x20> BucketTreeInfoBytes(std::uint64_t offset, std::uint64_t size, std::size_t count) {
    std::vector<std::byte> bytes(0x20);
    PutU64(bytes, 0x00, offset);
    PutU64(bytes, 0x08, size);
    PutU32(bytes, 0x10, 0x52544B42);  // "BKTR"
    PutU32(bytes, 0x14, 1);
    PutU32(bytes, 0x18, static_cast<std::uint32_t>(count));
    std::array<std::byte, 0x20> out{};
    std::ranges::copy(bytes, out.begin());
    return out;
}

}  // namespace biwa::test
