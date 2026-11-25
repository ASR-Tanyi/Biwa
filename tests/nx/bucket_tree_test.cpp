// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/bucket_tree.h"

#include <gtest/gtest.h>

#include <vector>

#include "support/bucket_tree_builder.h"

namespace {

using biwa::Errc;
using biwa::nx::BucketTree;

constexpr std::size_t kEntrySize = 0x10;

// `count` entries of 0x10 bytes at offsets 0, 0x100, 0x200, ..., each holding its index at 0x08.
std::vector<std::vector<std::byte>> Entries(std::size_t count) {
    std::vector<std::vector<std::byte>> entries;
    for (std::size_t i = 0; i < count; ++i) {
        std::vector<std::byte> entry(kEntrySize);
        biwa::test::PutU64(entry, 0, i * 0x100);
        biwa::test::PutU64(entry, 8, i);
        entries.push_back(std::move(entry));
    }
    return entries;
}

biwa::Result<BucketTree> Read(const std::vector<std::byte>& tree, std::size_t count, std::uint64_t at = 0) {
    std::vector<std::byte> storage(at);
    storage.insert(storage.end(), tree.begin(), tree.end());
    const auto info = biwa::test::BucketTreeInfoBytes(at, tree.size(), count);
    const auto parsed = biwa::nx::ParseBucketTreeInfo(info, "test tree");
    EXPECT_TRUE(parsed.has_value());
    return biwa::nx::ReadBucketTree(*biwa::nx::MakeMemoryStorage(storage), *parsed, kEntrySize, "test tree");
}

TEST(BucketTreeTest, ReadsEveryEntrySetAndFindsRanges) {
    constexpr std::size_t kCount = 2500;  // three entry sets of up to 1023 entries
    const auto tree = Read(biwa::test::BuildBucketTree(Entries(kCount), kEntrySize, kCount * 0x100), kCount, 0x200);
    ASSERT_TRUE(tree.has_value()) << tree.error();
    ASSERT_EQ(tree->count(), kCount);
    EXPECT_EQ(tree->end_offset, kCount * 0x100);
    EXPECT_EQ(tree->Offset(2000), 2000u * 0x100);
    EXPECT_EQ(tree->End(kCount - 1), kCount * 0x100);
    EXPECT_EQ(tree->Find(0), 0u);
    EXPECT_EQ(tree->Find(0xFF), 0u);
    EXPECT_EQ(tree->Find(1023 * 0x100 + 5), 1023u);
    EXPECT_EQ(tree->Find(kCount * 0x100 - 1), kCount - 1);
    EXPECT_EQ(tree->Find(kCount * 0x100), kCount);  // past the end
}

TEST(BucketTreeTest, AnEmptyTreeHasNoEntries) {
    const auto tree = Read({}, 0);
    ASSERT_TRUE(tree.has_value());
    EXPECT_EQ(tree->count(), 0u);
    EXPECT_EQ(tree->Find(0), 0u);
}

TEST(BucketTreeTest, RejectsBadTrees) {
    const std::vector<std::byte> good = biwa::test::BuildBucketTree(Entries(1500), kEntrySize, 0x5DC00);
    EXPECT_EQ(Read(good, 1501).error().code(), Errc::kCorrupt);  // more entries than the sets hold
    EXPECT_EQ(Read(good, 3000).error().code(), Errc::kCorrupt);  // the nodes do not fit

    std::vector<std::byte> bad = good;
    biwa::test::PutU32(bad, 0x8000, 7);  // the second entry set's index
    EXPECT_EQ(Read(bad, 1500).error().code(), Errc::kCorrupt);

    bad = good;
    biwa::test::PutU64(bad, 0x4000 + 0x10 + 5 * kEntrySize, 0x50);  // entry 5 before entry 4
    EXPECT_EQ(Read(bad, 1500).error().code(), Errc::kCorrupt);

    bad = good;
    biwa::test::PutU64(bad, 8, 0x100);  // end offset before the last entry
    EXPECT_EQ(Read(bad, 1500).error().code(), Errc::kCorrupt);

    auto info = biwa::test::BucketTreeInfoBytes(0, 0x4000, 1);
    info[0x10] = std::byte{0};
    EXPECT_EQ(biwa::nx::ParseBucketTreeInfo(info, "x").error().code(), Errc::kCorrupt);
}

}  // namespace
