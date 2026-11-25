// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/nca.h"

#include <gtest/gtest.h>

#include <vector>

#include "support/nca_builder.h"

namespace {

using biwa::Errc;
using biwa::nx::KeySet;
using biwa::nx::NcaContentType;
using biwa::nx::NcaEncryption;
using biwa::nx::NcaFsType;
using biwa::nx::NcaHashType;
using biwa::test::BuildNca;
using biwa::test::NcaBuildOptions;
using biwa::test::NcaBuildSection;

KeySet TestKeys() {
    auto keys = KeySet::Parse(biwa::test::TestKeysText(biwa::test::TestKey(1), 0), "test");
    EXPECT_TRUE(keys.has_value());
    return *keys;
}

std::vector<std::byte> TwoSectionNca(NcaBuildOptions options = {}) {
    NcaBuildSection exefs;
    exefs.fs_type = NcaFsType::kPartitionFs;
    exefs.hash_type = NcaHashType::kHierarchicalSha256;
    exefs.encryption = NcaEncryption::kAesCtr;
    exefs.counter_upper = 0x0000000200000001;
    exefs.data = std::vector<std::byte>(0x300, std::byte{0x11});
    NcaBuildSection romfs;
    romfs.fs_type = NcaFsType::kRomFs;
    romfs.hash_type = NcaHashType::kHierarchicalIntegrity;
    romfs.encryption = NcaEncryption::kAesCtr;
    romfs.data = std::vector<std::byte>(0x1000, std::byte{0x22});
    return BuildNca(options, {exefs, romfs});
}

biwa::Result<biwa::nx::NcaHeader> Read(std::vector<std::byte> nca, const KeySet& keys = TestKeys()) {
    const auto storage = biwa::nx::MakeMemoryStorage(std::move(nca));
    return biwa::nx::ReadNcaHeader(*storage, keys);
}

TEST(NcaHeaderTest, ReadsTheHeaderAndSections) {
    NcaBuildOptions options;
    options.content_type = NcaContentType::kProgram;
    options.key_generation = 0x12;
    options.program_id = 0x01008CF01BAAC000;
    const auto header = Read(TwoSectionNca(options));
    ASSERT_TRUE(header.has_value()) << header.error();
    EXPECT_EQ(header->content_type, NcaContentType::kProgram);
    EXPECT_EQ(header->program_id, 0x01008CF01BAAC000u);
    EXPECT_EQ(header->key_generation, 0x12);
    EXPECT_EQ(header->MasterKeyRevision(), 0x11u);
    EXPECT_FALSE(header->HasRightsId());
    ASSERT_EQ(header->sections.size(), 2u);

    const biwa::nx::NcaSection& exefs = header->sections[0];
    EXPECT_EQ(exefs.index, 0u);
    EXPECT_EQ(exefs.offset, 0xC00u);
    EXPECT_EQ(exefs.size, 0x400u);
    EXPECT_EQ(exefs.fs_type, NcaFsType::kPartitionFs);
    EXPECT_EQ(exefs.hash_type, NcaHashType::kHierarchicalSha256);
    EXPECT_EQ(exefs.encryption, NcaEncryption::kAesCtr);
    EXPECT_EQ(exefs.counter_upper, 0x0000000200000001u);
    EXPECT_FALSE(exefs.HasPatch());
    EXPECT_FALSE(exefs.HasCompression());

    const biwa::nx::NcaSection* romfs = header->Section(1);
    ASSERT_NE(romfs, nullptr);
    EXPECT_EQ(romfs->offset, 0x1000u);
    EXPECT_EQ(romfs->fs_type, NcaFsType::kRomFs);
    EXPECT_EQ(header->Section(2), nullptr);
}

TEST(NcaHeaderTest, OldKeyGenerationFieldCounts) {
    NcaBuildOptions options;
    options.key_generation = 0;
    const auto header = Read(TwoSectionNca(options));
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->MasterKeyRevision(), 0u);
}

TEST(NcaHeaderTest, WrongHeaderKeyIsABadMagic) {
    biwa::nx::AesXtsKey other = biwa::test::TestHeaderKey();
    other[0] ^= 1;
    const auto header = Read(BuildNca({}, {}, other));
    ASSERT_FALSE(header.has_value());
    EXPECT_EQ(header.error().code(), Errc::kCorrupt);
    EXPECT_NE(header.error().message().find("header_key"), std::string::npos);
}

TEST(NcaHeaderTest, MissingHeaderKey) {
    const auto keys = KeySet::Parse("titlekek_00 = 00000000000000000000000000000000\n", "test");
    ASSERT_TRUE(keys.has_value());
    const auto header = Read(TwoSectionNca(), *keys);
    ASSERT_FALSE(header.has_value());
    EXPECT_EQ(header.error().code(), Errc::kMissingKey);
}

TEST(NcaHeaderTest, Nca2IsUnsupported) {
    NcaBuildOptions options;
    options.magic_digit = '2';
    const auto header = Read(BuildNca(options, {}));
    ASSERT_FALSE(header.has_value());
    EXPECT_EQ(header.error().code(), Errc::kUnsupported);
}

TEST(NcaHeaderTest, TamperedSectionHeaderFailsItsHash) {
    std::vector<std::byte> nca = TwoSectionNca();
    // Decrypt, change one byte of section 1's header, encrypt again.
    std::span<std::byte> header = std::span(nca).first(biwa::nx::kNcaHeaderSize);
    ASSERT_TRUE(biwa::nx::AesXtsDecrypt(biwa::test::TestHeaderKey(), header, 0, 0x200).has_value());
    header[0x600 + 0x10] ^= std::byte{1};
    ASSERT_TRUE(biwa::nx::AesXtsEncrypt(biwa::test::TestHeaderKey(), header, 0, 0x200).has_value());
    const auto parsed = Read(nca);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().message(), "NCA section 1 header does not match its hash");
}

TEST(NcaHeaderTest, TruncatedContentIsCorrupt) {
    std::vector<std::byte> nca = TwoSectionNca();
    nca.resize(nca.size() - 0x200);
    const auto header = Read(nca);
    ASSERT_FALSE(header.has_value());
    EXPECT_EQ(header.error().code(), Errc::kCorrupt);
    EXPECT_FALSE(Read(std::vector<std::byte>(0x100)).has_value());
}

TEST(NcaHeaderTest, Names) {
    EXPECT_EQ(biwa::nx::ContentTypeName(NcaContentType::kControl), "control");
    EXPECT_EQ(biwa::nx::FsTypeName(NcaFsType::kPartitionFs), "PFS0");
    EXPECT_EQ(biwa::nx::HashTypeName(NcaHashType::kHierarchicalIntegrity), "ivfc");
    EXPECT_EQ(biwa::nx::EncryptionName(NcaEncryption::kAesCtrEx), "aes-ctr-ex");
}

}  // namespace
