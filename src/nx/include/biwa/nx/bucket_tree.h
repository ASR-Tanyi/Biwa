// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "biwa/core/error.h"
#include "biwa/nx/storage.h"

namespace biwa::nx {

// Bucket trees map ranges of a virtual storage to entries: compressed sections, and the two
// tables of a patch (BKTR) section. All of them use 0x4000-byte nodes.
inline constexpr std::uint64_t kBucketNodeSize = 0x4000;
inline constexpr std::uint32_t kBucketMagic = 0x52544B42;  // "BKTR"

// Where a tree is: {offset, size, "BKTR", version, entry count, reserved} (0x20 bytes), as stored in
// an FS header's patch and compression info.
struct BucketTreeInfo {
    std::uint64_t offset = 0;  // in the storage the tree is read from
    std::uint64_t size = 0;
    std::uint32_t entry_count = 0;
};

// Errc::kCorrupt, naming `what`, for a wrong magic or version or a negative count.
Result<BucketTreeInfo> ParseBucketTreeInfo(std::span<const std::byte> info, std::string_view what);

// All entries of a tree, in order. Every entry starts with its virtual offset (s64); offsets
// increase, and the last entry's range ends at `end_offset`.
struct BucketTree {
    std::vector<std::byte> entries;
    std::size_t entry_size = 0;
    std::uint64_t end_offset = 0;

    [[nodiscard]] std::size_t count() const { return entry_size == 0 ? 0 : entries.size() / entry_size; }
    [[nodiscard]] std::span<const std::byte> Entry(std::size_t index) const;
    [[nodiscard]] std::uint64_t Offset(std::size_t index) const;
    // Where entry `index`'s range ends: the next entry's offset, or end_offset for the last.
    [[nodiscard]] std::uint64_t End(std::size_t index) const;
    // The entry whose range holds `offset`, or count() if it is before the first entry or at or
    // past end_offset.
    [[nodiscard]] std::size_t Find(std::uint64_t offset) const;
};

// Reads the tree described by `info` from `storage`: the offset node(s), then the entry sets.
// Errc::kCorrupt, naming `what`, if it does not fit or its nodes or offsets are inconsistent.
Result<BucketTree> ReadBucketTree(const Storage& storage, const BucketTreeInfo& info, std::size_t entry_size,
                                  std::string_view what);

}  // namespace biwa::nx
