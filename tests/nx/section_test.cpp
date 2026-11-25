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
    exefs.hash_data = biwa::test::Sha256HashData(0x200, pfs0.size());
    exefs.data.resize(0x200);
    exefs.data.insert(exefs.data.end(), pfs0.begin(), pfs0.end());
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
