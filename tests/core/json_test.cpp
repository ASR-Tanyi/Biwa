// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/core/json.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <random>
#include <string>

#include "biwa/core/file.h"

namespace {

namespace fs = std::filesystem;
using biwa::Errc;
using biwa::Json;

class JsonTest : public ::testing::Test {
   protected:
    void SetUp() override {
        std::random_device random;
        dir_ = fs::temp_directory_path() / ("biwa-json-test-" + std::to_string(random()));
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    fs::path dir_;
};

TEST_F(JsonTest, WritesInInsertionOrderAndReadsBack) {
    Json manifest;
    manifest["title_id"] = "0100000000000000";
    manifest["version"] = 65536;
    manifest["modules"] = Json::array({"rtld", "main", "sdk"});
    ASSERT_TRUE(biwa::WriteJson(dir_ / "manifest.json", manifest).has_value());

    const auto text = biwa::ReadFileText(dir_ / "manifest.json");
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text,
              "{\n"
              "  \"title_id\": \"0100000000000000\",\n"
              "  \"version\": 65536,\n"
              "  \"modules\": [\n"
              "    \"rtld\",\n"
              "    \"main\",\n"
              "    \"sdk\"\n"
              "  ]\n"
              "}\n");

    const auto back = biwa::ReadJson(dir_ / "manifest.json");
    ASSERT_TRUE(back.has_value()) << back.error();
    EXPECT_EQ(*back, manifest);
}

TEST_F(JsonTest, BadJsonIsCorruptWithAnOffset) {
    const std::string text = "{\"a\": 1,, }";
    ASSERT_TRUE(biwa::WriteFileAtomic(dir_ / "bad.json", std::as_bytes(std::span(text))).has_value());
    const auto r = biwa::ReadJson(dir_ / "bad.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), Errc::kCorrupt);
    EXPECT_NE(r.error().message().find("bad.json is not valid JSON (byte 9)"), std::string::npos) << r.error();
}

TEST_F(JsonTest, MissingFileIsNotFound) {
    EXPECT_EQ(biwa::ReadJson(dir_ / "none.json").error().code(), Errc::kNotFound);
}

TEST_F(JsonTest, InvalidUtf8IsRefused) {
    Json value;
    value["name"] = std::string("\xff\xfe");
    const auto r = biwa::WriteJson(dir_ / "x.json", value);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), Errc::kInvalidArgument);
    EXPECT_FALSE(fs::exists(dir_ / "x.json"));
}

}  // namespace
