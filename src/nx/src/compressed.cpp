// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/compressed.h"

#include <lz4.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "biwa/nx/bucket_tree.h"

namespace biwa::nx {
namespace {

constexpr std::size_t kEntrySize = 0x18;  // virtual offset, stored offset, type (u8), size (s32) at 0x14
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
    const Result<BucketTreeInfo> info = ParseBucketTreeInfo(compression_info, "compressed section");
    if (!info) {
        return std::unexpected(info.error());
    }
    if (info->entry_count == 0) {
        return Fail(Errc::kCorrupt, "compressed section: the bucket tree is empty");
    }
    const Result<BucketTree> tree = ReadBucketTree(*stored, *info, kEntrySize, "compressed section");
    if (!tree) {
        return std::unexpected(tree.error());
    }
    const std::uint64_t table_offset = info->offset;
    const std::uint64_t size = tree->end_offset;

    std::vector<Entry> entries;
    entries.reserve(tree->count());
    for (std::size_t i = 0; i < tree->count(); ++i) {
        const std::span<const std::byte> raw = tree->Entry(i);
        const auto phys = Load<std::int64_t>(raw, 0x08);
        const auto type = Load<std::uint8_t>(raw, 0x10);
        const auto phys_size = Load<std::int32_t>(raw, 0x14);
        if (phys < 0) {
            return Failf(Errc::kCorrupt, "compressed section: bad entry {}", i);
        }
        if (type != 0 && type != 1 && type != 3) {
            return Failf(Errc::kUnsupported, "compressed section: compression type {} is not supported", type);
        }
        // The stored size only matters for LZ4 blocks; other entries have -1 there.
        const std::uint32_t lz4_size = type == 3 && phys_size > 0 ? static_cast<std::uint32_t>(phys_size) : 0;
        entries.push_back(
            {tree->Offset(i), static_cast<std::uint64_t>(phys), lz4_size, static_cast<CompressionType>(type)});
    }
    if (entries.front().virt != 0) {
        return Fail(Errc::kCorrupt, "compressed section: the entries do not start at 0");
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const Entry& entry = entries[i];
        const std::uint64_t end = tree->End(i);
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
    return std::make_shared<CompressedStorage>(std::move(stored), std::move(entries), size);
}

}  // namespace biwa::nx
