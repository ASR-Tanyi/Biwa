// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/section.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "biwa/nx/pfs0.h"
#include "biwa/nx/ticket.h"
#include "support/nca_builder.h"

namespace {

using biwa::Errc;
using biwa::nx::KeySet;
using biwa::nx::NcaEncryption;
using biwa::nx::NcaFsType;
using biwa::nx::NcaHashType;
using biwa::test::Bytes;

KeySet Keys() {
    auto keys = KeySet::Parse(biwa::test::TestKeysText(biwa::test::TestKey(1), 0), "test");
    EXPECT_TRUE(keys.has_value());
    return *keys;
}

std::string ReadAll(const biwa::nx::Storage& storage) {
    std::vector<std::byte> bytes(static_cast<std::size_t>(storage.Size()));
    EXPECT_TRUE(storage.Read(0, bytes).has_value());
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// A program NCA whose section 0 is an encrypted ExeFS: a hash table area, then a PFS0.
biwa::nx::StoragePtr ExefsNca() {
    const std::vector<std::byte> pfs0 =
        biwa::test::BuildPfs0({{"main", Bytes("main module bytes")}, {"main.npdm", Bytes("npdm")}});
    biwa::test::NcaBuildSection exefs;
    exefs.fs_type = NcaFsType::kPartitionFs;
    exefs.hash_type = NcaHashType::kHierarchicalSha256;
    exefs.encryption = NcaEncryption::kAesCtr;
    exefs.counter_upper = 0x0000000300000000;
    const biwa::test::HashedSection hashed = biwa::test::BuildSha256Section(pfs0, 0x40);
    exefs.hash_data = hashed.hash_data;
    exefs.data = hashed.data;
    biwa::test::NcaBuildOptions options;
    options.key_area_key = biwa::test::TestKey(1);
    options.key_area[2] = biwa::test::TestKey(7);
    return biwa::nx::MakeMemoryStorage(biwa::test::BuildNca(options, {exefs}));
}

TEST(SectionTest, OpensAnEncryptedExefs) {
    const biwa::nx::StoragePtr nca = ExefsNca();
    const auto header = biwa::nx::ReadNcaHeader(*nca, Keys());
    ASSERT_TRUE(header.has_value()) << header.error();
    const auto key = biwa::nx::NcaCtrKey(*header, Keys(), nullptr);
    ASSERT_TRUE(key.has_value());
    const auto section = biwa::nx::OpenNcaSection(nca, header->sections[0], *key);
    ASSERT_TRUE(section.has_value()) << section.error();
    const auto data = biwa::nx::OpenSectionData(*section, header->sections[0]);
    ASSERT_TRUE(data.has_value()) << data.error();
    const auto pfs = biwa::nx::Pfs0::Open(*data);
    ASSERT_TRUE(pfs.has_value()) << pfs.error();
    ASSERT_EQ(pfs->entries().size(), 2u);
    const auto main = pfs->OpenEntry(pfs->entries()[0]);
    ASSERT_TRUE(main.has_value());
    EXPECT_EQ(ReadAll(**main), "main module bytes");
}

TEST(SectionTest, CtrReadsAtAnyOffsetMatchThePlaintext) {
    std::vector<std::byte> plain(0x600);
    for (std::size_t i = 0; i < plain.size(); ++i) {
        plain[i] = static_cast<std::byte>(i * 13 + 5);
    }
    constexpr std::uint64_t kBase = 0x4000;  // where the bytes would sit in the NCA
    constexpr std::uint64_t kUpper = 0x0102030405060708;
    std::vector<std::byte> cipher = plain;
    biwa::nx::AesCtrCrypt(biwa::test::TestKey(3), biwa::nx::NcaCounter(kUpper, kBase), cipher);
    const biwa::nx::StoragePtr ctr =
        biwa::nx::MakeAesCtrStorage(biwa::nx::MakeMemoryStorage(cipher), biwa::test::TestKey(3), kUpper, kBase);
    for (const auto& [offset, size] : std::vector<std::pair<std::size_t, std::size_t>>{
             {0, 0x600}, {1, 1}, {15, 2}, {0x1F3, 0x31}, {0x5FF, 1}, {0x100, 0x200}}) {
        std::vector<std::byte> out(size);
        ASSERT_TRUE(ctr->Read(offset, out).has_value()) << offset;
        EXPECT_TRUE(std::equal(out.begin(), out.end(), plain.begin() + static_cast<std::ptrdiff_t>(offset))) << offset;
    }
    std::vector<std::byte> past(2);
    EXPECT_EQ(ctr->Read(0x5FF, past).error().code(), Errc::kCorrupt);
}

TEST(SectionTest, FindsIvfcDataLevel) {
    biwa::nx::NcaSection section;
    section.size = 0x100000;
    section.hash_type = NcaHashType::kHierarchicalIntegrity;
    std::vector<std::byte> hash(0xF8);
    biwa::test::PutU32(hash, 0, 0x43465649);  // "IVFC"
    biwa::test::PutU32(hash, 0x0C, 7);
    biwa::test::PutU64(hash, 0x10 + 5 * 0x18, 0x8000);
    biwa::test::PutU64(hash, 0x10 + 5 * 0x18 + 8, 0x20000);
    std::ranges::copy(hash, section.hash_data.begin());
    const auto region = biwa::nx::FindSectionData(section);
    ASSERT_TRUE(region.has_value()) << region.error();
    EXPECT_EQ(region->offset, 0x8000u);
    EXPECT_EQ(region->size, 0x20000u);

    biwa::test::PutU64(hash, 0x10 + 5 * 0x18 + 8, 0x200000);  // past the section
    std::ranges::copy(hash, section.hash_data.begin());
    EXPECT_EQ(biwa::nx::FindSectionData(section).error().code(), Errc::kCorrupt);
}

// A program NCA with one AES-CTR section built from `hashed`.
std::vector<std::byte> NcaWith(const biwa::test::HashedSection& hashed, NcaFsType fs_type, NcaHashType hash_type) {
    biwa::test::NcaBuildSection section;
    section.fs_type = fs_type;
    section.hash_type = hash_type;
    section.encryption = NcaEncryption::kAesCtr;
    section.counter_upper = 0x0000000500000000;
    section.hash_data = hashed.hash_data;
    section.data = hashed.data;
    biwa::test::NcaBuildOptions options;
    options.key_area_key = biwa::test::TestKey(1);
    options.key_area[2] = biwa::test::TestKey(7);
    return biwa::test::BuildNca(options, {section});
}

biwa::Result<biwa::nx::StoragePtr> OpenData(const std::vector<std::byte>& bytes,
                                            biwa::nx::HashCheck check = biwa::nx::HashCheck::kOn) {
    const biwa::nx::StoragePtr nca = biwa::nx::MakeMemoryStorage(bytes);
    const auto header = biwa::nx::ReadNcaHeader(*nca, Keys());
    EXPECT_TRUE(header.has_value()) << header.error();
    const auto section = biwa::nx::OpenNcaSection(nca, header->sections[0], biwa::test::TestKey(7));
    EXPECT_TRUE(section.has_value()) << section.error();
    return biwa::nx::OpenSectionData(*section, header->sections[0], check);
}

TEST(SectionTest, ChecksIvfcSections) {
    std::vector<std::byte> payload(0x1500);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(i * 29 + 3);
    }
    const biwa::test::HashedSection hashed = biwa::test::BuildIvfcSection(payload);
    const std::vector<std::byte> good = NcaWith(hashed, NcaFsType::kRomFs, NcaHashType::kHierarchicalIntegrity);
    const auto data = OpenData(good);
    ASSERT_TRUE(data.has_value()) << data.error();
    std::vector<std::byte> all(payload.size());
    ASSERT_TRUE((*data)->Read(0, all).has_value());
    EXPECT_EQ(all, payload);

    // Damage in the data level fails reads of that block only (AES-CTR flips the same plaintext bit).
    std::vector<std::byte> damaged = good;
    damaged[biwa::nx::kNcaHeaderSize + hashed.payload_offset + 0x345] ^= std::byte{0x80};
    const auto opened = OpenData(damaged);
    ASSERT_TRUE(opened.has_value()) << opened.error();
    std::vector<std::byte> block(0x200);
    EXPECT_TRUE((*opened)->Read(0, block).has_value());
    const auto status = (*opened)->Read(0, all);
    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), Errc::kCorrupt);
    EXPECT_NE(status.error().message().find("section 0 data: block 1"), std::string::npos) << status.error();
    EXPECT_TRUE((*OpenData(damaged, biwa::nx::HashCheck::kOff))->Read(0, all).has_value());

    // Damage in level 0 fails against the master hash when the section is opened.
    damaged = good;
    damaged[biwa::nx::kNcaHeaderSize + 3] ^= std::byte{0x01};
    const auto top = OpenData(damaged);
    ASSERT_FALSE(top.has_value());
    EXPECT_EQ(top.error().code(), Errc::kCorrupt);
    EXPECT_NE(top.error().message().find("section 0 hash level 0"), std::string::npos) << top.error();
}

