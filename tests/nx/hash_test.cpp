// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/hash.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "support/nca_builder.h"

namespace {

using biwa::Errc;
using biwa::nx::HashPadding;
using biwa::nx::StoragePtr;

std::vector<std::byte> Counting(std::size_t size) {
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>(i * 11 + (i >> 7));
    }
    return bytes;
}

StoragePtr Checked(const std::vector<std::byte>& data, std::size_t block, HashPadding padding,
                   std::vector<std::byte> hashes) {
    auto storage = biwa::nx::MakeHashCheckedStorage(biwa::nx::MakeMemoryStorage(std::move(hashes)),
                                                    biwa::nx::MakeMemoryStorage(data), block, padding, "test data");
    EXPECT_TRUE(storage.has_value()) << storage.error();
    return storage.value_or(nullptr);
}

TEST(HashTest, ReadsAtAnyOffsetMatchTheData) {
    const std::vector<std::byte> data = Counting(0x1234);  // a short last block
    for (const HashPadding padding : {HashPadding::kNone, HashPadding::kZeros}) {
        const StoragePtr storage =
            Checked(data, 0x100, padding, biwa::test::HashBlocks(data, 0x100, padding == HashPadding::kZeros));
        ASSERT_NE(storage, nullptr);
        for (const auto& [offset, size] : std::vector<std::pair<std::size_t, std::size_t>>{
                 {0, 0x1234}, {0, 0x100}, {0x100, 0x300}, {1, 1}, {0xFF, 2}, {0x1200, 0x34}, {0x1233, 1}}) {
            std::vector<std::byte> out(size);
            ASSERT_TRUE(storage->Read(offset, out).has_value()) << offset;
            EXPECT_TRUE(std::equal(out.begin(), out.end(), data.begin() + static_cast<std::ptrdiff_t>(offset)))
                << offset;
        }
        std::vector<std::byte> past(2);
        EXPECT_EQ(storage->Read(0x1233, past).error().code(), Errc::kCorrupt);
    }
}

TEST(HashTest, PaddingDecidesTheLastBlockHash) {
    const std::vector<std::byte> data = Counting(0x150);
    // Hashes made with zero padding do not match an unpadded check of the short last block.
    const StoragePtr storage = Checked(data, 0x100, HashPadding::kNone, biwa::test::HashBlocks(data, 0x100, true));
    ASSERT_NE(storage, nullptr);
    std::vector<std::byte> out(0x10);
    EXPECT_TRUE(storage->Read(0, out).has_value());
    EXPECT_EQ(storage->Read(0x140, out).error().code(), Errc::kCorrupt);
}

TEST(HashTest, ADamagedBlockFailsOnlyItsReads) {
    const std::vector<std::byte> good = Counting(0x400);
    std::vector<std::byte> damaged = good;
    damaged[0x2A0] ^= std::byte{0x01};
    const StoragePtr storage = Checked(damaged, 0x100, HashPadding::kNone, biwa::test::HashBlocks(good, 0x100, false));
    ASSERT_NE(storage, nullptr);
    std::vector<std::byte> out(0x100);
    EXPECT_TRUE(storage->Read(0x100, out).has_value());
    EXPECT_TRUE(storage->Read(0x300, out).has_value());
    const auto status = storage->Read(0x280, out);  // touches blocks 2 and 3
    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), Errc::kCorrupt);
    EXPECT_NE(status.error().message().find("test data: block 2"), std::string::npos) << status.error();
}

TEST(HashTest, RejectsBadBlockSizesAndShortTables) {
    const StoragePtr data = biwa::nx::MakeMemoryStorage(Counting(0x300));
    const StoragePtr two_hashes = biwa::nx::MakeMemoryStorage(std::vector<std::byte>(64));
    EXPECT_EQ(biwa::nx::MakeHashCheckedStorage(two_hashes, data, 0x100, HashPadding::kNone, "x").error().code(),
              Errc::kCorrupt);
    EXPECT_EQ(biwa::nx::MakeHashCheckedStorage(two_hashes, data, 0, HashPadding::kNone, "x").error().code(),
              Errc::kCorrupt);
    EXPECT_EQ(
        biwa::nx::MakeHashCheckedStorage(two_hashes, data, biwa::nx::kMaxHashBlockSize * 2, HashPadding::kNone, "x")
            .error()
            .code(),
        Errc::kCorrupt);
    EXPECT_TRUE(biwa::nx::MakeHashCheckedStorage(two_hashes, data, 0x180, HashPadding::kNone, "x").has_value());
}

}  // namespace
