// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <lz4.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "support/pfs0_builder.h"

namespace biwa::test {

struct CompressedSection {
    std::vector<std::byte> data;  // stored blocks, then the bucket tree
    std::array<std::byte, 0x28> compression_info{};
    std::vector<std::uint8_t> types;  // per block: 0 stored, 1 zeros, 3 LZ4
};

// Splits `plain` into `block`-byte blocks and stores each as zeros (all-zero blocks), as LZ4 (when
// that is smaller) or as it is, followed by a bucket tree with one entry set.
inline CompressedSection BuildCompressedSection(const std::vector<std::byte>& plain, std::size_t block = 0x1000) {
    struct Entry {
        std::uint64_t virt;
        std::uint64_t phys;
        std::uint8_t type;
        std::uint32_t phys_size;
    };
    CompressedSection out;
    std::vector<Entry> entries;
    for (std::size_t at = 0; at < plain.size(); at += block) {
        const std::size_t size = std::min(block, plain.size() - at);
        const auto* source = reinterpret_cast<const char*>(plain.data() + at);
        out.data.resize((out.data.size() + 15) / 16 * 16);
        Entry entry{at, out.data.size(), 0, static_cast<std::uint32_t>(size)};
        if (std::all_of(plain.begin() + static_cast<std::ptrdiff_t>(at),
                        plain.begin() + static_cast<std::ptrdiff_t>(at + size),
                        [](std::byte b) { return b == std::byte{0}; })) {
            entry = {at, 0, 1, 0};
        } else {
            std::vector<char> packed(static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(size))));
            const int n =
                LZ4_compress_default(source, packed.data(), static_cast<int>(size), static_cast<int>(packed.size()));
            if (n > 0 && static_cast<std::size_t>(n) < size) {
                entry.type = 3;
                entry.phys_size = static_cast<std::uint32_t>(n);
                const auto* bytes = reinterpret_cast<const std::byte*>(packed.data());
                out.data.insert(out.data.end(), bytes, bytes + n);
            } else {
                const auto* bytes = reinterpret_cast<const std::byte*>(source);
                out.data.insert(out.data.end(), bytes, bytes + size);
            }
        }
        if (entry.type != 3) {
            entry.phys_size = 0xFFFFFFFF;  // as real sections store it: only LZ4 blocks have a size
        }
        out.types.push_back(entry.type);
        entries.push_back(entry);
    }

    const std::size_t table = (out.data.size() + 0x1FF) / 0x200 * 0x200;
    out.data.resize(table + 0x8000);
    PutU32(out.data, table + 0, 0);  // L1 node: index, set count, end offset, set start offsets
    PutU32(out.data, table + 4, 1);
    PutU64(out.data, table + 8, plain.size());
    PutU64(out.data, table + 0x10, 0);
    const std::size_t set = table + 0x4000;  // the entry set
    PutU32(out.data, set + 0, 0);
    PutU32(out.data, set + 4, static_cast<std::uint32_t>(entries.size()));
    PutU64(out.data, set + 8, plain.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::size_t at = set + 0x10 + i * 0x18;
        PutU64(out.data, at + 0x00, entries[i].virt);
        PutU64(out.data, at + 0x08, entries[i].phys);
        out.data[at + 0x10] = static_cast<std::byte>(entries[i].type);
        PutU32(out.data, at + 0x14, entries[i].phys_size);
    }

    std::vector<std::byte> info(0x28);
    PutU64(info, 0x00, table);
    PutU64(info, 0x08, 0x8000);
    PutU32(info, 0x10, 0x52544B42);  // "BKTR"
    PutU32(info, 0x14, 1);
    PutU32(info, 0x18, static_cast<std::uint32_t>(entries.size()));
    std::ranges::copy(info, out.compression_info.begin());
    return out;
}

}  // namespace biwa::test
