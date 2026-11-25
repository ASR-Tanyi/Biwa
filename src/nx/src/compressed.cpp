// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/compressed.h"

#include <lz4.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace biwa::nx {
namespace {

constexpr std::uint64_t kNodeHeaderSize = 0x10;  // index (s32), count (s32), end offset (s64)
constexpr std::uint64_t kEntrySize = 0x18;
constexpr std::uint64_t kEntriesPerSet = (kBucketNodeSize - kNodeHeaderSize) / kEntrySize;
constexpr std::uint64_t kOffsetsPerNode = (kBucketNodeSize - kNodeHeaderSize) / sizeof(std::uint64_t);
constexpr std::uint64_t kMaxBlockSize = std::uint64_t{16} << 20;  // largest LZ4 block Biwa decompresses

struct Entry {
    std::uint64_t virt = 0;
    std::uint64_t phys = 0;
    std::uint32_t phys_size = 0;  // LZ4 blocks only
    CompressionType type = CompressionType::kNone;
};

template <class T>
T Load(std::span<const std::byte> bytes, std::size_t at) {
    T value{};
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return value;
}

std::uint64_t DivideUp(std::uint64_t a, std::uint64_t b) { return (a + b - 1) / b; }

// Size of the bucket tree's offset nodes (the L1 node and any L2 nodes) for `entries` entries.
std::uint64_t NodeStorageSize(std::uint64_t entries) {
    const std::uint64_t sets = DivideUp(entries, kEntriesPerSet);
    std::uint64_t l2 = 0;
    if (sets > kOffsetsPerNode) {
        const std::uint64_t l2_nodes = DivideUp(sets, kOffsetsPerNode);
        l2 = DivideUp(sets - (kOffsetsPerNode - (l2_nodes - 1)), kOffsetsPerNode);
    }
    return (1 + l2) * kBucketNodeSize;
}

class CompressedStorage final : public Storage {
   public:
    CompressedStorage(StoragePtr data, std::vector<Entry> entries, std::uint64_t size)
        : data_(std::move(data)), entries_(std::move(entries)), size_(size) {}

    [[nodiscard]] std::uint64_t Size() const override { return size_; }

    [[nodiscard]] Status Read(std::uint64_t offset, std::span<std::byte> out) const override {
        if (!RangeFits(offset, out.size(), size_)) {
            return ReadPastEnd(offset, out.size(), size_);
        }
        auto it = std::ranges::upper_bound(entries_, offset, {}, &Entry::virt) - 1;
        std::vector<std::byte> packed;
        std::vector<std::byte> unpacked;
        for (std::size_t done = 0; done < out.size(); ++it) {
            const Entry& entry = *it;
            const std::uint64_t end = std::next(it) == entries_.end() ? size_ : std::next(it)->virt;
            const std::uint64_t position = offset + done;
            const std::uint64_t within = position - entry.virt;
            const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(out.size() - done, end - position));
            const std::span<std::byte> piece = out.subspan(done, length);
            switch (entry.type) {
                case CompressionType::kNone:
                    BIWA_RETURN_IF_ERROR(data_->Read(entry.phys + within, piece));
                    break;
                case CompressionType::kZeros:
                    std::ranges::fill(piece, std::byte{0});
                    break;
                case CompressionType::kLz4: {
                    const auto block_size = static_cast<std::size_t>(end - entry.virt);
                    packed.resize(entry.phys_size);
                    BIWA_RETURN_IF_ERROR(data_->Read(entry.phys, packed));
                    const bool whole = within == 0 && length == block_size;
                    if (!whole) {
                        unpacked.resize(block_size);
                    }
                    const std::span<std::byte> target = whole ? piece : std::span<std::byte>(unpacked);
                    const int got = LZ4_decompress_safe(reinterpret_cast<const char*>(packed.data()),
                                                        reinterpret_cast<char*>(target.data()),
                                                        static_cast<int>(packed.size()), static_cast<int>(block_size));
                    if (got < 0 || static_cast<std::size_t>(got) != block_size) {
                        return Failf(Errc::kCorrupt, "compressed section: the LZ4 block at {:#x} is damaged",
                                     entry.virt);
                    }
                    if (!whole) {
                        std::copy_n(unpacked.begin() + static_cast<std::ptrdiff_t>(within), length, piece.begin());
                    }
                    break;
                }
            }
            done += length;
        }
        return {};
    }

   private:
    StoragePtr data_;
    std::vector<Entry> entries_;
    std::uint64_t size_;
};

}  // namespace

