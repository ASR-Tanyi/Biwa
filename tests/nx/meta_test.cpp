// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

// CNMT and NACP.

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "biwa/nx/cnmt.h"
#include "biwa/nx/nacp.h"
#include "support/meta_builder.h"

namespace {

using biwa::Errc;
using biwa::test::PutU32;
using biwa::test::PutU64;

std::vector<std::byte> BuildCnmt(std::uint8_t type, std::size_t contents) {
    std::vector<biwa::test::CnmtBuildContent> records;
    for (std::size_t i = 0; i < contents; ++i) {
        biwa::test::CnmtBuildContent record;
        record.id[0] = static_cast<std::uint8_t>(0x10 + i);
        record.size = 0x112345678;
        record.type = i == 0 ? 1 : 3;  // program, control
        records.push_back(record);
    }
    return biwa::test::BuildCnmt(0x0100000000003000, 0x30000, type, 0x0100000000003800, records);
}

TEST(CnmtTest, ReadsAnApplication) {
    const auto cnmt = biwa::nx::ParseCnmt(BuildCnmt(0x80, 2));
    ASSERT_TRUE(cnmt.has_value()) << cnmt.error();
    EXPECT_EQ(cnmt->title_id, 0x0100000000003000u);
    EXPECT_EQ(cnmt->version, 0x30000u);
    EXPECT_EQ(cnmt->type, biwa::nx::ContentMetaType::kApplication);
    EXPECT_EQ(cnmt->related_title_id, 0x0100000000003800u);  // its patch
    EXPECT_EQ(cnmt->required_system_version, 0x0C000000u);
    EXPECT_EQ(cnmt->required_application_version, 0x10000u);
    ASSERT_EQ(cnmt->contents.size(), 2u);
    EXPECT_EQ(cnmt->contents[0].type, biwa::nx::ContentType::kProgram);
    EXPECT_EQ(cnmt->contents[1].type, biwa::nx::ContentType::kControl);
    EXPECT_EQ(cnmt->contents[0].size, 0x112345678u);
    EXPECT_EQ(cnmt->contents[1].id[0], 0x11);
    EXPECT_EQ(cnmt->contents[0].hash[0], 0xAA);
    EXPECT_EQ(biwa::nx::ContentMetaTypeName(cnmt->type), "application");
    EXPECT_EQ(biwa::nx::ContentTypeName(cnmt->contents[1].type), "control");
}

TEST(CnmtTest, ReadsAPatchAndRejectsShortRecords) {
    const auto patch = biwa::nx::ParseCnmt(BuildCnmt(0x81, 1));
    ASSERT_TRUE(patch.has_value()) << patch.error();
    EXPECT_EQ(patch->related_title_id, 0x0100000000003800u);  // the application, in a real patch
    EXPECT_EQ(patch->required_application_version, 0u);

    std::vector<std::byte> bytes = BuildCnmt(0x80, 2);
    bytes[0x10] = std::byte{9};  // more records than the file holds
    EXPECT_EQ(biwa::nx::ParseCnmt(bytes).error().code(), Errc::kCorrupt);
    EXPECT_EQ(biwa::nx::ParseCnmt(std::vector<std::byte>(0x10)).error().code(), Errc::kCorrupt);
}

TEST(NacpTest, ReadsTitlesAndSaveSizes) {
    std::vector<std::byte> bytes(biwa::nx::kNacpSize);
    std::memcpy(bytes.data(), "Sample Game", 11);
    std::memcpy(bytes.data() + 0x200, "Sample Publisher", 16);
    std::memcpy(bytes.data() + 0x600, "サンプル", std::strlen("サンプル"));  // Japanese (the third title)
    PutU32(bytes, 0x302C, 0b101);
    std::memcpy(bytes.data() + 0x3060, "1.2.3", 5);
    PutU64(bytes, 0x3078, 0x0100000000003000);
    PutU64(bytes, 0x3080, 0x4000000);
    PutU64(bytes, 0x3088, 0x1000000);
    PutU64(bytes, 0x3170, 0x2000000);
    const auto nacp = biwa::nx::ParseNacp(bytes);
    ASSERT_TRUE(nacp.has_value()) << nacp.error();
    ASSERT_EQ(nacp->titles.size(), 2u);
    EXPECT_EQ(nacp->titles[0].language, "AmericanEnglish");
    EXPECT_EQ(nacp->titles[0].name, "Sample Game");
    EXPECT_EQ(nacp->titles[0].publisher, "Sample Publisher");
    EXPECT_EQ(nacp->titles[1].language, "Japanese");
    EXPECT_EQ(nacp->titles[1].name, "サンプル");
    EXPECT_EQ(nacp->display_version, "1.2.3");
    EXPECT_EQ(nacp->supported_languages, 0b101u);
    EXPECT_EQ(nacp->save_data_owner_id, 0x0100000000003000u);
    EXPECT_EQ(nacp->user_account_save_data_size, 0x4000000u);
    EXPECT_EQ(nacp->user_account_save_data_journal_size, 0x1000000u);
    EXPECT_EQ(nacp->cache_storage_size, 0x2000000u);
    EXPECT_FALSE(nacp->titles_compressed);
    EXPECT_EQ(biwa::nx::ParseNacp(std::vector<std::byte>(0x3000)).error().code(), Errc::kCorrupt);

    bytes[0x3215] = std::byte{1};  // titles compressed
    const auto compressed = biwa::nx::ParseNacp(bytes);
    ASSERT_TRUE(compressed.has_value());
    EXPECT_TRUE(compressed->titles_compressed);
    EXPECT_TRUE(compressed->titles.empty());
    EXPECT_EQ(compressed->display_version, "1.2.3");
}

}  // namespace
