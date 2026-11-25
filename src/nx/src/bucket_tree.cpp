// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/bucket_tree.h"

#include <algorithm>
#include <cstring>
#include <ranges>

namespace biwa::nx {
namespace {

constexpr std::uint64_t kNodeHeaderSize = 0x10;  // index (s32), count (s32), end offset (s64)
constexpr std::uint64_t kOffsetsPerNode = (kBucketNodeSize - kNodeHeaderSize) / sizeof(std::uint64_t);

template <class T>
T Load(std::span<const std::byte> bytes, std::size_t at) {
    T value{};
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return value;
}

std::uint64_t DivideUp(std::uint64_t a, std::uint64_t b) { return (a + b - 1) / b; }

// Size of the offset nodes (the L1 node and any L2 nodes) in front of the entry sets.
std::uint64_t NodeStorageSize(std::uint64_t sets) {
    std::uint64_t l2 = 0;
    if (sets > kOffsetsPerNode) {
        const std::uint64_t l2_nodes = DivideUp(sets, kOffsetsPerNode);
        l2 = DivideUp(sets - (kOffsetsPerNode - (l2_nodes - 1)), kOffsetsPerNode);
    }
    return (1 + l2) * kBucketNodeSize;
}

}  // namespace

Result<BucketTreeInfo> ParseBucketTreeInfo(std::span<const std::byte> info, std::string_view what) {
    if (info.size() < 0x20) {
        return Failf(Errc::kInternal, "{}: bucket tree info is {} bytes", what, info.size());
    }
    const auto magic = Load<std::uint32_t>(info, 0x10);
    const auto version = Load<std::uint32_t>(info, 0x14);
    const auto count = Load<std::int32_t>(info, 0x18);
    if (magic != kBucketMagic || version > 1 || count < 0) {
        return Failf(Errc::kCorrupt, "{}: bad bucket tree header", what);
    }
    return BucketTreeInfo{Load<std::uint64_t>(info, 0x00), Load<std::uint64_t>(info, 0x08),
                          static_cast<std::uint32_t>(count)};
}

std::span<const std::byte> BucketTree::Entry(std::size_t index) const {
    return std::span(entries).subspan(index * entry_size, entry_size);
}

std::uint64_t BucketTree::Offset(std::size_t index) const { return Load<std::uint64_t>(entries, index * entry_size); }

std::uint64_t BucketTree::End(std::size_t index) const { return index + 1 < count() ? Offset(index + 1) : end_offset; }

std::size_t BucketTree::Find(std::uint64_t offset) const {
    if (count() == 0 || offset < Offset(0) || offset >= end_offset) {
        return count();
    }
    const auto indices = std::views::iota(std::size_t{0}, count());
    const auto after = std::ranges::upper_bound(indices, offset, {}, [this](std::size_t i) { return Offset(i); });
    return *after - 1;
}

Result<BucketTree> ReadBucketTree(const Storage& storage, const BucketTreeInfo& info, std::size_t entry_size,
                                  std::string_view what) {
    BucketTree tree;
    tree.entry_size = entry_size;
    if (info.entry_count == 0) {
        return tree;
    }
    const std::uint64_t per_set = (kBucketNodeSize - kNodeHeaderSize) / entry_size;
    const std::uint64_t sets = DivideUp(info.entry_count, per_set);
    const std::uint64_t node_size = NodeStorageSize(sets);
    if (!RangeFits(info.offset, info.size, storage.Size()) || node_size + sets * kBucketNodeSize > info.size) {
        return Failf(Errc::kCorrupt, "{}: the bucket tree does not fit", what);
    }

    std::vector<std::byte> node(kBucketNodeSize);
    BIWA_RETURN_IF_ERROR(storage.Read(info.offset, std::span(node).first(kNodeHeaderSize)));
    const auto end = Load<std::int64_t>(node, 0x08);
    if (Load<std::int32_t>(node, 0) != 0 || end <= 0) {
        return Failf(Errc::kCorrupt, "{}: bad bucket tree root", what);
    }
    tree.end_offset = static_cast<std::uint64_t>(end);

    tree.entries.reserve(static_cast<std::size_t>(info.entry_count * entry_size));
    for (std::uint64_t set = 0; set < sets; ++set) {
        BIWA_RETURN_IF_ERROR(storage.Read(info.offset + node_size + set * kBucketNodeSize, node));
        const auto index = Load<std::int32_t>(node, 0);
        const auto in_set = Load<std::int32_t>(node, 4);
        const std::uint64_t expected = std::min(per_set, info.entry_count - set * per_set);
        if (index < 0 || static_cast<std::uint64_t>(index) != set || in_set < 0 ||
            static_cast<std::uint64_t>(in_set) != expected) {
            return Failf(Errc::kCorrupt, "{}: bad entry set {}", what, set);
        }
        const auto bytes = std::span(node).subspan(kNodeHeaderSize, static_cast<std::size_t>(expected * entry_size));
        tree.entries.insert(tree.entries.end(), bytes.begin(), bytes.end());
    }
    for (std::size_t i = 0; i < tree.count(); ++i) {
        const auto offset = Load<std::int64_t>(tree.entries, i * entry_size);
        if (offset < 0 || (i > 0 && tree.Offset(i) <= tree.Offset(i - 1)) || tree.Offset(i) >= tree.end_offset) {
            return Failf(Errc::kCorrupt, "{}: bucket tree entry {} is out of order", what, i);
        }
    }
    return tree;
}

}  // namespace biwa::nx