TEST(SectionTest, ChecksSha256Sections) {
    const std::vector<std::byte> pfs0 = biwa::test::BuildPfs0({{"main", Bytes("some code")}});
    const biwa::test::HashedSection hashed = biwa::test::BuildSha256Section(pfs0, 0x20);
    const std::vector<std::byte> good = NcaWith(hashed, NcaFsType::kPartitionFs, NcaHashType::kHierarchicalSha256);
    ASSERT_TRUE(OpenData(good).has_value());

    std::vector<std::byte> damaged = good;
    damaged[biwa::nx::kNcaHeaderSize + hashed.payload_offset + 0x21] ^= std::byte{0x01};
    const auto data = OpenData(damaged);
    ASSERT_TRUE(data.has_value()) << data.error();
    std::vector<std::byte> out(0x10);
    EXPECT_TRUE((*data)->Read(0, out).has_value());
    EXPECT_EQ((*data)->Read(0x20, out).error().code(), Errc::kCorrupt);

    damaged = good;
    damaged[biwa::nx::kNcaHeaderSize + 1] ^= std::byte{0x01};  // the hash table
    EXPECT_EQ(OpenData(damaged).error().code(), Errc::kCorrupt);
    EXPECT_TRUE(OpenData(damaged, biwa::nx::HashCheck::kOff).has_value());
}

TEST(SectionTest, UnsupportedLayersSaySo) {
    biwa::test::NcaBuildSection compressed;
    compressed.data.resize(0x200);
    compressed.compression_info[0x08] = std::byte{0x10};  // table size
    biwa::test::NcaBuildSection patch;
    patch.data.resize(0x200);
    patch.patch_info[0x08] = std::byte{0x10};  // indirect table size
    const auto nca = biwa::nx::MakeMemoryStorage(biwa::test::BuildNca({}, {compressed, patch}));
    const auto header = biwa::nx::ReadNcaHeader(*nca, Keys());
    ASSERT_TRUE(header.has_value()) << header.error();
    const auto first = biwa::nx::OpenNcaSection(nca, header->sections[0], std::nullopt);
    ASSERT_FALSE(first.has_value());
    EXPECT_EQ(first.error().code(), Errc::kUnsupported);
    EXPECT_NE(first.error().message().find("compressed"), std::string::npos);
    EXPECT_EQ(biwa::nx::OpenNcaSection(nca, header->sections[1], std::nullopt).error().code(), Errc::kUnsupported);
}

}  // namespace
