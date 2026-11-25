// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/storage.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "biwa/core/file.h"
#include "support/temp_dir.h"

namespace {

using biwa::Errc;
using biwa::nx::StoragePtr;

std::vector<std::byte> Counting(std::size_t size) {
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>(i * 7 + (i >> 8));
    }
    return bytes;
}

TEST(StorageTest, RangeFitsHandlesEdgesAndOverflow) {
    using biwa::nx::RangeFits;
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    EXPECT_TRUE(RangeFits(0, 10, 10));
    EXPECT_TRUE(RangeFits(10, 0, 10));
    EXPECT_FALSE(RangeFits(11, 0, 10));
    EXPECT_FALSE(RangeFits(5, 6, 10));
    EXPECT_FALSE(RangeFits(kMax, 2, kMax));
    EXPECT_FALSE(RangeFits(2, kMax, kMax));
}

TEST(StorageTest, MemoryReadsAndRejectsPastTheEnd) {
    const std::vector<std::byte> data = Counting(100);
    const StoragePtr storage = biwa::nx::MakeMemoryStorage(data);
    EXPECT_EQ(storage->Size(), 100u);
    std::vector<std::byte> out(10);
    ASSERT_TRUE(storage->Read(90, out).has_value());
    EXPECT_TRUE(std::equal(out.begin(), out.end(), data.begin() + 90));
    const auto past = storage->Read(91, out);
    ASSERT_FALSE(past.has_value());
    EXPECT_EQ(past.error().code(), Errc::kCorrupt);
}

TEST(StorageTest, SubStorageIsAWindow) {
    const std::vector<std::byte> data = Counting(1000);
    const StoragePtr base = biwa::nx::MakeMemoryStorage(data);
    const auto sub = biwa::nx::MakeSubStorage(base, 100, 200, "part");
    ASSERT_TRUE(sub.has_value()) << sub.error();
    EXPECT_EQ((*sub)->Size(), 200u);
    const auto inner = biwa::nx::MakeSubStorage(*sub, 50, 10, "inner");
    ASSERT_TRUE(inner.has_value());
    std::vector<std::byte> out(10);
    ASSERT_TRUE((*inner)->Read(0, out).has_value());
    EXPECT_TRUE(std::equal(out.begin(), out.end(), data.begin() + 150));
    EXPECT_FALSE((*sub)->Read(195, out).has_value());  // inside the base, outside the window
}

TEST(StorageTest, SubStorageMustFit) {
    const StoragePtr base = biwa::nx::MakeMemoryStorage(Counting(1000));
    const auto bad = biwa::nx::MakeSubStorage(base, 900, 101, "section 1");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code(), Errc::kCorrupt);
    EXPECT_EQ(bad.error().message(), "section 1 (0x65 bytes at 0x384) does not fit in its container (size 0x3e8)");
    EXPECT_FALSE(biwa::nx::MakeSubStorage(base, ~std::uint64_t{0}, 2, "wrap").has_value());
}

TEST(StorageTest, ReadStructReadsLittleEndian) {
    std::vector<std::byte> data = {std::byte{0x50}, std::byte{0x46}, std::byte{0x53}, std::byte{0x30},
                                   std::byte{0x02}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}};
    const StoragePtr storage = biwa::nx::MakeMemoryStorage(data);
    struct Pair {
        std::uint32_t magic;
        std::uint32_t value;
    };
    const auto pair = biwa::nx::ReadStruct<Pair>(*storage, 0);
    ASSERT_TRUE(pair.has_value());
    EXPECT_EQ(pair->magic, 0x30534650u);  // "PFS0"
    EXPECT_EQ(pair->value, 0x102u);
    EXPECT_FALSE(biwa::nx::ReadStruct<Pair>(*storage, 4).has_value());
}

TEST(StorageTest, FileStorageReadsAFile) {
    const biwa::test::TempDir dir;
    const std::vector<std::byte> data = Counting(5000);
    ASSERT_TRUE(biwa::WriteFileAtomic(dir / "f.bin", data).has_value());
    const auto storage = biwa::nx::OpenFileStorage(dir / "f.bin");
    ASSERT_TRUE(storage.has_value()) << storage.error();
    EXPECT_EQ((*storage)->Size(), 5000u);
    std::vector<std::byte> out(100);
    ASSERT_TRUE((*storage)->Read(4900, out).has_value());
    EXPECT_TRUE(std::equal(out.begin(), out.end(), data.begin() + 4900));
    EXPECT_EQ(biwa::nx::OpenFileStorage(dir / "missing").error().code(), Errc::kNotFound);
}

TEST(StorageTest, CopyUsesSeveralThreadsAndOddSizes) {
    const biwa::test::TempDir dir;
    const std::vector<std::byte> data = Counting(100003);  // not a multiple of the piece size
    const StoragePtr from = biwa::nx::MakeMemoryStorage(data);
    auto out = biwa::OutputFile::Create(dir / "copy.bin");
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(biwa::nx::CopyStorage(*from, *out, 7, {.threads = 4, .piece_size = 4096}).has_value());
    ASSERT_TRUE(out->Close().has_value());
    const auto back = biwa::ReadFileBytes(dir / "copy.bin");
    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(back->size(), data.size() + 7);
    EXPECT_TRUE(std::equal(data.begin(), data.end(), back->begin() + 7));
}

TEST(StorageTest, CopyReportsTheFirstError) {
    class Failing final : public biwa::nx::Storage {
       public:
        [[nodiscard]] std::uint64_t Size() const override { return 1 << 20; }
        [[nodiscard]] biwa::Status Read(std::uint64_t offset, std::span<std::byte> out) const override {
            if (offset >= std::uint64_t{4096} * 10) {
                return biwa::Fail(Errc::kCorrupt, "bad block");
            }
            std::ranges::fill(out, std::byte{0});
            return {};
        }
    };
    const biwa::test::TempDir dir;
    auto out = biwa::OutputFile::Create(dir / "copy.bin");
    ASSERT_TRUE(out.has_value());
    const auto status = biwa::nx::CopyStorage(Failing{}, *out, 0, {.threads = 3, .piece_size = 4096});
    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().message(), "bad block");
}

}  // namespace
