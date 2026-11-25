// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/pfs0.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "support/pfs0_builder.h"

namespace {

using biwa::Errc;
using biwa::nx::Pfs0;
using biwa::test::Bytes;
using biwa::test::PutU32;
using biwa::test::PutU64;

std::string ReadAll(const biwa::nx::Storage& storage) {
    std::vector<std::byte> bytes(static_cast<std::size_t>(storage.Size()));
    EXPECT_TRUE(storage.Read(0, bytes).has_value());
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

biwa::Result<Pfs0> Open(std::vector<std::byte> bytes) {
    return Pfs0::Open(biwa::nx::MakeMemoryStorage(std::move(bytes)));
}

TEST(Pfs0Test, ListsAndOpensFiles) {
    const auto pfs = Open(biwa::test::BuildPfs0({{"main", Bytes("code")},
                                                 {"main.npdm", Bytes("meta")},
                                                 {"0123456789abcdef0123456789abcdef.nca", Bytes("content!")}}));
    ASSERT_TRUE(pfs.has_value()) << pfs.error();
    ASSERT_EQ(pfs->entries().size(), 3u);
    EXPECT_EQ(pfs->entries()[1].name, "main.npdm");
    EXPECT_EQ(pfs->entries()[2].size, 8u);

    const biwa::nx::Pfs0Entry* nca = pfs->Find("0123456789abcdef0123456789abcdef.nca");
    ASSERT_NE(nca, nullptr);
    const auto storage = pfs->OpenEntry(*nca);
    ASSERT_TRUE(storage.has_value());
    EXPECT_EQ(ReadAll(**storage), "content!");
    EXPECT_EQ(pfs->Find("missing"), nullptr);
}

TEST(Pfs0Test, EmptyContainerIsValid) {
    const auto pfs = Open(biwa::test::BuildPfs0({}));
    ASSERT_TRUE(pfs.has_value()) << pfs.error();
    EXPECT_TRUE(pfs->entries().empty());
}

TEST(Pfs0Test, WrongMagicIsNotAPfs0) {
    std::vector<std::byte> bytes = biwa::test::BuildPfs0({{"a", Bytes("x")}});
    PutU32(bytes, 0, 0x30534648);  // "HFS0"
    const auto pfs = Open(bytes);
    ASSERT_FALSE(pfs.has_value());
    EXPECT_EQ(pfs.error().code(), Errc::kInvalidArgument);
    EXPECT_EQ(pfs.error().message(), "not a PFS0 container (magic 0x30534648)");
}

TEST(Pfs0Test, TruncatedHeaderIsCorrupt) {
    const auto pfs = Open(Bytes("PFS0"));
    ASSERT_FALSE(pfs.has_value());
    EXPECT_EQ(pfs.error().code(), Errc::kCorrupt);
}

TEST(Pfs0Test, HugeFileCountIsCorruptWithoutAllocating) {
    std::vector<std::byte> bytes = biwa::test::BuildPfs0({{"a", Bytes("x")}});
    PutU32(bytes, 4, 0xFFFFFFFF);
    const auto pfs = Open(bytes);
    ASSERT_FALSE(pfs.has_value());
    EXPECT_EQ(pfs.error().code(), Errc::kCorrupt);
}

TEST(Pfs0Test, FilePastTheEndIsCorrupt) {
    std::vector<std::byte> bytes = biwa::test::BuildPfs0({{"a", Bytes("abc")}});
    PutU64(bytes, 0x10 + 8, 4);  // size 4, but only 3 bytes of data
    const auto pfs = Open(bytes);
    ASSERT_FALSE(pfs.has_value());
    EXPECT_EQ(pfs.error().code(), Errc::kCorrupt);
    EXPECT_EQ(pfs.error().message(), "PFS0 file a (0x4 bytes at 0x0) ends past the container");
}

TEST(Pfs0Test, BadNamesAreCorrupt) {
    std::vector<std::byte> outside = biwa::test::BuildPfs0({{"a", Bytes("x")}});
    PutU32(outside, 0x10 + 16, 0x1000);
    EXPECT_EQ(Open(outside).error().code(), Errc::kCorrupt);

    // A string table with no terminating NUL: one entry, string table "ab", no padding.
    std::vector<std::byte> unterminated(0x10 + 0x18 + 2);
    PutU32(unterminated, 0, 0x30534650);
    PutU32(unterminated, 4, 1);
    PutU32(unterminated, 8, 2);
    unterminated[0x28] = std::byte{'a'};
    unterminated[0x29] = std::byte{'b'};
    const auto r = Open(unterminated);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().message(), "PFS0 entry 0: name is not terminated");
}

TEST(Pfs0Test, DuplicateNamesAreCorrupt) {
    const auto pfs = Open(biwa::test::BuildPfs0({{"same", Bytes("1")}, {"same", Bytes("2")}}));
    ASSERT_FALSE(pfs.has_value());
    EXPECT_EQ(pfs.error().message(), "PFS0 has two files named same");
}

}  // namespace
