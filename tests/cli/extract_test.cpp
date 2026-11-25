// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include <gtest/gtest.h>

#include <string>

#include "biwa/core/file.h"
#include "biwa/core/hex.h"
#include "biwa/core/json.h"
#include "support/cli_runner.h"
#include "support/game_builder.h"
#include "support/temp_dir.h"

namespace {

using biwa::test::PathText;
using biwa::test::RunBiwa;

constexpr std::uint64_t kTitle = 0x0100000000004000;

// A base game and its update on disk, with keys for both.
struct Game {
    biwa::test::TempDir dir;
    std::string base;
    std::string update;
    std::string keys;
    std::string work;
    Game() {
        biwa::test::TitleBuild base_title;
        base_title.program_sections = {biwa::test::ExefsSection(0x11, kTitle)};
        biwa::test::TitleBuild update_title;
        update_title.meta_type = 0x81;
        update_title.title_id = kTitle + 0x800;
        update_title.version = 0x10000;
        update_title.related = kTitle;
        update_title.id_seed = 0x20;
        update_title.program_sections = {biwa::test::ExefsSection(0x22, kTitle)};
        EXPECT_TRUE(biwa::WriteFileAtomic(dir / "base.nsp", biwa::test::BuildTitleNsp(base_title)));
        EXPECT_TRUE(biwa::WriteFileAtomic(dir / "update.nsp", biwa::test::BuildTitleNsp(update_title)));
        const std::string text = biwa::test::TestKeysText(biwa::test::TestKey(1), 0);
        EXPECT_TRUE(biwa::WriteFileAtomic(dir / "prod.keys", std::as_bytes(std::span(text))));
        base = PathText(dir / "base.nsp");
        update = PathText(dir / "update.nsp");
        keys = PathText(dir / "prod.keys");
        work = PathText(dir / "work");
    }
};

std::string BuildIdOf(std::uint8_t seed) {
    std::array<std::uint8_t, 0x20> id{};
    for (std::size_t i = 0; i < id.size(); ++i) {
        id[i] = static_cast<std::uint8_t>(seed + i);
    }
    return biwa::ToHex(id);
}

TEST(CliExtractTest, WritesTheExefsModulesAndManifest) {
    const Game game;
    const auto r = RunBiwa({"extract", "-k", game.keys, "-o", game.work, game.base});
    ASSERT_EQ(r.code, biwa::cli::kExitOk) << r.err;
    const std::filesystem::path work(game.work);
    for (const char* file : {"exefs/main", "exefs/main.npdm", "exefs/rtld", "modules/main.bin", "modules/rtld.json"}) {
        EXPECT_TRUE(std::filesystem::exists(work / file)) << file;
    }
    const auto main = biwa::ReadJson(work / "modules/main.json");
    ASSERT_TRUE(main.has_value()) << main.error();
    EXPECT_EQ((*main)["build_id"], BuildIdOf(0x11));
    EXPECT_EQ((*main)["image_size"], 0x2100);
    EXPECT_EQ((*main)["mod0"]["bss_end"], 0x2800);
    EXPECT_EQ((*main)["segments"]["rodata"]["memory_offset"], 0x1000);

    const auto npdm = biwa::ReadJson(work / "npdm.json");
    ASSERT_TRUE(npdm.has_value()) << npdm.error();
    EXPECT_EQ((*npdm)["program_id"], "0100000000004000");
    EXPECT_EQ((*npdm)["main_thread"]["priority"], 44);
    EXPECT_EQ((*npdm)["thread_priorities"]["lowest"], 59);

    const auto manifest = biwa::ReadJson(work / "manifest.json");
    ASSERT_TRUE(manifest.has_value()) << manifest.error();
    EXPECT_EQ((*manifest)["title_id"], "0100000000004000");
    EXPECT_TRUE((*manifest)["update"].is_null());
    EXPECT_EQ((*manifest)["inputs"][0]["file"], "base.nsp");
    EXPECT_EQ((*manifest)["inputs"][0]["type"], "application");
    const auto& outputs = (*manifest)["outputs"];
    const auto bin = std::ranges::find(outputs, "modules/main.bin", [](const auto& o) { return o["path"]; });
    ASSERT_NE(bin, outputs.end());
    const auto bytes = biwa::ReadFileBytes(work / "modules/main.bin");
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ((*bin)["sha256"], biwa::ToHex(biwa::Sha256Of(*bytes)));
}

TEST(CliExtractTest, TheUpdatesCodeReplacesTheBase) {
    const Game game;
    const auto r = RunBiwa({"extract", "-k", game.keys, "-o", game.work, game.update, game.base});
    ASSERT_EQ(r.code, biwa::cli::kExitOk) << r.err;
    const auto main = biwa::ReadJson(std::filesystem::path(game.work) / "modules/main.json");
    ASSERT_TRUE(main.has_value());
    EXPECT_EQ((*main)["build_id"], BuildIdOf(0x22));
    const auto manifest = biwa::ReadJson(std::filesystem::path(game.work) / "manifest.json");
    ASSERT_TRUE(manifest.has_value());
    EXPECT_EQ((*manifest)["update"]["version"], 0x10000);
    EXPECT_EQ((*manifest)["inputs"].size(), 2u);
}

TEST(CliExtractTest, ProblemsHaveExitCodes) {
    const Game game;
    EXPECT_EQ(RunBiwa({"extract", "-k", game.keys, game.base}).code, biwa::cli::kExitUsage);
    EXPECT_EQ(RunBiwa({"extract", "-k"}).code, biwa::cli::kExitUsage);

    auto r = RunBiwa({"extract", "-k", game.keys, "-o", game.work, game.update});
    EXPECT_EQ(r.code, biwa::cli::kExitInput);
    EXPECT_EQ(r.err, "biwa extract: the base game's NSP is needed\n");

    r = RunBiwa({"extract", "-k", game.keys, "-o", game.work, game.base, game.base});
    EXPECT_EQ(r.code, biwa::cli::kExitInput);

    biwa::test::TitleBuild other;
    other.meta_type = 0x81;
    other.title_id = 0x0100000000005800;
    other.related = 0x0100000000005000;
    other.id_seed = 0x30;
    other.program_sections = {biwa::test::ExefsSection(0x33, 0x0100000000005000)};
    ASSERT_TRUE(biwa::WriteFileAtomic(game.dir / "other.nsp", biwa::test::BuildTitleNsp(other)));
    r = RunBiwa({"extract", "-k", game.keys, "-o", game.work, game.base, PathText(game.dir / "other.nsp")});
    EXPECT_EQ(r.code, biwa::cli::kExitInput);
    EXPECT_EQ(r.err, "biwa extract: other.nsp is for 0100000000005000, not for 0100000000004000\n");
}

}  // namespace