Result<StoragePtr> MakeCompressedStorage(StoragePtr stored, const std::array<std::byte, 0x28>& compression_info) {
    // bucket tree: table offset, table size, then the header: "BKTR", version, entry count
    const std::span<const std::byte> info(compression_info);
    const auto table_offset = Load<std::uint64_t>(info, 0x00);
    const auto table_size = Load<std::uint64_t>(info, 0x08);
    const auto magic = Load<std::uint32_t>(info, 0x10);
    const auto version = Load<std::uint32_t>(info, 0x14);
    const auto count = Load<std::int32_t>(info, 0x18);
    if (magic != kBucketMagic || version > 1 || count <= 0) {
        return Fail(Errc::kCorrupt, "compressed section: bad bucket tree header");
    }
    const auto entries_count = static_cast<std::uint64_t>(count);
    const std::uint64_t sets = DivideUp(entries_count, kEntriesPerSet);
    const std::uint64_t node_size = NodeStorageSize(entries_count);
    if (!RangeFits(table_offset, table_size, stored->Size()) || node_size + sets * kBucketNodeSize > table_size) {
        return Fail(Errc::kCorrupt, "compressed section: the bucket tree does not fit");
    }

    std::vector<std::byte> l1(kNodeHeaderSize);
    BIWA_RETURN_IF_ERROR(stored->Read(table_offset, l1));
    const auto size = Load<std::int64_t>(l1, 0x08);
    if (Load<std::int32_t>(l1, 0) != 0 || size <= 0) {
        return Fail(Errc::kCorrupt, "compressed section: bad bucket tree root");
    }

    std::vector<Entry> entries;
    entries.reserve(static_cast<std::size_t>(entries_count));
    std::vector<std::byte> node(kBucketNodeSize);
    for (std::uint64_t set = 0; set < sets; ++set) {
        BIWA_RETURN_IF_ERROR(stored->Read(table_offset + node_size + set * kBucketNodeSize, node));
        const auto index = Load<std::int32_t>(node, 0);
        const auto in_set = Load<std::int32_t>(node, 4);
        if (index < 0 || static_cast<std::uint64_t>(index) != set || in_set <= 0 ||
            static_cast<std::uint64_t>(in_set) > kEntriesPerSet) {
            return Failf(Errc::kCorrupt, "compressed section: bad entry set {}", set);
        }
        for (std::size_t i = 0; i < static_cast<std::size_t>(in_set); ++i) {
            const std::size_t at = kNodeHeaderSize + i * kEntrySize;
            const auto virt = Load<std::int64_t>(node, at);
            const auto phys = Load<std::int64_t>(node, at + 0x08);
            const auto type = Load<std::uint8_t>(node, at + 0x10);
            const auto phys_size = Load<std::int32_t>(node, at + 0x14);
            if (virt < 0 || phys < 0 || (!entries.empty() && static_cast<std::uint64_t>(virt) <= entries.back().virt)) {
                return Failf(Errc::kCorrupt, "compressed section: bad entry {}", entries.size());
            }
            if (type != 0 && type != 1 && type != 3) {
                return Failf(Errc::kUnsupported, "compressed section: compression type {} is not supported", type);
            }
            // The stored size only matters for LZ4 blocks; other entries have -1 there.
            const std::uint32_t lz4_size = type == 3 && phys_size > 0 ? static_cast<std::uint32_t>(phys_size) : 0;
            entries.push_back({static_cast<std::uint64_t>(virt), static_cast<std::uint64_t>(phys), lz4_size,
                               static_cast<CompressionType>(type)});
        }
    }
    if (entries.size() != entries_count || entries.front().virt != 0 ||
        entries.back().virt >= static_cast<std::uint64_t>(size)) {
        return Fail(Errc::kCorrupt, "compressed section: the entries do not cover the section");
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const Entry& entry = entries[i];
        const std::uint64_t end = i + 1 < entries.size() ? entries[i + 1].virt : static_cast<std::uint64_t>(size);
        const std::uint64_t virt_size = end - entry.virt;
        bool bad = false;
        switch (entry.type) {
            case CompressionType::kNone:
                bad = !RangeFits(entry.phys, virt_size, table_offset);
                break;
            case CompressionType::kZeros:
                break;
            case CompressionType::kLz4:
                bad = entry.phys_size == 0 || virt_size > kMaxBlockSize ||
                      !RangeFits(entry.phys, entry.phys_size, table_offset);
                break;
        }
        if (bad) {
            return Failf(Errc::kCorrupt, "compressed section: entry {} (at {:#x}) does not fit", i, entry.virt);
        }
    }
    return std::make_shared<CompressedStorage>(std::move(stored), std::move(entries), static_cast<std::uint64_t>(size));
}

}  // namespace biwa::nx
