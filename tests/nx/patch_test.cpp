// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/patch.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "biwa/nx/romfs.h"
#include "biwa/nx/section.h"
#include "support/bucket_tree_builder.h"
#include "support/nca_builder.h"
#include "support/patch_builder.h"
#include "support/romfs_builder.h"

namespace {

using biwa::Errc;
using biwa::nx::NcaEncryption;
using biwa::nx::NcaFsType;
using biwa::nx::NcaHashType;
using biwa::test::Bytes;

constexpr std::uint64_t kBaseUpper = 0x0000000200000000;
constexpr std::uint64_t kPatchUpper = 0x0000000200000003;

biwa::nx::KeySet Keys() {
    auto keys = biwa::nx::KeySet::Parse(biwa::test::TestKeysText(biwa::test::TestKey(1), 0), "test");
    EXPECT_TRUE(keys.has_value());
    return *keys;
}

std::string Text(const biwa::nx::Storage& storage) {
    std::string text(static_cast<std::size_t>(storage.Size()), '\0');
    EXPECT_TRUE(storage.Read(0, std::as_writable_bytes(std::span(text))).has_value());
    return text;
}

struct Built {
    biwa::nx::StoragePtr base_nca;
    biwa::nx::StoragePtr patch_nca;
    biwa::test::PatchSection patch;
};

// A base NCA with a RomFS and an update NCA whose patch section changes one file and adds one.
Built Build() {
    std::vector<std::byte> same(0x1800);
    for (std::size_t i = 0; i < same.size(); ++i) {
        same[i] = static_cast<std::byte>(i * 7);
    }
    const auto old_image = biwa::test::BuildRomfs({{"a.txt", Bytes("old text")}, {"same.bin", same}});
    const auto new_image =
        biwa::test::BuildRomfs({{"a.txt", Bytes("new text!")}, {"same.bin", same}, {"added.txt", Bytes("added")}});
    const biwa::test::HashedSection old_hashed = biwa::test::BuildIvfcSection(old_image);
    const biwa::test::HashedSection new_hashed = biwa::test::BuildIvfcSection(new_image);

    biwa::test::NcaBuildSection base;
    base.fs_type = NcaFsType::kRomFs;
    base.hash_type = NcaHashType::kHierarchicalIntegrity;
    base.encryption = NcaEncryption::kAesCtr;
    base.counter_upper = kBaseUpper;
    base.hash_data = old_hashed.hash_data;
    base.data = old_hashed.data;
    biwa::test::NcaBuildOptions base_options;
    base_options.key_area_key = biwa::test::TestKey(1);
    base_options.key_area[2] = biwa::test::TestKey(7);

    Built built;
    built.patch = biwa::test::BuildPatchSection(old_hashed.data, new_hashed.data, biwa::test::TestKey(8), kPatchUpper,
                                                biwa::nx::kNcaHeaderSize);
    biwa::test::NcaBuildSection patch;
    patch.fs_type = NcaFsType::kRomFs;
    patch.hash_type = NcaHashType::kHierarchicalIntegrity;
    patch.encryption = NcaEncryption::kAesCtrEx;
    patch.counter_upper = kPatchUpper;
    patch.hash_data = new_hashed.hash_data;
    patch.patch_info = built.patch.patch_info;
    patch.data = built.patch.data;
    biwa::test::NcaBuildOptions patch_options = base_options;
    patch_options.key_area[2] = biwa::test::TestKey(8);

    built.base_nca = biwa::nx::MakeMemoryStorage(biwa::test::BuildNca(base_options, {base}));
    built.patch_nca = biwa::nx::MakeMemoryStorage(biwa::test::BuildNca(patch_options, {patch}));
    return built;
}

biwa::Result<biwa::nx::StoragePtr> OpenPatched(const Built& built) {
    const auto base_header = biwa::nx::ReadNcaHeader(*built.base_nca, Keys());
    const auto patch_header = biwa::nx::ReadNcaHeader(*built.patch_nca, Keys());
    EXPECT_TRUE(base_header.has_value() && patch_header.has_value());
    EXPECT_TRUE(patch_header->sections[0].HasPatch());
    const auto base = biwa::nx::OpenNcaSection(built.base_nca, base_header->sections[0], biwa::test::TestKey(7));
    EXPECT_TRUE(base.has_value());
    const auto patched =
        biwa::nx::OpenPatchedSection(*base, built.patch_nca, patch_header->sections[0], biwa::test::TestKey(8));
    if (!patched) {
        return patched;
    }
    return biwa::nx::OpenSectionData(*patched, patch_header->sections[0]);
}

TEST(PatchTest, LaysTheUpdateOverTheBase) {
    const Built built = Build();
    EXPECT_GT(built.patch.update_bytes, 0x200u);  // both counter ranges are used
    const auto data = OpenPatched(built);
    ASSERT_TRUE(data.has_value()) << data.error();
    const auto romfs = biwa::nx::Romfs::Open(*data);
    ASSERT_TRUE(romfs.has_value()) << romfs.error();
    for (const auto& [path, text] :
         std::vector<std::pair<std::string, std::string>>{{"a.txt", "new text!"}, {"added.txt", "added"}}) {
        const auto file = romfs->OpenFile(path);
        ASSERT_TRUE(file.has_value()) << file.error();
        EXPECT_EQ(Text(**file), text);
    }
    const auto same = romfs->OpenFile("same.bin");
    ASSERT_TRUE(same.has_value()) << same.error();
    EXPECT_EQ(Text(**same)[0x100], static_cast<char>(0x100 * 7));

    // Checked with every hash, the whole patched file system reads back.
    std::vector<std::byte> all(static_cast<std::size_t>((*data)->Size()));
    EXPECT_TRUE((*data)->Read(0, all).has_value());
}

TEST(PatchTest, DamagedUpdateDataFailsTheHashCheck) {
    Built built = Build();
    std::vector<std::byte> bytes(static_cast<std::size_t>(built.patch_nca->Size()));
    ASSERT_TRUE(built.patch_nca->Read(0, bytes).has_value());
    bytes[biwa::nx::kNcaHeaderSize + 0x10] ^= std::byte{0x01};  // in the update data
    built.patch_nca = biwa::nx::MakeMemoryStorage(bytes);
    const auto data = OpenPatched(built);
    if (data) {
        std::vector<std::byte> all(static_cast<std::size_t>((*data)->Size()));
        EXPECT_EQ((*data)->Read(0, all).error().code(), Errc::kCorrupt);
    } else {
        EXPECT_EQ(data.error().code(), Errc::kCorrupt);
    }
}

TEST(PatchTest, RejectsBadRelocations) {
    const auto base = biwa::nx::MakeMemoryStorage(std::vector<std::byte>(0x1000));
    const auto patch = biwa::nx::MakeMemoryStorage(std::vector<std::byte>(0x100));
    auto tree_of = [](std::uint64_t virt, std::uint64_t phys, std::uint32_t index, std::uint64_t end) {
        std::vector<std::byte> entry(0x14);
        biwa::test::PutU64(entry, 0, virt);
        biwa::test::PutU64(entry, 8, phys);
        biwa::test::PutU32(entry, 0x10, index);
        const auto bytes = biwa::test::BuildBucketTree({entry}, 0x14, end);
        const auto info = biwa::test::BucketTreeInfoBytes(0, bytes.size(), 1);
        return biwa::nx::ReadBucketTree(*biwa::nx::MakeMemoryStorage(bytes), *biwa::nx::ParseBucketTreeInfo(info, "t"),
                                        0x14, "t");
    };
    EXPECT_TRUE(biwa::nx::MakeIndirectStorage(base, patch, *tree_of(0, 0, 0, 0x1000)).has_value());
    EXPECT_EQ(biwa::nx::MakeIndirectStorage(base, patch, *tree_of(0, 0, 1, 0x1000)).error().code(),
              Errc::kCorrupt);  // past the patch data
    EXPECT_EQ(biwa::nx::MakeIndirectStorage(base, patch, *tree_of(0, 0, 2, 0x10)).error().code(), Errc::kCorrupt);
    EXPECT_EQ(biwa::nx::MakeIndirectStorage(base, patch, *tree_of(0x10, 0, 0, 0x100)).error().code(), Errc::kCorrupt);
}

}  // namespace
