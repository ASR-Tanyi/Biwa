// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/compressed.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "support/compressed_builder.h"

namespace {

using biwa::Errc;
using biwa::nx::StoragePtr;

// Blocks of 0x1000: text (compresses), zeros, noise (does not compress), text, then a short tail.
std::vector<std::byte> Plain() {
    std::vector<std::byte> plain;
    auto text = [&] {
        for (int i = 0; i < 0x1000; ++i) {
            plain.push_back(static_cast<std::byte>("compressible "[i % 13]));
        }
    };
    text();
    plain.resize(plain.size() + 0x1000);
    std::uint32_t x = 0x12345678;
    for (int i = 0; i < 0x1000; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        plain.push_back(static_cast<std::byte>(x));
    }
    text();
    for (int i = 0; i < 0x234; ++i) {
        plain.push_back(static_cast<std::byte>(i));
    }
    return plain;
}

StoragePtr Open(const biwa::test::CompressedSection& section) {
    auto storage = biwa::nx::MakeCompressedStorage(biwa::nx::MakeMemoryStorage(section.data), section.compression_info);
    EXPECT_TRUE(storage.has_value()) << storage.error();
    return storage.value_or(nullptr);
}

TEST(CompressedTest, ReadsMatchThePlainBytes) {
    const std::vector<std::byte> plain = Plain();
    const biwa::test::CompressedSection section = biwa::test::BuildCompressedSection(plain);
    EXPECT_EQ(section.types, (std::vector<std::uint8_t>{3, 1, 0, 3, 3}));
    const StoragePtr storage = Open(section);
    ASSERT_NE(storage, nullptr);
    EXPECT_EQ(storage->Size(), plain.size());
    const std::vector<std::pair<std::size_t, std::size_t>> ranges = {
        {0, plain.size()}, {0, 0x1000}, {0x1000, 0x1000}, {0x10, 1}, {0xFFF, 2}, {0x1F00, 0x2200}, {0x4100, 0x134}};
    for (const auto& [offset, size] : ranges) {
        std::vector<std::byte> out(size);
        ASSERT_TRUE(storage->Read(offset, out).has_value()) << offset;
        EXPECT_TRUE(std::equal(out.begin(), out.end(), plain.begin() + static_cast<std::ptrdiff_t>(offset))) << offset;
    }
    std::vector<std::byte> past(2);
    EXPECT_EQ(storage->Read(plain.size() - 1, past).error().code(), Errc::kCorrupt);
}

TEST(CompressedTest, ADamagedBlockFailsItsReads) {
    biwa::test::CompressedSection section = biwa::test::BuildCompressedSection(Plain());
    section.data[0] = std::byte{0xFF};  // the first LZ4 token: a literal run far past the block
    section.data[1] = std::byte{0xFF};
    const StoragePtr storage = Open(section);
    ASSERT_NE(storage, nullptr);
    std::vector<std::byte> out(0x100);
    EXPECT_EQ(storage->Read(0, out).error().code(), Errc::kCorrupt);
    EXPECT_TRUE(storage->Read(0x2000, out).has_value());
}

TEST(CompressedTest, RejectsBadTrees) {
    const biwa::test::CompressedSection good = biwa::test::BuildCompressedSection(Plain());
    auto open = [](const biwa::test::CompressedSection& section) {
        return biwa::nx::MakeCompressedStorage(biwa::nx::MakeMemoryStorage(section.data), section.compression_info);
    };
    biwa::test::CompressedSection bad = good;
    bad.compression_info[0x10] = std::byte{0};  // magic
    EXPECT_EQ(open(bad).error().code(), Errc::kCorrupt);

    bad = good;
    std::vector<std::byte> info(good.compression_info.begin(), good.compression_info.end());
    biwa::test::PutU64(info, 0x08, 0x4000);  // table size too small for its nodes
    std::ranges::copy(info, bad.compression_info.begin());
    EXPECT_EQ(open(bad).error().code(), Errc::kCorrupt);

    bad = good;
    std::size_t table = 0;
    std::memcpy(&table, good.compression_info.data(), sizeof(table));
    const std::size_t third = table + 0x4000 + 0x10 + 0x30;  // the third entry
    bad.data[third + 0x10] = std::byte{2};                   // an unknown compression type
    EXPECT_EQ(open(bad).error().code(), Errc::kUnsupported);

    bad = good;
    biwa::test::PutU64(bad.data, third + 0x08, 0x7FFFFFFF);  // stored past the table
    EXPECT_EQ(open(bad).error().code(), Errc::kCorrupt);
}

}  // namespace
