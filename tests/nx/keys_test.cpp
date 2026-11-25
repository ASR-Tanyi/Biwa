// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/keys.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "biwa/core/file.h"
#include "support/temp_dir.h"

namespace {

using biwa::Errc;
using biwa::nx::KeySet;

// Made-up values: these are not real keys.
constexpr std::string_view kText =
    "; test keys\n"
    "header_key = 00112233445566778899AABBCCDDEEFF00112233445566778899aabbccddeeff\n"
    "\n"
    "  Key_Area_Key_Application_00 = 000102030405060708090a0b0c0d0e0f  \r\n"
    "titlekek_0f=f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff\n"
    "# comment\n";

TEST(KeysTest, ParsesNamesAndValues) {
    const auto keys = KeySet::Parse(kText, "test.keys");
    ASSERT_TRUE(keys.has_value()) << keys.error();
    EXPECT_EQ(keys->size(), 3u);
    EXPECT_EQ(keys->Names(), (std::vector<std::string>{"header_key", "key_area_key_application_00", "titlekek_0f"}));
    EXPECT_TRUE(keys->Contains("HEADER_KEY"));

    const auto kaek = keys->Get<16>("key_area_key_application_00");
    ASSERT_TRUE(kaek.has_value()) << kaek.error();
    EXPECT_EQ((*kaek)[15], 0x0f);
    const auto header = keys->Get<32>("header_key");
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ((*header)[9], 0x99);
}

TEST(KeysTest, MissingAndWrongSizeKeys) {
    const auto keys = KeySet::Parse(kText, "test.keys");
    ASSERT_TRUE(keys.has_value());
    const auto missing = keys->Get<16>("titlekek_00");
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), Errc::kMissingKey);
    EXPECT_EQ(missing.error().message(), "titlekek_00 is not in the key files");
    const auto wrong = keys->Get<16>("header_key");
    ASSERT_FALSE(wrong.has_value());
    EXPECT_EQ(wrong.error().code(), Errc::kCorrupt);
    EXPECT_EQ(wrong.error().message(), "header_key has 32 bytes, expected 16");
}

TEST(KeysTest, ErrorsNameTheLineButNeverTheValue) {
    const auto no_equals = KeySet::Parse("header_key 0011\n", "prod.keys");
    ASSERT_FALSE(no_equals.has_value());
    EXPECT_EQ(no_equals.error().message(), "prod.keys line 1: expected 'name = value'");

    const auto bad_hex = KeySet::Parse("\nheader_key = 00112233zz\n", "prod.keys");
    ASSERT_FALSE(bad_hex.has_value());
    EXPECT_EQ(bad_hex.error().code(), Errc::kInvalidArgument);
    EXPECT_EQ(bad_hex.error().message(),
              "prod.keys line 2: the value of header_key is not hex (not a hex digit at position 8)");
    EXPECT_EQ(bad_hex.error().message().find("00112233"), std::string::npos);

    EXPECT_FALSE(KeySet::Parse("header_key =\n", "prod.keys").has_value());
    EXPECT_FALSE(KeySet::Parse("bad name = 00\n", "prod.keys").has_value());
}

TEST(KeysTest, LaterLinesAndMergesReplace) {
    auto keys = KeySet::Parse("a = 01\na = 02\n", "x");
    ASSERT_TRUE(keys.has_value());
    EXPECT_EQ((*keys->Get<1>("a"))[0], 0x02);
    const auto title =
        KeySet::Parse("a = 03\n0100000000000000000000000000000a = 00000000000000000000000000000001\n", "y");
    ASSERT_TRUE(title.has_value());
    keys->Merge(*title);
    EXPECT_EQ(keys->size(), 2u);
    EXPECT_EQ((*keys->Get<1>("a"))[0], 0x03);
    EXPECT_TRUE(keys->Get<16>("0100000000000000000000000000000A").has_value());
}

TEST(KeysTest, LoadsAFile) {
    const biwa::test::TempDir dir;
    ASSERT_TRUE(biwa::WriteFileAtomic(dir / "prod.keys", std::as_bytes(std::span(kText))).has_value());
    const auto keys = KeySet::Load(dir / "prod.keys");
    ASSERT_TRUE(keys.has_value()) << keys.error();
    EXPECT_EQ(keys->size(), 3u);
    EXPECT_EQ(KeySet::Load(dir / "missing.keys").error().code(), Errc::kNotFound);
}

TEST(KeysTest, IndexedNames) {
    EXPECT_EQ(biwa::nx::IndexedKeyName("titlekek", 0), "titlekek_00");
    EXPECT_EQ(biwa::nx::IndexedKeyName("key_area_key_application", 0x1f), "key_area_key_application_1f");
}

}  // namespace
